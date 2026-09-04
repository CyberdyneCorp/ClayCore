// A sampled volume is cropped to the region its tape can be asked about
// (scene-model spec, issue #455).
//
// THE DEFECT. A per-brick culled tape carries every item whose influence bound
// touches the brick, and a volume's influence bound is its whole box — so the
// cull that drops 94 of 97 primitives on a worked ball could never drop a
// volume, and every per-brick tape copied the entire payload. Measured before
// the fix: compiling a tape for ONE 8^3 brick emitted 1,243,861 floats so that
// 512 samples could be read, at 0.3166 ms against 0.0056 ms for the 97
// primitives the volume replaced. Meshing with gradient normals compiles one
// tape per brick, so that was paid 6,859 times — 1740 ms, against 80 ms for the
// same mesh with face normals.
//
// WHY CROPPING IS SOUND, AND EXACTLY HOW FAR. `ctape_volume_dist` CLAMPS a
// query onto the sampled box and folds in the distance to it, so a cropped
// volume answers differently OUTSIDE the crop. That is not a new hazard: it is
// the property a culled tape already has, stated in `tape.h` as "band-clamped
// results are bit-identical to the full tape inside the region". So the whole
// of this file is one question — is the cropped tape identical INSIDE — and it
// is asked with exact equality rather than a tolerance, because a crop that
// merely rounds the same answer differently would be a second field.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "clay/field/volume.h"
#include "clay/scene/consolidate.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// The #306 / #455 fixture: a sphere plus 96 additive stamps walked over it.
scene::Document worked_ball(bool coloured = false) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    scene::Node base;
    base.id = l.sdf->reserve_id();
    base.prim = scene::Prim::sphere(1.0f);
    if (coloured) base.color = cf3(0.9f, 0.2f, 0.1f);
    l.sdf->insert(base);
    for (int i = 0; i < 96; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 96.0f;
        const float r = std::sqrt(z * z >= 1.0f ? 0.0f : 1.0f - z * z);
        scene::Node d;
        d.id = l.sdf->reserve_id();
        d.prim = scene::Prim::sphere(0.18f);
        d.xform.position = cf3(r * std::cos(t), r * std::sin(t), z);
        // Distinct colours, so `layer_colors_vary` is true and the bake keeps a
        // colour lattice — a uniformly coloured layer bakes without one and the
        // colour assertion below would be vacuous.
        if (coloured) d.color = cf3(0.1f, 0.4f + 0.005f * static_cast<float>(i), 0.8f);
        l.sdf->insert(d);
    }
    return doc;
}

field::FieldVolume bake(const scene::Document& doc, float cell) {
    scene::ConsolidationParams cp;
    cp.cell_size = cell;
    std::optional<field::FieldVolume> v = scene::bake_layer(doc.layers.front(), cp);
    REQUIRE(v.has_value());
    REQUIRE(!v->empty());
    return std::move(*v);
}

// One layer holding one volume item, optionally placed by a transform, so the
// crop has to be carried into the item's own frame rather than assumed to be
// world-aligned.
scene::Document volume_document(const field::FieldVolume& vol, const math::Transform& xform) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("baked");
    scene::Node n;
    n.id = l.sdf->reserve_id();
    n.prim = scene::Prim::volume();
    n.volume = std::make_shared<const field::FieldVolume>(vol);
    n.xform = xform;
    l.sdf->insert(n);
    return doc;
}

std::vector<cfloat3> points_in(const math::Aabb& box, int side) {
    std::vector<cfloat3> pts;
    const cfloat3 e = box.extent();
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < side; ++k) {
                const float u = static_cast<float>(i) / static_cast<float>(side - 1);
                const float v = static_cast<float>(j) / static_cast<float>(side - 1);
                const float w = static_cast<float>(k) / static_cast<float>(side - 1);
                pts.push_back(cf3(box.min.x + e.x * u, box.min.y + e.y * v, box.min.z + e.z * w));
            }
    return pts;
}

}  // namespace

TEST_CASE("volume cull: a cropped tape answers exactly what the whole one does") {
    const scene::Document prims = worked_ball();
    const field::FieldVolume vol = bake(prims, 0.03f);

    // Identity, and then a placement that rotates, moves and scales the item,
    // because the crop is taken in the item's LOCAL frame and a world-aligned
    // crop would pass the first and fail the second.
    math::Transform turned;
    turned.position = cf3(0.13f, -0.07f, 0.21f);
    turned.rotation = math::Quat::from_axis_angle(cf3(0.3f, 1.0f, 0.2f), 0.7f);
    turned.scale = 1.35f;

    for (const math::Transform& xform : {math::Transform{}, turned}) {
        const scene::Document doc = volume_document(vol, xform);
        const scene::Tape whole = scene::compile_document(doc);
        REQUIRE(!whole.empty());

        // Regions through the form and just off it. WITHIN THE BAND is the
        // whole of the comparison, because that is the whole of what a culled
        // tape has ever promised: `tape.h` says "band-clamped results are
        // bit-identical to the full tape inside the region", and far outside it
        // the cull drops the item altogether and answers FAR. That is
        // pre-existing and correct — the first version of this test compared
        // raw values two and a half units off the surface and failed against
        // 3.4e37, which was the cull working rather than the crop breaking.
        const float b = 0.03f * 8.0f;  // one brick
        const float band = 3.0f * 0.03f;
        const cfloat3 corners[] = {cf3(0.0f, 0.0f, 0.6f), cf3(0.7f, 0.1f, 0.1f),
                                   cf3(-0.72f, -0.30f, 0.10f), cf3(0.15f, -0.85f, 0.30f)};
        std::size_t in_band = 0;
        for (cfloat3 c : corners) {
            const math::Aabb region{c, c + cf3(b, b, b)};
            scene::CullRegion cull{region};
            const scene::Tape culled = scene::compile_document(doc, &cull);

            for (cfloat3 p : points_in(region, 7)) {
                const kernel::CTapeValue a = whole.eval(kernel::cf3(p.x, p.y, p.z));
                const kernel::CTapeValue k = culled.eval(kernel::cf3(p.x, p.y, p.z));
                if (std::fabs(a.d) > band) continue;
                // EXACT, not close. A crop that produced the same shape by a
                // different route would be a second field, and this is why the
                // crop leaves the volume's origin and brick grid alone: moving
                // the origin changes `(p - origin) / cell` and the trilinear
                // weights that come out of it, which measured as a last-ulp
                // disagreement rather than as an error.
                CHECK(a.d == k.d);
                ++in_band;
            }
        }
        // A gate that compared nothing would pass. These regions are chosen to
        // cross the surface, so most of their samples are in the band.
        CHECK(in_band > 200);
    }
}

TEST_CASE("volume cull: the cropped tape is a fraction of the payload") {
    // The point of the exercise, asserted as BLOB FLOATS rather than as a
    // timing: the blob is what the compile copies, and a count cannot be
    // blamed on a shared box.
    const scene::Document prims = worked_ball();
    const field::FieldVolume vol = bake(prims, 0.02f);
    const scene::Document doc = volume_document(vol, math::Transform{});

    const scene::Tape whole = scene::compile_document(doc);
    const float b = 0.02f * 8.0f;
    const math::Aabb region{cf3(0.1f, 0.1f, 0.6f), cf3(0.1f + b, 0.1f + b, 0.6f + b)};
    scene::CullRegion cull{region};
    const scene::Tape culled = scene::compile_document(doc, &cull);

    MESSAGE("whole blob " << whole.blob.size() << " floats, culled " << culled.blob.size());
    REQUIRE(whole.blob.size() > 0);
    // A brick-sized region reaches 27 of the volume's bricks at the very most
    // (its own, plus the shell of neighbours the crop keeps for the trilinear
    // taps), so the culled blob is a small multiple of one brick rather than a
    // fraction of the whole. Asserted as a RATIO so it does not depend on the
    // fixture's brick count.
    CHECK(culled.blob.size() * 20 < whole.blob.size());
    // ... and it still carries the volume: an empty blob would pass the line
    // above and mean the item was dropped, which is a different field.
    CHECK(culled.blob.size() > 0);
    CHECK(culled.instrs.size() == whole.instrs.size());
}

TEST_CASE("volume cull: an uncalled compile is untouched") {
    // The whole-document tape is what clay_eval_points uses and what a host
    // uploads. Cropping must not reach it: with no cull region there is no
    // region to crop to, and the payload is the whole volume by definition.
    const scene::Document prims = worked_ball();
    const field::FieldVolume vol = bake(prims, 0.03f);
    const scene::Document doc = volume_document(vol, math::Transform{});

    const scene::Tape tape = scene::compile_document(doc);
    CHECK(tape.blob.size() == vol.blob_floats() + 0u);
}

TEST_CASE("volume cull: cropping preserves colour") {
    // colors_ is parallel to data_ per stored SAMPLE, so a crop that compacted
    // the samples and not the colours would hand the kernel a colour from
    // another brick — wrong shading over a correct surface, which is the
    // hardest kind of wrong to notice.
    const float cell = 0.05f;
    const scene::Document prims = worked_ball(/*coloured=*/true);
    const field::FieldVolume vol = bake(prims, cell);
    REQUIRE(vol.has_color());

    const scene::Document doc = volume_document(vol, math::Transform{});
    const scene::Tape whole = scene::compile_document(doc);
    const float b = cell * 8.0f;
    const math::Aabb region{cf3(0.0f, 0.0f, 0.55f), cf3(b, b, 0.55f + b)};
    scene::CullRegion cull{region};
    const scene::Tape culled = scene::compile_document(doc, &cull);
    CHECK(culled.blob.size() < whole.blob.size());

    for (cfloat3 p : points_in(region, 5)) {
        const kernel::CTapeValue a = whole.eval(kernel::cf3(p.x, p.y, p.z));
        const kernel::CTapeValue k = culled.eval(kernel::cf3(p.x, p.y, p.z));
        CHECK(a.d == k.d);
        CHECK(a.color.x == k.color.x);
        CHECK(a.color.y == k.color.y);
        CHECK(a.color.z == k.color.z);
    }
}
