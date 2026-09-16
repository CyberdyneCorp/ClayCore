// An adaptive stroke's record is exact, and replaying it keeps the sculptor in
// step (dynamic-topology spec, undo-a-dynamic-stroke-across-the-abi).
//
// Three claims, each of which was false on the tree this change started from:
//
//   - THE RECORD'S `after` END IS THE LIVE SURFACE, normals included. The relax
//     pass rewrote face and vertex normals it never noted, so redo restored
//     stale normals and undo occasionally did too. Positions and indices were
//     exact throughout, which is why the old `same_mesh` never saw it.
//   - A REPLAY THROUGH THE SCULPTOR LEAVES ITS INDEX COVERING EXACTLY THE LIVE
//     FACES. `TopologyDelta::revert` alone left faces the stroke deleted
//     outside every chunk, so the same stroke stamped again produced a
//     different surface.
//   - A REPLAY ONTO THE WRONG STATE IS REFUSED BEFORE ANYTHING IS WRITTEN.
//     Two strokes on opposite hemispheres share slots, so an out-of-order
//     revert corrupts a surface no spatial test would connect to it.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay/mesh/dynamic_sculpt.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/topology_delta.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSculptor;
using mesh::DynamicSurface;
using mesh::DynamicTopologySettings;
using mesh::Mesh;
using mesh::TopologyDelta;

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

struct StrokeShape {
    int stamps;
    float radius;
    float strength;
    float detail;
    // Where the stroke runs: a unit direction the arc is centred on.
    cfloat3 axis = cf3(0, 0, 1);
};

// One Draw stroke along a short arc, with the topology DEFAULTS -- so the relax
// pass is on, which is the configuration the record used to get wrong.
mesh::MeshBrushSettings stroke_brush(const StrokeShape& shape, int i) {
    mesh::MeshBrushSettings s;
    s.radius = shape.radius;
    s.strength = shape.strength;
    const float t = -0.3f + 0.6f * static_cast<float>(i) / static_cast<float>(shape.stamps);
    const cfloat3 side = std::abs(shape.axis.x) < 0.5f ? cf3(1, 0, 0) : cf3(0, 1, 0);
    s.center = cnormalize(shape.axis + side * t);
    return s;
}

DynamicTopologySettings stroke_topology(const StrokeShape& shape) {
    DynamicTopologySettings topo;
    topo.detail_mode = mesh::DynamicDetailMode::BrushRelative;
    topo.detail_resolution = shape.detail;
    return topo;
}

std::size_t run_stroke(DynamicSculptor& sculptor, const StrokeShape& shape,
                       TopologyDelta* record) {
    const DynamicTopologySettings topo = stroke_topology(shape);
    std::size_t ops = 0;
    for (int i = 0; i < shape.stamps; ++i) {
        const auto r =
            sculptor.stamp(mesh::MeshBrush::Draw, stroke_brush(shape, i), topo, {}, record);
        ops += r.remesh.total();
    }
    return ops;
}

bool same_bits(const cfloat3& a, const cfloat3& b) {
    return std::memcmp(&a.x, &b.x, sizeof(float)) == 0 &&
           std::memcmp(&a.y, &b.y, sizeof(float)) == 0 &&
           std::memcmp(&a.z, &b.z, sizeof(float)) == 0;
}

// How many recorded `after` states disagree with the live surface. Zero is the
// requirement: the record's end IS the surface immediately after capture.
struct AfterMismatch {
    std::size_t vertex_normals = 0;
    std::size_t vertex_other = 0;
    std::size_t face_normals = 0;
    std::size_t face_other = 0;
};

AfterMismatch after_mismatch(const TopologyDelta& d, const DynamicSurface& s) {
    AfterMismatch out;
    for (const auto& e : d.vertex_entries()) {
        const mesh::DynamicVertex* live = s.vertex(e.after_id);
        if (e.exists_after != (live != nullptr)) {
            ++out.vertex_other;
            continue;
        }
        if (!live) continue;
        if (!same_bits(live->normal, e.after.normal)) ++out.vertex_normals;
        if (!same_bits(live->position, e.after.position) ||
            !same_bits(live->color, e.after.color) || live->outgoing != e.after.outgoing)
            ++out.vertex_other;
    }
    for (const auto& e : d.face_entries()) {
        const mesh::DynamicFace* live = s.face(e.after_id);
        if (e.exists_after != (live != nullptr)) {
            ++out.face_other;
            continue;
        }
        if (!live) continue;
        if (!same_bits(live->normal, e.after.normal)) ++out.face_normals;
        if (live->halfedge != e.after.halfedge) ++out.face_other;
    }
    return out;
}

// The export, compared byte for byte: positions, NORMALS and indices.
std::size_t normal_differences(const Mesh& a, const Mesh& b) {
    if (a.normals.size() != b.normals.size()) return a.normals.size() + b.normals.size() + 1;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.normals.size(); ++i)
        if (!same_bits(a.normals[i], b.normals[i])) ++n;
    return n;
}

bool same_export(const Mesh& a, const Mesh& b) {
    if (a.positions.size() != b.positions.size() || a.indices.size() != b.indices.size())
        return false;
    for (std::size_t i = 0; i < a.positions.size(); ++i)
        if (!same_bits(a.positions[i], b.positions[i])) return false;
    if (a.indices != b.indices) return false;
    return normal_differences(a, b) == 0;
}

}  // namespace

TEST_CASE("dynamic replay: a stroke with relax on leaves an exact record, normals included") {
    // Measurement 2 of the proposal, turned into an assertion. Before the fix
    // the rows below read 346 / 760 / 2,785 / 16 / 572 / 251 vertex normals
    // wrong on redo, and 14 on undo for the last shape; with
    // `relax_after_remesh` off every count was zero.
    const StrokeShape shapes[] = {
        {32, 0.3f, 0.3f, 8.0f},  {32, 0.3f, 0.0f, 8.0f},  {16, 0.3f, 0.0f, 16.0f},
        {48, 0.15f, 0.05f, 12.0f}, {32, 0.3f, -0.3f, 8.0f}, {24, 0.4f, 0.6f, 6.0f},
    };
    for (const StrokeShape& shape : shapes) {
        CAPTURE(shape.stamps);
        CAPTURE(shape.detail);
        CAPTURE(shape.strength);
        auto surface = DynamicSurface::from_mesh(cube_sphere(32, 1.0f));
        REQUIRE(surface.has_value());
        DynamicSculptor sculptor(*surface);
        const Mesh before = surface->to_mesh();

        TopologyDelta record;
        const std::size_t ops = run_stroke(sculptor, shape, &record);
        REQUIRE(ops > 0);
        const Mesh after = surface->to_mesh();

        // The record's end IS the live surface, right after capture.
        const AfterMismatch m = after_mismatch(record, *surface);
        CHECK(m.vertex_normals == 0);
        CHECK(m.face_normals == 0);
        CHECK(m.vertex_other == 0);
        CHECK(m.face_other == 0);

        // Undo and redo reproduce the exported normals of both ends exactly.
        REQUIRE(record.revert(*surface));
        CHECK(normal_differences(surface->to_mesh(), before) == 0);
        CHECK(same_export(surface->to_mesh(), before));
        CHECK(mesh::validate_dynamic_surface(*surface).ok);
        REQUIRE(record.apply(*surface));
        CHECK(normal_differences(surface->to_mesh(), after) == 0);
        CHECK(same_export(surface->to_mesh(), after));
        CHECK(mesh::validate_dynamic_surface(*surface).ok);
    }
}

TEST_CASE("dynamic replay: eight strokes undo in reverse and redo in order, normals exact") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(32, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    const cfloat3 axes[8] = {cf3(0, 0, 1),  cf3(0, 0, -1), cf3(1, 0, 0),  cf3(-1, 0, 0),
                             cf3(0, 1, 0.2f), cf3(0, -1, 0.2f), cf3(0.6f, 0, 0.8f),
                             cf3(-0.6f, 0, 0.8f)};
    std::vector<Mesh> exports{surface->to_mesh()};
    std::vector<TopologyDelta> records(8);
    for (int k = 0; k < 8; ++k) {
        StrokeShape shape{12, 0.3f, 0.3f, 8.0f, cnormalize(axes[k])};
        run_stroke(sculptor, shape, &records[k]);
        exports.push_back(surface->to_mesh());
    }
    std::size_t undo_wrong = 0, redo_wrong = 0;
    for (int k = 7; k >= 0; --k) {
        REQUIRE(records[k].revert(*surface));
        undo_wrong += normal_differences(surface->to_mesh(), exports[k]);
        CHECK(same_export(surface->to_mesh(), exports[k]));
    }
    for (int k = 0; k < 8; ++k) {
        REQUIRE(records[k].apply(*surface));
        redo_wrong += normal_differences(surface->to_mesh(), exports[k + 1]);
        CHECK(same_export(surface->to_mesh(), exports[k + 1]));
    }
    CHECK(undo_wrong == 0);
    CHECK(redo_wrong == 0);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}
