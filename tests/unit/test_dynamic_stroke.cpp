// A stroke onto an adaptive surface (brush-engine and dynamic-topology specs,
// stroke-an-adaptive-surface).
//
// THE EQUALITY CASES ARE BIT-EXACT, not within a tolerance, and that is
// affordable because the adaptive sculptor was measured deterministic with
// topology on: the same stamps over the same surface give the same connectivity
// and the same positions. So "a stroke equals its stamps" is asserted as the
// same surface, vertex for vertex and index for index.
//
// The Snakehook cases assert a REACH and pair it with the loop a host would
// write without the stroke engine, asserted to fall short in the same test, so
// the threshold cannot be met vacuously by a fixture where every loop reaches.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/brush/surface_measure.h"
#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/remesh_local.h"
#include "clay/mesh/topology_delta.h"
#include "clay/voxel/mask.h"

using namespace clay;
using namespace clay::kernel;
using brush::Stamp;
using brush::StrokePreset;
using brush::StrokeSample;
using mesh::DynamicSculptor;
using mesh::DynamicStampResult;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::VertexId;

namespace {

Mesh cube_sphere(int n, float radius) {
    Mesh m;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const cfloat3 p = cf3(c[0], c[1], c[2]);
                const cfloat3 unit = p / clength(p);
                m.positions.push_back(unit * radius);
                m.normals.push_back(unit);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

std::vector<StrokeSample> line(cfloat3 a, cfloat3 b, int n) {
    std::vector<StrokeSample> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        out[static_cast<std::size_t>(i)].position = a + (b - a) * t;
        out[static_cast<std::size_t>(i)].pressure = 1.0f;
    }
    return out;
}

// Positions AND connectivity, bit for bit. Counts only would let a stroke that
// remeshed differently but landed on the same vertex count pass.
bool same_mesh(const Mesh& ma, const Mesh& mb) {
    if (ma.positions.size() != mb.positions.size() || ma.indices.size() != mb.indices.size())
        return false;
    for (std::size_t i = 0; i < ma.positions.size(); ++i)
        if (ma.positions[i].x != mb.positions[i].x || ma.positions[i].y != mb.positions[i].y ||
            ma.positions[i].z != mb.positions[i].z)
            return false;
    for (std::size_t i = 0; i < ma.indices.size(); ++i)
        if (ma.indices[i] != mb.indices[i]) return false;
    return true;
}

bool same_surface(const DynamicSurface& a, const DynamicSurface& b) {
    return same_mesh(a.to_mesh(), b.to_mesh());
}

float max_along(const DynamicSurface& s, cfloat3 axis) {
    float best = -1e30f;
    s.vertices().for_each_live([&](VertexId, const mesh::DynamicVertex& v) {
        best = std::max(best, cdot(v.position, axis));
    });
    return best;
}

// What the hand loop saw, for comparison with the stroke's summary.
struct LoopRun {
    std::size_t applied = 0;
    std::size_t anchor_deaths = 0;
    DynamicStampResult summed;
};

// THE STROKE WRITTEN OUT BY HAND, one `DynamicSculptor::stamp` per stamp, with
// the rules the stroke engine documents: radius and strength from the stamp,
// Grab centred on the first stamp, Snakehook on the vertex it drags and
// re-found at the previous stamp's position when a collapse retired it.
//
// `follow_cursor` is instead the loop a host writes from the headers alone:
// every stamp centred on its own position.
LoopRun hand_loop(DynamicSculptor& sc, const std::vector<Stamp>& stamps, MeshBrush verb,
                  const MeshBrushSettings& base, const DynamicTopologySettings& topo,
                  bool follow_cursor = false, mesh::TopologyDelta* record = nullptr) {
    LoopRun out;
    const bool dragging = verb == MeshBrush::Grab || verb == MeshBrush::Snakehook;
    VertexId anchor;
    if (verb == MeshBrush::Snakehook) anchor = sc.nearest_vertex(stamps.front().position);
    cfloat3 previous = stamps.front().position;
    for (const Stamp& s : stamps) {
        MeshBrushSettings b = base;
        b.radius = s.radius;
        b.strength = base.strength * s.strength;
        b.center = s.position;
        if (dragging) {
            b.direction = s.position - previous;
            if (follow_cursor) {
                b.center = s.position;
            } else if (verb == MeshBrush::Grab) {
                b.center = stamps.front().position;
            } else {
                if (sc.surface().vertex(anchor) == nullptr) {
                    ++out.anchor_deaths;
                    anchor = sc.nearest_vertex(previous);
                }
                b.center = sc.surface().position_of(anchor);
            }
        }
        previous = s.position;
        const DynamicStampResult r = sc.stamp(verb, b, topo, {}, record);
        if (r.changed()) ++out.applied;
        out.summed.moved_vertices += r.moved_vertices;
        out.summed.remesh.split += r.remesh.split;
        out.summed.remesh.collapsed += r.remesh.collapsed;
        out.summed.remesh.flipped += r.remesh.flipped;
        out.summed.remesh.relaxed += r.remesh.relaxed;
    }
    return out;
}

// A stroke with every stroke-engine channel that could make a hand loop and the
// consumer disagree: taper both ends, positional jitter, and a pressure ramp
// driving the size.
std::vector<Stamp> shaped_stroke() {
    StrokePreset preset;
    preset.radius = 0.3f;
    preset.spacing = 0.2f;
    preset.taper_start = 0.2f;
    preset.taper_end = 0.3f;
    preset.jitter_position = 0.1f;
    preset.seed = 7;
    preset.pressure.size = 1.0f;
    std::vector<StrokeSample> samples = line(cf3(-0.4f, 0.0f, 1.0f), cf3(0.4f, 0.2f, 1.1f), 40);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i].pressure = 0.4f + 0.015f * static_cast<float>(i);
    return brush::resolve_stroke(samples, preset);
}

MeshBrushSettings base_for(MeshBrush verb, float strength) {
    MeshBrushSettings b;
    b.strength = strength;
    b.geodesic = mesh::default_geodesic(verb);
    return b;
}

}  // namespace

TEST_CASE("dynamic stroke: a stroke equals its stamps, topology included") {
    const std::vector<Stamp> stamps = shaped_stroke();
    REQUIRE(stamps.size() > 4);
    const DynamicTopologySettings topo;  // enabled, brush-relative
    REQUIRE(topo.enabled);

    // One verb of each remesh timing is among these: Grab AFTER, Clay (and the
    // deposit family) BEFORE, Snakehook BEFORE AND AFTER.
    const MeshBrush verbs[] = {MeshBrush::Draw,    MeshBrush::Clay, MeshBrush::Smooth,
                               MeshBrush::Flatten, MeshBrush::Grab, MeshBrush::Snakehook};
    for (MeshBrush verb : verbs) {
        CAPTURE(static_cast<int>(verb));
        const MeshBrushSettings base = base_for(verb, 0.5f);

        auto a = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
        auto b = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        DynamicSculptor sa(*a);
        DynamicSculptor sb(*b);

        DynamicStampResult summary;
        const std::size_t applied =
            brush::apply_to_dynamic(sa, stamps, verb, base, topo, nullptr, nullptr, {}, &summary);
        const LoopRun loop = hand_loop(sb, stamps, verb, base, topo);

        CHECK(applied > 0);
        CHECK(applied == loop.applied);
        CHECK(same_surface(*a, *b));

        // THE SCHEDULE RAN PER STAMP: the stroke's topology counts are the sum
        // of its stamps', not one stroke-level pass.
        CHECK(summary.moved_vertices == loop.summed.moved_vertices);
        CHECK(summary.remesh.split == loop.summed.remesh.split);
        CHECK(summary.remesh.collapsed == loop.summed.remesh.collapsed);
        CHECK(summary.remesh.flipped == loop.summed.remesh.flipped);
        CHECK(summary.remesh.relaxed == loop.summed.remesh.relaxed);
        CHECK(summary.topology_revision == a->topology_revision());
        CHECK(summary.geometry_revision == a->geometry_revision());
        CHECK_FALSE(summary.dirty_bounds.empty());
        CHECK(mesh::validate_dynamic_surface(*a).ok);
    }
}

TEST_CASE("dynamic stroke: a Clay stroke remeshes before its first deposit") {
    // The deposit family's timing is BEFORE, so the first stamp already splits
    // the coarse sphere it lands on — the same count a single stamp reports.
    REQUIRE(mesh::default_timing(MeshBrush::Clay) == mesh::RemeshTiming::BeforeBrush);
    const std::vector<Stamp> stamps = shaped_stroke();
    const std::vector<Stamp> first(stamps.begin(), stamps.begin() + 1);
    const DynamicTopologySettings topo;
    const MeshBrushSettings base = base_for(MeshBrush::Clay, 0.5f);

    auto a = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    auto b = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    DynamicSculptor sa(*a);
    DynamicSculptor sb(*b);
    DynamicStampResult summary;
    REQUIRE(brush::apply_to_dynamic(sa, first, MeshBrush::Clay, base, topo, nullptr, nullptr, {},
                                    &summary) == 1);
    const LoopRun loop = hand_loop(sb, first, MeshBrush::Clay, base, topo);
    CHECK(summary.remesh.split > 0);
    CHECK(summary.remesh.split == loop.summed.remesh.split);
    CHECK(same_surface(*a, *b));
}

TEST_CASE("dynamic stroke: Layer is refused, not remapped, and runs no remesh") {
    const std::vector<Stamp> stamps = shaped_stroke();
    auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    auto pristine = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    REQUIRE(surface.has_value());
    REQUIRE(pristine.has_value());
    DynamicSculptor sc(*surface);
    const std::uint64_t topology_before = surface->topology_revision();
    const std::uint64_t geometry_before = surface->geometry_revision();

    mesh::TopologyDelta record;
    DynamicStampResult summary;
    const DynamicTopologySettings topo;
    REQUIRE(topo.enabled);
    // An estimator in the options, so "touches nothing" includes the sculptor's
    // automask inputs: `DynamicSculptor::stamp` refuses Layer per stamp as well,
    // and the surface alone could not tell a stroke-level refusal from that.
    brush::MeshStrokeOptions options;
    options.cavity_field = [](cfloat3 p) { return clength(p) - 1.0f; };
    CHECK(brush::apply_to_dynamic(sc, stamps, MeshBrush::Layer, base_for(MeshBrush::Layer, 0.5f),
                                  topo, nullptr, &record, options, &summary) == 0);
    CHECK_FALSE(static_cast<bool>(sc.automask_inputs().cavity));
    CHECK(record.empty());
    CHECK(surface->topology_revision() == topology_before);
    CHECK(surface->geometry_revision() == geometry_before);
    CHECK(summary.remesh.total() == 0);
    CHECK(summary.moved_vertices == 0);
    CHECK(same_surface(*surface, *pristine));
}

TEST_CASE("dynamic stroke: a request to defer normals is refused rather than ignored") {
    const std::vector<Stamp> stamps = shaped_stroke();
    auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    auto pristine = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
    REQUIRE(surface.has_value());
    REQUIRE(pristine.has_value());
    DynamicSculptor sc(*surface);
    brush::MeshStrokeOptions options;
    options.defer_normals = true;
    options.cavity_field = [](cfloat3 p) { return clength(p) - 1.0f; };
    mesh::TopologyDelta record;
    CHECK(brush::apply_to_dynamic(sc, stamps, MeshBrush::Draw, base_for(MeshBrush::Draw, 0.5f),
                                  DynamicTopologySettings{}, nullptr, &record, options) == 0);
    CHECK_FALSE(static_cast<bool>(sc.automask_inputs().cavity));
    CHECK(record.empty());
    CHECK(same_surface(*surface, *pristine));

    // And an empty stroke is nothing, not a stamp at the origin.
    CHECK(brush::apply_to_dynamic(sc, {}, MeshBrush::Draw, base_for(MeshBrush::Draw, 0.5f),
                                  DynamicTopologySettings{}) == 0);
    CHECK(same_surface(*surface, *pristine));
}

TEST_CASE("dynamic stroke: the stylus azimuth reaches the alpha only on request") {
    std::vector<float> alpha(64, 0.0f);
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 4; ++u) alpha[static_cast<std::size_t>(v * 8 + u)] = 1.0f;
    StrokePreset preset;
    preset.radius = 0.35f;
    preset.spacing = 0.25f;
    preset.rotate_to_azimuth = true;
    MeshBrushSettings base;
    base.strength = 0.3f;
    base.alpha = alpha.data();
    base.alpha_width = 8;
    base.alpha_height = 8;
    base.alpha_direction = cf3(0, 0, 1);
    const DynamicTopologySettings topo;

    auto run = [&](float azimuth, bool orient) {
        std::vector<StrokeSample> samples = line(cf3(-0.1f, 0, 1.0f), cf3(0.1f, 0, 1.0f), 8);
        for (StrokeSample& s : samples) {
            s.tilt = 0.5f;
            s.azimuth = azimuth;
        }
        auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
        REQUIRE(surface.has_value());
        DynamicSculptor sc(*surface);
        brush::MeshStrokeOptions options;
        options.orient_alpha_by_stamp = orient;
        CHECK(brush::apply_to_dynamic(sc, brush::resolve_stroke(samples, preset), MeshBrush::Draw,
                                      base, topo, nullptr, nullptr, options) > 0);
        return surface->to_mesh();
    };
    const float quarter = 1.5707964f;
    CHECK(same_mesh(run(0.0f, false), run(quarter, false)));
    CHECK_FALSE(same_mesh(run(0.0f, true), run(quarter, true)));
}

TEST_CASE("dynamic stroke: a snakehook keeps pulling where a cursor-following loop falls behind") {
    StrokePreset preset;
    preset.radius = 0.3f;
    preset.spacing = 0.1f;
    const cfloat3 from = cf3(0, 0, 1.0f);
    const cfloat3 to = cf3(0, 0, 1.8f);
    const std::vector<Stamp> stamps = brush::resolve_stroke(line(from, to, 64), preset);
    const MeshBrushSettings base = base_for(MeshBrush::Snakehook, 1.0f);
    DynamicTopologySettings topo;
    topo.detail_resolution = 8.0f;
    const cfloat3 axis = cf3(0, 0, 1);

    auto stroke = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    auto host = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(stroke.has_value());
    REQUIRE(host.has_value());
    DynamicSculptor ss(*stroke);
    DynamicSculptor sh(*host);
    CHECK(brush::apply_to_dynamic(ss, stamps, MeshBrush::Snakehook, base, topo) > 0);
    hand_loop(sh, stamps, MeshBrush::Snakehook, base, topo, /*follow_cursor=*/true);

    const float drag = cdot(to - from, axis);
    const float reach = (max_along(*stroke, axis) - cdot(from, axis)) / drag;
    const float host_reach = (max_along(*host, axis) - cdot(from, axis)) / drag;
    CAPTURE(reach);
    CAPTURE(host_reach);
    // Measured 96% and 42%.
    CHECK(reach >= 0.90f);
    CHECK(host_reach < 0.60f);
    CHECK(mesh::validate_dynamic_surface(*stroke).ok);
}

TEST_CASE("dynamic stroke: a snakehook re-finds an anchor the remesher retired") {
    // Detail 4 and a dense stroke: the coarse target collapses the vertex being
    // dragged, repeatedly. Measured 14 deaths over 61 stamps.
    StrokePreset preset;
    preset.radius = 0.25f;
    preset.spacing = 0.05f;
    const cfloat3 from = cf3(0, 0, 1.0f);
    const cfloat3 to = cf3(0, 0, 2.5f);
    const std::vector<Stamp> stamps = brush::resolve_stroke(line(from, to, 64), preset);
    const MeshBrushSettings base = base_for(MeshBrush::Snakehook, 1.0f);
    DynamicTopologySettings topo;
    topo.detail_resolution = 4.0f;

    auto stroke = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    auto loop_surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(stroke.has_value());
    REQUIRE(loop_surface.has_value());
    DynamicSculptor ss(*stroke);
    DynamicSculptor sl(*loop_surface);
    CHECK(brush::apply_to_dynamic(ss, stamps, MeshBrush::Snakehook, base, topo) > 0);
    const LoopRun loop = hand_loop(sl, stamps, MeshBrush::Snakehook, base, topo);

    // THE PRECONDITION, counted: without a death this test says nothing about
    // re-finding.
    CAPTURE(loop.anchor_deaths);
    REQUIRE(loop.anchor_deaths >= 1);
    CHECK(same_surface(*stroke, *loop_surface));

    const cfloat3 axis = cf3(0, 0, 1);
    const float reach = (max_along(*stroke, axis) - cdot(from, axis)) / cdot(to - from, axis);
    CAPTURE(reach);
    // Measured 81%; stamping where the dead anchor was kept 15%.
    CHECK(reach >= 0.75f);
    CHECK(mesh::validate_dynamic_surface(*stroke).ok);
}

TEST_CASE("dynamic stroke: one stroke is one undo step") {
    StrokePreset preset;
    preset.radius = 0.3f;
    preset.spacing = 0.2f;
    const std::vector<Stamp> stamps =
        brush::resolve_stroke(line(cf3(-0.4f, 0, 1), cf3(0.4f, 0, 1), 20), preset);
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    auto pristine = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    REQUIRE(pristine.has_value());
    DynamicSculptor sc(*surface);
    mesh::TopologyDelta record;
    DynamicStampResult summary;
    REQUIRE(brush::apply_to_dynamic(sc, stamps, MeshBrush::Clay, base_for(MeshBrush::Clay, 0.5f),
                                    DynamicTopologySettings{}, nullptr, &record, {},
                                    &summary) > 0);
    REQUIRE(summary.remesh.split > 0);
    REQUIRE_FALSE(same_surface(*surface, *pristine));

    REQUIRE(record.revert(*surface));
    const mesh::DynamicValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    CHECK(report.ok);
    CHECK(same_surface(*surface, *pristine));
}

TEST_CASE("dynamic stroke: a mask gates the stroke") {
    // Topology off so vertex ids are stable and a vertex can be compared with
    // itself before and after.
    DynamicTopologySettings topo;
    topo.enabled = false;
    StrokePreset preset;
    preset.radius = 0.35f;
    preset.spacing = 0.25f;
    const std::vector<Stamp> stamps =
        brush::resolve_stroke(line(cf3(0, -0.3f, 1.0f), cf3(0, 0.3f, 1.0f), 16), preset);
    const MeshBrushSettings base = base_for(MeshBrush::Draw, 0.5f);

    SUBCASE("a stamp centred in a frozen region is skipped, remesh included") {
        // Topology ON here: skipping the stamp is what keeps a frozen region's
        // connectivity, since the gate alone would still let it remesh.
        voxel::MaskField mask(0.25f);
        for (int x = -8; x <= 8; ++x)
            for (int y = -8; y <= 8; ++y)
                for (int z = -8; z <= 8; ++z) mask.set({x, y, z}, 1.0f);
        auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
        auto pristine = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
        REQUIRE(surface.has_value());
        REQUIRE(pristine.has_value());
        DynamicSculptor sc(*surface);
        CHECK(brush::apply_to_dynamic(sc, stamps, MeshBrush::Draw, base,
                                      DynamicTopologySettings{}, &mask) == 0);
        CHECK(same_surface(*surface, *pristine));
    }

    SUBCASE("a half-masked region moves on one side only") {
        // Every cell with x < 0 frozen; the stroke runs along x = 0, so its
        // centres sample the unfrozen cell and the stamps run.
        voxel::MaskField mask(0.25f);
        for (int x = -8; x <= -1; ++x)
            for (int y = -8; y <= 8; ++y)
                for (int z = -8; z <= 8; ++z) mask.set({x, y, z}, 1.0f);
        auto surface = DynamicSurface::from_mesh(cube_sphere(12, 1.0f));
        REQUIRE(surface.has_value());
        std::unordered_map<std::uint32_t, cfloat3> before;
        surface->vertices().for_each_live(
            [&](VertexId id, const mesh::DynamicVertex& v) { before[id.slot] = v.position; });
        DynamicSculptor sc(*surface);
        CHECK(brush::apply_to_dynamic(sc, stamps, MeshBrush::Draw, base, topo, &mask) > 0);

        std::size_t moved_frozen = 0;
        std::size_t moved_free = 0;
        surface->vertices().for_each_live([&](VertexId id, const mesh::DynamicVertex& v) {
            const cfloat3 was = before.at(id.slot);
            const bool moved = was.x != v.position.x || was.y != v.position.y ||
                               was.z != v.position.z;
            if (!moved) return;
            if (was.x < 0.0f) ++moved_frozen;
            if (was.x > 0.0f) ++moved_free;
        });
        CHECK(moved_frozen == 0);
        CHECK(moved_free > 0);
    }
}

TEST_CASE("dynamic stroke: the stroke wires the cavity estimator from its options") {
    // A rippled sphere: the troughs read as cavity, so the factor has something
    // to protect.
    const std::function<float(cfloat3)> field = [](cfloat3 p) {
        return clength(p) - 1.0f + 0.04f * std::sin(18.0f * p.x);
    };
    brush::MeasureSettings measure;
    measure.scale = 0.05f;
    const std::vector<Stamp> stamps = shaped_stroke();
    MeshBrushSettings base = base_for(MeshBrush::Draw, 0.5f);
    base.automask.factors = static_cast<std::uint32_t>(mesh::AutomaskFactor::Cavity);
    const DynamicTopologySettings topo;

    auto wired = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    auto by_hand = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    auto unwired = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(wired.has_value());
    REQUIRE(by_hand.has_value());
    REQUIRE(unwired.has_value());

    DynamicSculptor sw(*wired);
    brush::MeshStrokeOptions options;
    options.cavity_field = field;
    options.cavity_measure = measure;
    REQUIRE(brush::apply_to_dynamic(sw, stamps, MeshBrush::Draw, base, topo, nullptr, nullptr,
                                    options) > 0);

    // The same estimator, set on the sculptor by a host driving it stamp by
    // stamp — what `mesh_automask_inputs` builds, spelled out.
    DynamicSculptor sh(*by_hand);
    mesh::AutomaskInputs inputs;
    inputs.cavity = [field, measure](cfloat3 p) {
        return brush::measure_at(field, brush::SurfaceMeasure::Cavity, p, measure);
    };
    sh.set_automask_inputs(std::move(inputs));
    hand_loop(sh, stamps, MeshBrush::Draw, base, topo);

    DynamicSculptor su(*unwired);
    REQUIRE(brush::apply_to_dynamic(su, stamps, MeshBrush::Draw, base, topo) > 0);

    CHECK(same_surface(*wired, *by_hand));
    CHECK_FALSE(same_surface(*wired, *unwired));
}

TEST_CASE("dynamic stroke: the summary ORs the budget flag and unites the dirty bounds") {
    // The LAST stamp lands far off the surface, so it moves nothing, remeshes
    // nothing and reports no budget hit and empty bounds. A summary that kept
    // the last stamp's flag or bounds instead of folding them would read
    // "converged, nothing dirty" for a stroke that stopped at its bound.
    std::vector<Stamp> stamps = shaped_stroke();
    stamps.resize(3);
    Stamp away = stamps.back();
    away.position = cf3(0.0f, 0.0f, 10.0f);
    stamps.push_back(away);

    DynamicTopologySettings topo;
    topo.max_ops_per_stamp = 1;
    const MeshBrushSettings base = base_for(MeshBrush::Clay, 0.5f);

    auto a = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    auto b = DynamicSurface::from_mesh(cube_sphere(8, 1.0f));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    DynamicSculptor sa(*a);
    DynamicSculptor sb(*b);

    DynamicStampResult summary;
    REQUIRE(brush::apply_to_dynamic(sa, stamps, MeshBrush::Clay, base, topo, nullptr, nullptr, {},
                                    &summary) == 3);

    // The expectation, stamp by stamp on an identical surface.
    bool any_budget = false;
    math::Aabb united;
    DynamicStampResult last;
    for (const Stamp& s : stamps) {
        MeshBrushSettings stamp_settings = base;
        stamp_settings.center = s.position;
        stamp_settings.radius = s.radius;
        stamp_settings.strength = base.strength * s.strength;
        last = sb.stamp(MeshBrush::Clay, stamp_settings, topo, {}, nullptr);
        any_budget = any_budget || last.remesh.hit_budget;
        united.expand(last.dirty_bounds);
    }
    // The preconditions: the flag and the bounds come from EARLIER stamps.
    REQUIRE(any_budget);
    REQUIRE_FALSE(last.remesh.hit_budget);
    REQUIRE(last.dirty_bounds.empty());
    REQUIRE_FALSE(united.empty());

    CHECK(summary.remesh.hit_budget);
    CHECK(summary.dirty_bounds.min.x == united.min.x);
    CHECK(summary.dirty_bounds.min.y == united.min.y);
    CHECK(summary.dirty_bounds.min.z == united.min.z);
    CHECK(summary.dirty_bounds.max.x == united.max.x);
    CHECK(summary.dirty_bounds.max.y == united.max.y);
    CHECK(summary.dirty_bounds.max.z == united.max.z);
    CHECK(same_surface(*a, *b));
}

// -- a whole stroke as one replayable record (record-a-whole-adaptive-stroke) --
//
// "EXACT" below is stronger than `same_surface`: the export's positions, normals
// and indices AND the stored positions and normals of every live vertex and
// face, bit for bit. A replay restores slots, so live elements compare in slot
// order.

namespace {

bool same_bits(const cfloat3& a, const cfloat3& b) {
    return std::memcmp(&a, &b, sizeof(cfloat3)) == 0;
}

bool same_points(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_bits(a[i], b[i])) return false;
    return true;
}

struct Exact {
    std::vector<cfloat3> vertex_positions, vertex_normals, face_normals;
    Mesh exported;
};

Exact exact_of(const DynamicSurface& s) {
    Exact out;
    s.vertices().for_each_live([&](VertexId, const mesh::DynamicVertex& v) {
        out.vertex_positions.push_back(v.position);
        out.vertex_normals.push_back(v.normal);
    });
    s.faces().for_each_live(
        [&](mesh::FaceId, const mesh::DynamicFace& f) { out.face_normals.push_back(f.normal); });
    out.exported = s.to_mesh();
    return out;
}

bool same_exact(const Exact& a, const Exact& b) {
    return same_mesh(a.exported, b.exported) &&
           same_points(a.exported.normals, b.exported.normals) &&
           same_points(a.vertex_positions, b.vertex_positions) &&
           same_points(a.vertex_normals, b.vertex_normals) &&
           same_points(a.face_normals, b.face_normals);
}

// Live faces the sculptor's index does not hold: a replay must keep it in step.
std::size_t unindexed_faces(const DynamicSculptor& sc) {
    std::size_t n = 0;
    sc.surface().faces().for_each_live([&](mesh::FaceId f, const mesh::DynamicFace&) {
        if (sc.bvh().leaf_of(f) == mesh::DynamicBvh::kNoLeaf) ++n;
    });
    return n;
}

// The stroke's rules, one `stamp_recorded` per stamp, every one REQUIRED to
// succeed: a mismatch at stamp k>0 is exactly what checking the mark once
// assumes cannot happen.
std::size_t recorded_loop(DynamicSculptor& sc, const std::vector<Stamp>& stamps, MeshBrush verb,
                          const MeshBrushSettings& base, const DynamicTopologySettings& topo,
                          mesh::RecordedGesture& record) {
    std::size_t deaths = 0;
    const bool dragging = verb == MeshBrush::Grab || verb == MeshBrush::Snakehook;
    VertexId anchor;
    if (verb == MeshBrush::Snakehook) anchor = sc.nearest_vertex(stamps.front().position);
    cfloat3 previous = stamps.front().position;
    for (const Stamp& s : stamps) {
        MeshBrushSettings b = base;
        b.radius = s.radius;
        b.strength = base.strength * s.strength;
        b.center = s.position;
        if (dragging) {
            b.direction = s.position - previous;
            if (verb == MeshBrush::Grab) {
                b.center = stamps.front().position;
            } else {
                if (sc.surface().vertex(anchor) == nullptr) {
                    ++deaths;
                    anchor = sc.nearest_vertex(previous);
                }
                b.center = sc.surface().position_of(anchor);
            }
        }
        previous = s.position;
        REQUIRE(sc.stamp_recorded(verb, b, topo, {}, record).has_value());
    }
    return deaths;
}

struct RecordFixture {
    MeshBrush verb;
    std::vector<Stamp> stamps;
    MeshBrushSettings base;
    DynamicTopologySettings topo;
    int grid;
    bool expects_deaths;
};

// The six verbs over the shaped stroke, and the anchor-death Snakehook of
// "re-finds an anchor the remesher retired". Default topology: relax ON.
std::vector<RecordFixture> record_fixtures() {
    std::vector<RecordFixture> out;
    const MeshBrush verbs[] = {MeshBrush::Draw,    MeshBrush::Clay, MeshBrush::Smooth,
                               MeshBrush::Flatten, MeshBrush::Grab, MeshBrush::Snakehook};
    for (MeshBrush verb : verbs)
        out.push_back({verb, shaped_stroke(), base_for(verb, 0.5f), DynamicTopologySettings{}, 16,
                       false});
    StrokePreset preset;
    preset.radius = 0.25f;
    preset.spacing = 0.05f;
    DynamicTopologySettings topo;
    topo.detail_resolution = 4.0f;
    out.push_back({MeshBrush::Snakehook,
                   brush::resolve_stroke(line(cf3(0, 0, 1.0f), cf3(0, 0, 2.5f), 64), preset),
                   base_for(MeshBrush::Snakehook, 1.0f), topo, 24, true});
    return out;
}

struct RecordState {
    std::size_t encoded;
    mesh::SurfaceMark before, after;
};

RecordState state_of(const mesh::RecordedGesture& r) {
    return {r.encoded_size(), r.before(), r.after()};
}

bool same_state(const RecordState& a, const RecordState& b) {
    return a.encoded == b.encoded && a.before == b.before && a.after == b.after;
}

}  // namespace

TEST_CASE("dynamic stroke: a recorded stroke is the unrecorded stroke, and undoes exactly") {
    for (const RecordFixture& f : record_fixtures()) {
        CAPTURE(static_cast<int>(f.verb));
        CAPTURE(f.expects_deaths);
        REQUIRE(f.topo.relax_after_remesh);
        auto recorded = DynamicSurface::from_mesh(cube_sphere(f.grid, 1.0f));
        auto plain = DynamicSurface::from_mesh(cube_sphere(f.grid, 1.0f));
        auto looped = DynamicSurface::from_mesh(cube_sphere(f.grid, 1.0f));
        REQUIRE(recorded.has_value());
        REQUIRE(plain.has_value());
        REQUIRE(looped.has_value());
        DynamicSculptor sr(*recorded);
        DynamicSculptor sp(*plain);
        DynamicSculptor sl(*looped);
        const Exact before = exact_of(*recorded);

        mesh::RecordedGesture record;
        DynamicStampResult summary;
        const std::optional<std::size_t> applied = brush::apply_to_dynamic_recorded(
            sr, f.stamps, f.verb, f.base, f.topo, nullptr, record, {}, &summary);
        REQUIRE(applied.has_value());
        CHECK(*applied > 0);
        const Exact after = exact_of(*recorded);

        // RECORDING CHANGES NOTHING: surface, applied count and summary.
        DynamicStampResult plain_summary;
        const std::size_t plain_applied = brush::apply_to_dynamic(
            sp, f.stamps, f.verb, f.base, f.topo, nullptr, nullptr, {}, &plain_summary);
        CHECK(same_exact(after, exact_of(*plain)));
        CHECK(*applied == plain_applied);
        CHECK(summary.moved_vertices == plain_summary.moved_vertices);
        CHECK(summary.remesh.split == plain_summary.remesh.split);
        CHECK(summary.remesh.collapsed == plain_summary.remesh.collapsed);
        CHECK(summary.remesh.flipped == plain_summary.remesh.flipped);
        CHECK(summary.remesh.relaxed == plain_summary.remesh.relaxed);

        // THE SAME RECORD as the stamps captured one by one with the stroke's
        // rules, every `stamp_recorded` required to pass the mark.
        mesh::RecordedGesture loop_record;
        const std::size_t deaths = recorded_loop(sl, f.stamps, f.verb, f.base, f.topo, loop_record);
        if (f.expects_deaths) REQUIRE(deaths >= 1);  // measured 11
        CHECK(same_exact(after, exact_of(*looped)));
        CHECK(record.encoded_size() == loop_record.encoded_size());

        // ONE UNDO STEP, exact at both ends, index in step.
        REQUIRE(sr.replay(record, mesh::ReplayDirection::Revert) == mesh::ReplayResult::Applied);
        CHECK(same_exact(exact_of(*recorded), before));
        CHECK(mesh::validate_dynamic_surface(*recorded).ok);
        CHECK(unindexed_faces(sr) == 0);
        REQUIRE(sr.replay(record, mesh::ReplayDirection::Apply) == mesh::ReplayResult::Applied);
        CHECK(same_exact(exact_of(*recorded), after));
        CHECK(mesh::validate_dynamic_surface(*recorded).ok);
        CHECK(unindexed_faces(sr) == 0);
    }
}

TEST_CASE("dynamic stroke: strokes accumulate into one record and revert as one step") {
    StrokePreset preset;
    preset.radius = 0.3f;
    preset.spacing = 0.2f;
    const std::vector<Stamp> grab =
        brush::resolve_stroke(line(cf3(0.0f, -0.4f, 1.0f), cf3(0.2f, 0.4f, 1.0f), 30), preset);
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sc(*surface);
    const Exact pristine = exact_of(*surface);

    mesh::RecordedGesture record;
    MeshBrushSettings dab = base_for(MeshBrush::Draw, 0.5f);
    dab.radius = 0.3f;
    dab.center = cf3(0.3f, 0.3f, 0.9f);
    REQUIRE(sc.stamp_recorded(MeshBrush::Draw, dab, DynamicTopologySettings{}, {}, record));
    const std::size_t after_dab = record.encoded_size();
    REQUIRE(brush::apply_to_dynamic_recorded(sc, shaped_stroke(), MeshBrush::Clay,
                                             base_for(MeshBrush::Clay, 0.5f),
                                             DynamicTopologySettings{}, nullptr, record));
    const std::size_t after_clay = record.encoded_size();
    REQUIRE(brush::apply_to_dynamic_recorded(sc, grab, MeshBrush::Grab,
                                             base_for(MeshBrush::Grab, 0.5f),
                                             DynamicTopologySettings{}, nullptr, record));
    CHECK(after_dab < after_clay);
    CHECK(after_clay < record.encoded_size());
    const Exact end = exact_of(*surface);

    REQUIRE(sc.replay(record, mesh::ReplayDirection::Revert) == mesh::ReplayResult::Applied);
    CHECK(same_exact(exact_of(*surface), pristine));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    REQUIRE(sc.replay(record, mesh::ReplayDirection::Apply) == mesh::ReplayResult::Applied);
    CHECK(same_exact(exact_of(*surface), end));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("dynamic stroke: a recorded stroke binds where it began, so older records still undo") {
    // Two records, last in first out: a dab, then a stroke. Undoing the stroke
    // must leave the surface at the mark the DAB's record ends at, or the dab can
    // no longer be undone. A stroke into an empty record that failed to bind its
    // start would restore an unnamed state here and strand every older record.
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sc(*surface);
    const Exact pristine = exact_of(*surface);

    mesh::RecordedGesture dab_record;
    MeshBrushSettings dab = base_for(MeshBrush::Draw, 0.5f);
    dab.radius = 0.3f;
    dab.center = cf3(0.3f, 0.3f, 0.9f);
    REQUIRE(sc.stamp_recorded(MeshBrush::Draw, dab, DynamicTopologySettings{}, {}, dab_record));
    const mesh::SurfaceMark after_dab = surface->mark();

    mesh::RecordedGesture stroke_record;
    REQUIRE(brush::apply_to_dynamic_recorded(sc, shaped_stroke(), MeshBrush::Grab,
                                             base_for(MeshBrush::Grab, 0.5f),
                                             DynamicTopologySettings{}, nullptr, stroke_record));
    CHECK(stroke_record.before() == after_dab);
    CHECK(stroke_record.after() == surface->mark());

    REQUIRE(sc.replay(stroke_record, mesh::ReplayDirection::Revert) ==
            mesh::ReplayResult::Applied);
    CHECK(surface->mark() == after_dab);
    CHECK(sc.replay(dab_record, mesh::ReplayDirection::Revert) == mesh::ReplayResult::Applied);
    CHECK(same_exact(exact_of(*surface), pristine));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("dynamic stroke: a refused or mismatched recorded stroke leaves the record untouched") {
    const std::vector<Stamp> stamps = shaped_stroke();
    const MeshBrushSettings draw = base_for(MeshBrush::Draw, 0.5f);
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sc(*surface);
    mesh::RecordedGesture record;
    REQUIRE(brush::apply_to_dynamic_recorded(sc, stamps, MeshBrush::Draw, draw,
                                             DynamicTopologySettings{}, nullptr, record));
    const RecordState held = state_of(record);

    // Everything the stroke refuses by itself returns 0, not a mismatch.
    auto refused = [&](const std::vector<Stamp>& s, MeshBrush verb,
                       const brush::MeshStrokeOptions& options) {
        const Exact was = exact_of(*surface);
        const std::uint64_t topology = surface->topology_revision();
        const std::uint64_t geometry = surface->geometry_revision();
        const std::optional<std::size_t> r = brush::apply_to_dynamic_recorded(
            sc, s, verb, base_for(verb, 0.5f), DynamicTopologySettings{}, nullptr, record,
            options);
        CHECK(r.has_value());
        CHECK(r.value_or(1) == 0);
        CHECK(same_state(state_of(record), held));
        CHECK(same_exact(exact_of(*surface), was));
        CHECK(surface->topology_revision() == topology);
        CHECK(surface->geometry_revision() == geometry);
    };
    SUBCASE("Layer") { refused(stamps, MeshBrush::Layer, {}); }
    SUBCASE("defer_normals") {
        brush::MeshStrokeOptions defer;
        defer.defer_normals = true;
        refused(stamps, MeshBrush::Draw, defer);
    }
    SUBCASE("no stamps") { refused({}, MeshBrush::Draw, {}); }

    SUBCASE("an unrecorded stamp in between is a mismatch, and nothing is stamped") {
        MeshBrushSettings dab = draw;
        dab.radius = 0.3f;
        dab.center = cf3(0, 0, -1);
        REQUIRE(sc.stamp(MeshBrush::Draw, dab, DynamicTopologySettings{}).changed());
        const Exact was = exact_of(*surface);
        const std::uint64_t geometry = surface->geometry_revision();
        DynamicStampResult summary;
        summary.moved_vertices = 99;
        CHECK_FALSE(brush::apply_to_dynamic_recorded(sc, stamps, MeshBrush::Draw, draw,
                                                     DynamicTopologySettings{}, nullptr, record,
                                                     {}, &summary)
                        .has_value());
        CHECK(summary.moved_vertices == 0);  // reset on every path
        CHECK(same_state(state_of(record), held));
        CHECK(same_exact(exact_of(*surface), was));
        CHECK(surface->geometry_revision() == geometry);
        // And the replay still refuses it: the record was not re-bound.
        CHECK(sc.replay(record, mesh::ReplayDirection::Revert) == mesh::ReplayResult::Mismatch);
    }

    SUBCASE("a record from another surface is a mismatch") {
        auto other = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
        REQUIRE(other.has_value());
        DynamicSculptor so(*other);
        const Exact was = exact_of(*other);
        const std::uint64_t geometry = other->geometry_revision();
        CHECK_FALSE(brush::apply_to_dynamic_recorded(so, stamps, MeshBrush::Draw, draw,
                                                     DynamicTopologySettings{}, nullptr, record)
                        .has_value());
        CHECK(same_state(state_of(record), held));
        CHECK(same_exact(exact_of(*other), was));
        CHECK(other->geometry_revision() == geometry);
    }

    SUBCASE("a stroke that misses the surface leaves a non-empty record and the mark alone") {
        std::vector<Stamp> far = stamps;
        for (Stamp& s : far) s.position = s.position + cf3(0, 0, 50);
        const mesh::SurfaceMark mark = surface->mark();
        const std::optional<std::size_t> r = brush::apply_to_dynamic_recorded(
            sc, far, MeshBrush::Draw, draw, DynamicTopologySettings{}, nullptr, record);
        CHECK(r.value_or(1) == 0);
        CHECK(same_state(state_of(record), held));
        CHECK(surface->mark() == mark);
    }
}

TEST_CASE("dynamic stroke: a malformed stroke into a stale record is refused, not a mismatch") {
    // The order the header promises: the stroke's own refusals come BEFORE the
    // mark, so a host is never told to retry a call that can never succeed.
    // With a matching record both orders return 0; only a stale one tells them
    // apart.
    const std::vector<Stamp> stamps = shaped_stroke();
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sc(*surface);
    mesh::RecordedGesture record;
    REQUIRE(brush::apply_to_dynamic_recorded(sc, stamps, MeshBrush::Draw,
                                             base_for(MeshBrush::Draw, 0.5f),
                                             DynamicTopologySettings{}, nullptr, record));
    MeshBrushSettings dab = base_for(MeshBrush::Draw, 0.5f);
    dab.radius = 0.3f;
    dab.center = cf3(0, 0, -1);
    REQUIRE(sc.stamp(MeshBrush::Draw, dab, DynamicTopologySettings{}).changed());
    REQUIRE_FALSE(record.can_capture_on(*surface));  // stale
    const RecordState held = state_of(record);
    const Exact was = exact_of(*surface);

    auto refused = [&](const std::vector<Stamp>& s, MeshBrush verb,
                       const brush::MeshStrokeOptions& options) {
        const std::optional<std::size_t> r = brush::apply_to_dynamic_recorded(
            sc, s, verb, base_for(verb, 0.5f), DynamicTopologySettings{}, nullptr, record,
            options);
        CHECK(r.has_value());
        CHECK(r.value_or(1) == 0);
        CHECK(same_state(state_of(record), held));
        CHECK(same_exact(exact_of(*surface), was));
    };
    SUBCASE("Layer") { refused(stamps, MeshBrush::Layer, {}); }
    SUBCASE("defer_normals") {
        brush::MeshStrokeOptions defer;
        defer.defer_normals = true;
        refused(stamps, MeshBrush::Draw, defer);
    }
    SUBCASE("no stamps") { refused({}, MeshBrush::Draw, {}); }
}
