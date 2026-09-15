// Normal discontinuity as a gate (issue #596).
//
// WHY THIS FILE EXISTS. #593 shipped a volume whose FEATHER was silently
// dropped. The displacement was correct to four decimals, every parity, golden
// and unit gate stayed green, and a user found it. The reason is structural: a
// gate that compares DISTANCES can only catch parameters whose effect is on
// distances, and the feather does not move the zero set -- it shapes the
// NORMALS across a replace boundary. `bindings/c/clay.h` says so in as many
// words, explaining why a hard replace corrugates: "the zero set is exact, the
// shading is not".
//
// CORRECTED, and the correction is the point. A normal-quantity gate ALREADY
// EXISTED when this was written: `tests/unit/test_c_volume.cpp:793` ("a
// feathered replace uncorrugates the bake round trip", added 2026-08-11 with
// the feather itself) measures the angle between the field gradient and the
// analytic radial direction, and asserts a hard replace tilts past 4 degrees
// while a feathered one converges under 1.2. It even records that "the surface
// itself was never wrong ... the corrugation is all in the normals".
//
// So "every gate here compares distances" was too strong. The true gap is
// narrower and sharper: that gate covers the PLACEMENT -- a feather set on a
// bake, through clay_volume_params -- and no gate covered any VERB that
// REBUILDS a volume. #593 was exactly that: move_topological re-sampled through
// a callable and the feather had nowhere to ride. The bake-path gate could
// never have fired, because it never runs a verb.
//
// This file therefore does two separate things. The first case is the mesh-level
// counterpart of the existing field-level check, which is what makes a bar that
// can hold across backends. The second is the part that was genuinely missing:
// every volume verb must carry the placement contract through.
//
// THE METRIC IS A RATIO, AND THAT IS THE WHOLE DESIGN. An absolute roughness is
// a property of the tessellation, the cell size and the marcher; the ratio
// against the SAME shape through the SAME mesher with the defect absent is a
// property of the defect. `benchmarks/feather_roughness_probe.cpp` calibrated
// it, and which statistic to use was the finding rather than a detail:
//
//   mean         1.05x   -- dilutes; the defect is a seam in 422k edges
//   p99          1.87x   -- separates cleanly, and is what this file uses
//   p99.9, max   1.00x   -- saturated: the control sphere already carries
//                           90-degree edges, so the top of the distribution
//                           belongs to the marcher and not to the defect
//   seam-local   0.80x   -- WORSE than useless. A spatial shell around the
//                           replace box also selects where the surface runs
//                           tangent to the lattice, which is rougher on the
//                           control too, so it reports the defect as an
//                           improvement.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "clay/eval/bake_volume.h"
#include "clay/field/volume.h"
#include "clay/mesh/marching.h"
#include "clay/field/flatten.h"
#include "clay/field/move_topological.h"
#include "clay/field/relax.h"
#include "clay/scene/tape.h"
#include "clay/scene/document.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using mesh::Mesh;

namespace {

constexpr float kRadius = 1.0f;
constexpr float kCell = 0.02f;
constexpr float kBand = 0.06f;  // three cells, which is what a <= 0 request picks

math::Aabb whole() { return math::Aabb{cf3(-1.4f, -1.4f, -1.4f), cf3(1.4f, 1.4f, 1.4f)}; }

// The region baked and placed back. It has to CROSS material: a box containing
// the whole sphere puts the crossfade out in empty space, where there is no
// surface for it to be seen on.
math::Aabb cap() { return math::Aabb{cf3(-0.6f, -0.6f, 0.0f), cf3(0.6f, 0.6f, 1.3f)}; }

// The 99th percentile of the angle, in degrees, between the two incident face
// normals over interior edges.
//
// `mesh::validate` walks the same edges but keys its map on the vertex pair and
// stores only counts, never the triangle indices, so there is nothing there to
// reuse -- this needs the incident pair itself.
//
// Boundary edges (one incident face) have no angle. Non-manifold edges (more
// than two) have no well-defined pair, and picking two of them arbitrarily
// would make the result depend on triangle order. Both are skipped.
double normal_discontinuity_p99(const Mesh& m) {
    const std::size_t tris = m.triangle_count();
    if (tris == 0) return 0.0;

    std::vector<cfloat3> face(tris);
    for (std::size_t t = 0; t < tris; ++t) {
        const std::uint32_t a = m.indices[t * 3], b = m.indices[t * 3 + 1],
                            c = m.indices[t * 3 + 2];
        cfloat3 n = ccross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
        const float len = clength(n);
        face[t] = len > 0.0f ? n * (1.0f / len) : cf3(0, 0, 0);
    }

    struct Pair { std::uint32_t t0 = 0, t1 = 0; int count = 0; };
    std::unordered_map<std::uint64_t, Pair> edges;
    edges.reserve(tris * 3);
    for (std::size_t t = 0; t < tris; ++t)
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = m.indices[t * 3 + e], b = m.indices[t * 3 + (e + 1) % 3];
            if (a == b) continue;
            const std::uint64_t key = a < b ? (static_cast<std::uint64_t>(a) << 32) | b
                                            : (static_cast<std::uint64_t>(b) << 32) | a;
            Pair& p = edges[key];
            if (p.count == 0) p.t0 = static_cast<std::uint32_t>(t);
            else if (p.count == 1) p.t1 = static_cast<std::uint32_t>(t);
            ++p.count;
        }

    std::vector<double> angles;
    angles.reserve(edges.size());
    for (const auto& [key, p] : edges) {
        if (p.count != 2) continue;
        const float d = std::max(-1.0f, std::min(1.0f, cdot(face[p.t0], face[p.t1])));
        angles.push_back(std::acos(static_cast<double>(d)) * 57.29577951308232);
    }
    if (angles.empty()) return 0.0;
    std::sort(angles.begin(), angles.end());
    return angles[static_cast<std::size_t>(0.99 * static_cast<double>(angles.size() - 1))];
}

scene::Document sphere_doc() {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(kRadius), cf3(0, 0, 0)));
    return doc;
}

Mesh mesh_of(const scene::Document& doc) {
    return mesh::mesh_tape(scene::compile_document(doc), whole(), kCell);
}

// The #67 round trip: bake a region of the sphere and place it back over
// itself with Replace. No verb is applied, so the SHAPE is unchanged by
// construction and any roughness difference is the replace boundary alone.
Mesh round_tripped(float feather) {
    scene::Document doc = sphere_doc();
    const scene::Tape tape = scene::compile_document(doc);
    field::FieldVolume v = field::FieldVolume::sample_blocks(
        eval::document_block_fill(doc, tape), cap(), kCell, kBand);
    REQUIRE(v.brick_count() > 0);
    v.set_feather(feather);

    scene::Node n = item(scene::Prim::volume(), cf3(0, 0, 0), scene::Op::Replace);
    n.volume = std::make_shared<field::FieldVolume>(std::move(v));
    doc.layers[0].sdf->insert(n);
    return mesh_of(doc);
}

}  // namespace

TEST_CASE("shading: a feathered replace does not corrugate the surface it lands on") {
    const Mesh control = mesh_of(sphere_doc());
    REQUIRE(control.triangle_count() > 0);
    const double base = normal_discontinuity_p99(control);
    REQUIRE(base > 0.0);

    // A feather of about one band is what clay.h calls the sweet spot. The
    // round trip is lossless there: the volume was baked FROM this sphere, so
    // crossfading back to it returns the sphere.
    const Mesh soft = round_tripped(kBand);
    REQUIRE(soft.triangle_count() > 0);
    const double ratio = normal_discontinuity_p99(soft) / base;
    CAPTURE(base);
    CAPTURE(ratio);
    CHECK(ratio < 1.30);

    // AND THE CHECK MUST BE ABLE TO FIRE. A bar that nothing can cross is not a
    // gate, and this repository has shipped several -- a parity row that passes
    // vacuously when a backend is absent, a bindings gate that compares source
    // to itself. So the same measurement, on the hard replace the feather
    // exists to cure, has to CROSS the bar: calibrated at 1.87x against 1.00x
    // in benchmarks/feather_roughness_probe.cpp.
    //
    // If this half ever fails while the half above passes, the metric has gone
    // blind and the test above is no longer evidence of anything.
    const Mesh hard = round_tripped(0.0f);
    REQUIRE(hard.triangle_count() > 0);
    const double hard_ratio = normal_discontinuity_p99(hard) / base;
    CAPTURE(hard_ratio);
    CHECK(hard_ratio > 1.30);
}

TEST_CASE("shading: every volume verb carries the placement contract through") {
    // THE GAP #593 ACTUALLY WENT THROUGH. The existing gate at
    // test_c_volume.cpp:793 covers a feather set on a BAKE and never runs a
    // verb, so it could not have fired. A verb that rebuilds a volume rather
    // than copying it drops whatever it does not explicitly carry, and the
    // feather is invisible to every distance comparison once dropped.
    //
    // Enumerated rather than spot-checked: a verb added later is the next #593,
    // and the point of this case is that it fails the day it is added without
    // carrying the feather.
    scene::Document doc = sphere_doc();
    const scene::Tape tape = scene::compile_document(doc);
    field::FieldVolume v = field::FieldVolume::sample_blocks(
        eval::document_block_fill(doc, tape), cap(), kCell, kBand);
    REQUIRE(v.brick_count() > 0);
    v.set_feather(kBand);
    REQUIRE(v.feather() == doctest::Approx(kBand));

    SUBCASE("relax") {
        // Copies (`FieldVolume out = v;`), so it carries the feather without
        // trying. Asserted anyway: the reason it passes is an implementation
        // detail, and a rewrite could take it away silently.
        CHECK(field::relax(v).feather() == doctest::Approx(kBand));
    }

    SUBCASE("flatten") {
        field::FlattenSettings fs;
        fs.plane_point = cf3(0, 0, 0.8f);
        fs.plane_normal = cf3(0, 0, 1);
        fs.centre = cf3(0, 0, 1.0f);
        fs.region_radius = 0.3f;
        CHECK(field::flatten(v, fs).feather() == doctest::Approx(kBand));
    }

    SUBCASE("move_topological") {
        // The one that shipped broken. It REBUILDS through FieldVolume::sample,
        // which takes bounds, cell size and band as arguments and knows nothing
        // about the volume they came from.
        field::TopologicalMoveSettings ms;
        ms.anchor = cf3(0, 0, 1.0f);
        ms.radius = 0.3f;
        ms.displacement = cf3(0, 0, 0.08f);
        CHECK(field::move_topological(v, ms).feather() == doctest::Approx(kBand));

        // And the drag that touches nothing, which re-samples rather than
        // handing back the source and so drops it by the same mechanism.
        ms.displacement = cf3(0, 0, 0);
        CHECK(field::move_topological(v, ms).feather() == doctest::Approx(kBand));
    }
}
