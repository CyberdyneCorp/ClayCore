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
using mesh::RecordedGesture;
using mesh::ReplayDirection;
using mesh::ReplayResult;
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

// Every live face is in exactly one chunk, and the index holds nothing else.
struct IndexCoverage {
    std::size_t live_missing = 0;
    std::size_t dead_indexed = 0;
};

[[maybe_unused]] IndexCoverage index_coverage(const DynamicSculptor& sculptor) {
    IndexCoverage out;
    const DynamicSurface& s = sculptor.surface();
    const mesh::DynamicBvh& bvh = sculptor.bvh();
    s.faces().for_each_live([&](mesh::FaceId f, const mesh::DynamicFace&) {
        if (bvh.leaf_of(f) == mesh::DynamicBvh::kNoLeaf) ++out.live_missing;
    });
    for (std::size_t i = 0; i < bvh.leaf_count(); ++i) {
        const mesh::SurfaceLeaf* leaf = bvh.leaf(static_cast<std::uint32_t>(i));
        if (!leaf) continue;
        for (mesh::FaceId f : leaf->faces)
            if (!s.live(f)) ++out.dead_indexed;
    }
    return out;
}

std::size_t run_recorded(DynamicSculptor& sculptor, const StrokeShape& shape,
                         RecordedGesture* record) {
    const DynamicTopologySettings topo = stroke_topology(shape);
    std::size_t ops = 0;
    for (int i = 0; i < shape.stamps; ++i) {
        const auto r = sculptor.stamp_recorded(mesh::MeshBrush::Draw, stroke_brush(shape, i), topo,
                                               {}, *record);
        REQUIRE(r.has_value());
        ops += r->remesh.total();
    }
    return ops;
}

struct Revisions {
    std::uint64_t topology, geometry, attributes;
    bool operator==(const Revisions& o) const {
        return topology == o.topology && geometry == o.geometry && attributes == o.attributes;
    }
};

Revisions revisions_of(const DynamicSurface& s) {
    return {s.topology_revision(), s.geometry_revision(), s.attribute_revision()};
}

const StrokeShape kNorth{16, 0.35f, 0.3f, 8.0f, cf3(0, 0, 1)};
const StrokeShape kSouth{16, 0.35f, 0.3f, 8.0f, cf3(0, 0, -1)};

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

TEST_CASE("dynamic replay: the same stroke after an undo repeats the first, index in step") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    const DynamicSurface pristine = *surface;
    DynamicSculptor sculptor(*surface);
    const Mesh before = surface->to_mesh();

    RecordedGesture record;
    REQUIRE(run_recorded(sculptor, kNorth, &record) > 0);
    const Mesh first = surface->to_mesh();

    REQUIRE(sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), before));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    // EVERY LIVE FACE IS REACHABLE, and nothing dead is indexed. Without the
    // reindex this read 200 missing on this fixture.
    IndexCoverage cov = index_coverage(sculptor);
    CHECK(cov.live_missing == 0);
    CHECK(cov.dead_indexed == 0);

    // The same stroke again, on the SAME sculptor, lands where a fresh one does.
    RecordedGesture again;
    run_recorded(sculptor, kNorth, &again);
    DynamicSurface reference = pristine;
    DynamicSculptor fresh(reference);
    RecordedGesture unused;
    run_recorded(fresh, kNorth, &unused);
    CHECK(same_export(reference.to_mesh(), first));
    CHECK(same_export(surface->to_mesh(), first));
    cov = index_coverage(sculptor);
    CHECK(cov.live_missing == 0);
    CHECK(cov.dead_indexed == 0);

    // Redo of the second record after undoing it, for the apply direction.
    REQUIRE(sculptor.replay(again, ReplayDirection::Revert) == ReplayResult::Applied);
    REQUIRE(sculptor.replay(again, ReplayDirection::Apply) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), first));
    cov = index_coverage(sculptor);
    CHECK(cov.live_missing == 0);
    CHECK(cov.dead_indexed == 0);
}

TEST_CASE("dynamic replay: an undo marks the chunks it changed dirty") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    RecordedGesture record;
    run_recorded(sculptor, kNorth, &record);
    sculptor.bvh().clear_dirty();
    REQUIRE(sculptor.bvh().dirty_leaves().empty());
    REQUIRE(sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::Applied);
    // Local: some chunks, not all of them.
    CHECK_FALSE(sculptor.bvh().dirty_leaves().empty());
    CHECK(sculptor.bvh().dirty_leaves().size() < sculptor.bvh().leaf_count());
}

TEST_CASE("dynamic replay: a replay out of order is refused and changes nothing") {
    // Two strokes on OPPOSITE hemispheres. They do not overlap in space, and
    // they still share slots: the later reuses what the earlier freed.
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    const Mesh start = surface->to_mesh();
    RecordedGesture a, b;
    run_recorded(sculptor, kNorth, &a);
    const Mesh after_a = surface->to_mesh();
    run_recorded(sculptor, kSouth, &b);
    const Mesh after_b = surface->to_mesh();

    const Revisions revs = revisions_of(*surface);
    CHECK(sculptor.replay(a, ReplayDirection::Revert) == ReplayResult::Mismatch);
    CHECK(sculptor.replay(b, ReplayDirection::Apply) == ReplayResult::NoOp);
    CHECK(sculptor.replay(a, ReplayDirection::Apply) == ReplayResult::Mismatch);
    CHECK(revisions_of(*surface) == revs);
    CHECK(same_export(surface->to_mesh(), after_b));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);

    // Last in, first out works.
    REQUIRE(sculptor.replay(b, ReplayDirection::Revert) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), after_a));
    REQUIRE(sculptor.replay(a, ReplayDirection::Revert) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), start));
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
    REQUIRE(sculptor.replay(a, ReplayDirection::Apply) == ReplayResult::Applied);
    REQUIRE(sculptor.replay(b, ReplayDirection::Apply) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), after_b));
    const IndexCoverage cov = index_coverage(sculptor);
    CHECK(cov.live_missing == 0);
    CHECK(cov.dead_indexed == 0);
}

TEST_CASE("dynamic replay: an unrecorded edit in between is refused, capture included") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    RecordedGesture record;
    run_recorded(sculptor, kNorth, &record);

    // A stamp with no record.
    const DynamicTopologySettings topo = stroke_topology(kSouth);
    REQUIRE(sculptor.stamp(mesh::MeshBrush::Draw, stroke_brush(kSouth, 0), topo, {}, nullptr)
                .changed());
    const Mesh edited = surface->to_mesh();
    const Revisions revs = revisions_of(*surface);
    CHECK(sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::Mismatch);
    CHECK(revisions_of(*surface) == revs);
    CHECK(same_export(surface->to_mesh(), edited));

    // Capturing more into the record now would join two unrelated histories.
    CHECK_FALSE(sculptor
                    .stamp_recorded(mesh::MeshBrush::Draw, stroke_brush(kNorth, 0), topo, {},
                                    record)
                    .has_value());
    CHECK(revisions_of(*surface) == revs);

    // A record from ANOTHER surface in the same state is refused too.
    auto other = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(other.has_value());
    DynamicSculptor other_sculptor(*other);
    CHECK(other_sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::Mismatch);
    CHECK_FALSE(other_sculptor
                    .stamp_recorded(mesh::MeshBrush::Draw, stroke_brush(kNorth, 0), topo, {},
                                    record)
                    .has_value());

    // A cleared record binds afresh.
    record.clear();
    CHECK(sculptor
              .stamp_recorded(mesh::MeshBrush::Draw, stroke_brush(kNorth, 0), topo, {}, record)
              .has_value());
}

TEST_CASE("dynamic replay: an undone stroke's epochs are never handed out again") {
    // THE MARK MUST NOT BE A COUNTER THAT REWINDS WITH THE UNDO. Revert A puts
    // the surface back at A's `before`; a different stroke B from there takes
    // as many bumps as A did. Were the epoch `before + bumps`, B would end on
    // A's `after`, and reverting A would "match" a surface that is B.
    auto surface = DynamicSurface::from_mesh(cube_sphere(24, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    // Deformation only, so each stamp is exactly one bump and the two strokes
    // below take the same number: the case a rewinding counter gets wrong.
    DynamicTopologySettings topo = stroke_topology(kNorth);
    topo.enabled = false;
    auto stroke = [&](float strength, RecordedGesture* record) {
        for (int i = 0; i < kNorth.stamps; ++i) {
            mesh::MeshBrushSettings brush = stroke_brush(kNorth, i);
            brush.strength = strength;
            REQUIRE(sculptor.stamp_recorded(mesh::MeshBrush::Draw, brush, topo, {}, *record)
                        ->moved_vertices > 0);
        }
    };
    RecordedGesture a;
    stroke(0.3f, &a);
    REQUIRE(sculptor.replay(a, ReplayDirection::Revert) == ReplayResult::Applied);

    // B: as many stamps and bumps as A, and a different surface.
    RecordedGesture b;
    stroke(0.1f, &b);
    CHECK(surface->mark() != a.after());
    CHECK(sculptor.replay(a, ReplayDirection::Revert) == ReplayResult::Mismatch);
    CHECK(sculptor.replay(a, ReplayDirection::Apply) == ReplayResult::Mismatch);
    // And the two records still compose last in, first out.
    REQUIRE(sculptor.replay(b, ReplayDirection::Revert) == ReplayResult::Applied);
    REQUIRE(sculptor.replay(a, ReplayDirection::Apply) == ReplayResult::Applied);
    CHECK(mesh::validate_dynamic_surface(*surface).ok);
}

TEST_CASE("dynamic replay: reverting twice is reverting once") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    RecordedGesture record;
    run_recorded(sculptor, kNorth, &record);
    REQUIRE(sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::Applied);
    const Revisions revs = revisions_of(*surface);
    sculptor.bvh().clear_dirty();
    CHECK(sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::NoOp);
    CHECK(revisions_of(*surface) == revs);
    CHECK(sculptor.bvh().dirty_leaves().empty());

    // A stamp that changed nothing advances no epoch, so it does not break the
    // history either.
    mesh::MeshBrushSettings miss = stroke_brush(kNorth, 0);
    miss.center = cf3(0, 0, 5);
    const mesh::SurfaceMark mark = surface->mark();
    CHECK_FALSE(sculptor.stamp(mesh::MeshBrush::Draw, miss, stroke_topology(kNorth), {}, nullptr)
                    .changed());
    CHECK(surface->mark() == mark);
    CHECK(sculptor.replay(record, ReplayDirection::Apply) == ReplayResult::Applied);

    // An empty record replays as nothing, bound or not.
    RecordedGesture empty;
    CHECK(sculptor.replay(empty, ReplayDirection::Revert) == ReplayResult::NoOp);
}

TEST_CASE("dynamic replay: a record spills to bytes and replays identically") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(16, 1.0f));
    REQUIRE(surface.has_value());
    DynamicSculptor sculptor(*surface);
    const Mesh start = surface->to_mesh();
    RecordedGesture record;
    run_recorded(sculptor, kNorth, &record);
    const Mesh stroked = surface->to_mesh();

    // THE BYTE COST IS A COUNT: exact, and the documented formula.
    const TopologyDelta& d = record.delta();
    const std::vector<std::uint8_t> bytes = record.encode();
    CHECK(bytes.size() == record.encoded_size());
    CHECK(d.encode().size() == d.encoded_size());
    CHECK(bytes.size() == 56 + 122 * d.vertex_count() + 114 * d.halfedge_count() +
                              42 * d.edge_count() + 66 * d.face_count());
    // Resident is a floor, not an exact figure: every entry in memory, before
    // any capacity slack or slot map.
    const std::size_t entries_in_memory =
        d.vertex_count() * sizeof(d.vertex_entries().front()) +
        d.halfedge_count() * sizeof(d.halfedge_entries().front()) +
        d.edge_count() * sizeof(d.edge_entries().front()) +
        d.face_count() * sizeof(d.face_entries().front());
    CHECK(record.bytes() >= entries_in_memory);

    RecordedGesture loaded;
    REQUIRE(RecordedGesture::decode(bytes.data(), bytes.size(), &loaded) ==
            mesh::GestureDecode::Ok);
    CHECK(loaded.before() == record.before());
    CHECK(loaded.after() == record.after());
    CHECK(loaded.encode() == bytes);
    REQUIRE(sculptor.replay(loaded, ReplayDirection::Revert) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), start));
    REQUIRE(sculptor.replay(record, ReplayDirection::Apply) == ReplayResult::Applied);
    CHECK(same_export(surface->to_mesh(), stroked));

    // Refusals: truncated, trailing, wrong magic, and newer versions of either
    // layer -- which are told apart from damage.
    RecordedGesture untouched;
    CHECK(RecordedGesture::decode(bytes.data(), bytes.size() - 1, &untouched) ==
          mesh::GestureDecode::Malformed);
    CHECK(RecordedGesture::decode(bytes.data(), 20, &untouched) ==
          mesh::GestureDecode::Malformed);
    std::vector<std::uint8_t> longer = bytes;
    longer.push_back(0);
    CHECK(RecordedGesture::decode(longer.data(), longer.size(), &untouched) ==
          mesh::GestureDecode::Malformed);
    std::vector<std::uint8_t> bad = bytes;
    bad[0] ^= 0xff;
    CHECK(RecordedGesture::decode(bad.data(), bad.size(), &untouched) ==
          mesh::GestureDecode::Malformed);
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 2;
    CHECK(RecordedGesture::decode(newer.data(), newer.size(), &untouched) ==
          mesh::GestureDecode::ForwardVersion);
    std::vector<std::uint8_t> newer_inner = bytes;
    newer_inner[RecordedGesture::kHeaderBytes + 4] = 2;
    CHECK(RecordedGesture::decode(newer_inner.data(), newer_inner.size(), &untouched) ==
          mesh::GestureDecode::ForwardVersion);
    // A hostile count is refused before anything is sized from it.
    std::vector<std::uint8_t> hostile = bytes;
    hostile[RecordedGesture::kHeaderBytes + 8] = 0xff;
    hostile[RecordedGesture::kHeaderBytes + 11] = 0x7f;
    CHECK(RecordedGesture::decode(hostile.data(), hostile.size(), &untouched) ==
          mesh::GestureDecode::Malformed);
    CHECK(untouched.empty());

    // A surface decoded from bytes is a new lineage: no record replays onto it.
    const std::vector<std::uint8_t> surface_bytes = surface->encode();
    DynamicSurface reloaded;
    REQUIRE(DynamicSurface::decode(surface_bytes.data(), surface_bytes.size(), &reloaded));
    DynamicSculptor reloaded_sculptor(reloaded);
    CHECK(reloaded_sculptor.replay(record, ReplayDirection::Revert) == ReplayResult::Mismatch);
}
