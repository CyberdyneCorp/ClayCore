// Undo for an adaptive stroke across the C ABI (c-abi spec,
// undo-a-dynamic-stroke-across-the-abi).
//
// Every claim here is asserted as a COUNT or as bytes, never as a clock:
//
//   - a revert gives back the export taken before the stroke, byte for byte,
//     normals included, and an apply the export taken after it;
//   - the byte cost of a record is the documented formula, exactly, and it is
//     what the serialize size query returns;
//   - a replay onto a state the record does not end at is
//     CLAY_ERROR_SNAPSHOT_MISMATCH with nothing written;
//   - after an undo the dirty-chunk stream still reconstructs the surface, and
//     the same stroke again lands where the first one did.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "clay.h"

namespace {

void cube_sphere(int n, float radius, std::vector<float>* positions,
                 std::vector<uint32_t>* indices) {
    positions->clear();
    indices->clear();
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = static_cast<uint32_t>(positions->size() / 3);
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                for (int k = 0; k < 3; ++k) positions->push_back(c[k] / len * radius);
            }
        const uint32_t stride = static_cast<uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const uint32_t a =
                    base + static_cast<uint32_t>(v) * stride + static_cast<uint32_t>(u);
                const uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    indices->insert(indices->end(), {a, c2, b, b, c2, d});
                else
                    indices->insert(indices->end(), {a, b, c2, b, d, c2});
            }
    }
}

clay_dynamic_surface* make_surface(int n) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    cube_sphere(n, 1.0f, &positions, &indices);
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &mesh) == CLAY_OK);
    clay_dynamic_surface* surface = nullptr;
    int32_t err = -1;
    REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    clay_mesh_destroy(mesh);
    return surface;
}

struct Fixture {
    clay_dynamic_surface* surface = nullptr;
    clay_dynamic_sculptor* sculptor = nullptr;

    explicit Fixture(int n = 24) {
        surface = make_surface(n);
        REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
    }
    ~Fixture() {
        clay_dynamic_sculptor_destroy(sculptor);
        clay_dynamic_surface_destroy(surface);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

struct Record {
    clay_dynamic_delta* delta = clay_dynamic_delta_create();
    ~Record() { clay_dynamic_delta_destroy(delta); }
    Record() = default;
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
};

// What a host sees, as bytes. `to_mesh` gives positions and indices, and NO
// normals for a surface built from a mesh that had none -- which is every mesh
// clay_mesh_from_triangles builds. The stored normals a host draws with come
// through the chunk copy, so they are read there: every chunk's triangles, each
// as its nine position floats and nine normal floats, sorted so the comparison
// does not depend on how the triangles are partitioned into chunks.
using Shaded = std::array<uint32_t, 18>;

struct Export {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    std::vector<Shaded> shaded;
};

void append_chunk(const clay_dynamic_sculptor* sculptor, size_t index, std::vector<Shaded>* out) {
    clay_dynamic_chunk_info info{};
    info.struct_size = sizeof(info);
    REQUIRE(clay_dynamic_surface_copy_chunk(sculptor, index, nullptr, 0, nullptr, 0, nullptr, 0,
                                            &info) == CLAY_OK);
    if (info.vertex_count == 0) return;
    std::vector<float> p(info.vertex_count * 3), n(info.vertex_count * 3);
    std::vector<uint32_t> idx(info.index_count);
    clay_dynamic_chunk_info written{};
    written.struct_size = sizeof(written);
    REQUIRE(clay_dynamic_surface_copy_chunk(sculptor, index, p.data(), p.size(), n.data(), n.size(),
                                            idx.data(), idx.size(), &written) == CLAY_OK);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        Shaded key{};
        for (int k = 0; k < 3; ++k) {
            std::memcpy(&key[k * 3], &p[idx[t + k] * 3], 3 * sizeof(float));
            std::memcpy(&key[9 + k * 3], &n[idx[t + k] * 3], 3 * sizeof(float));
        }
        out->push_back(key);
    }
}

Export export_of(const clay_dynamic_surface* surface, const clay_dynamic_sculptor* sculptor) {
    clay_mesh* m = nullptr;
    REQUIRE(clay_dynamic_surface_to_mesh(surface, &m) == CLAY_OK);
    Export out;
    const size_t v = clay_mesh_vertex_count(m), i = clay_mesh_index_count(m);
    const float* p = clay_mesh_positions(m);
    const uint32_t* idx = clay_mesh_indices(m);
    if (p) out.positions.assign(p, p + v * 3);
    if (idx) out.indices.assign(idx, idx + i);
    clay_mesh_destroy(m);
    const size_t chunks = clay_dynamic_surface_chunk_count(sculptor);
    for (size_t c = 0; c < chunks; ++c) append_chunk(sculptor, c, &out.shaded);
    std::sort(out.shaded.begin(), out.shaded.end());
    REQUIRE(out.shaded.size() * 3 == out.indices.size());
    return out;
}

// BYTE comparison, so -0.0 against 0.0 and a NaN payload count as different.
bool same_bytes(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// How many shaded triangles one side has that the other does not. With the
// positions equal, every one of them is a triangle whose NORMALS differ.
std::size_t normal_differences(const Export& a, const Export& b) {
    std::vector<Shaded> diff;
    std::set_symmetric_difference(a.shaded.begin(), a.shaded.end(), b.shaded.begin(),
                                  b.shaded.end(), std::back_inserter(diff));
    return diff.size();
}

bool same_export(const Export& a, const Export& b) {
    return same_bytes(a.positions, b.positions) && a.indices == b.indices &&
           normal_differences(a, b) == 0;
}

bool valid(const clay_dynamic_surface* surface) {
    int32_t ok = 0;
    size_t len = 0;
    REQUIRE(clay_dynamic_surface_validate(surface, &ok, nullptr, &len) == CLAY_OK);
    return ok == 1;
}

clay_surface_revision revision_of(const clay_dynamic_surface* surface) {
    clay_surface_revision r{};
    r.struct_size = sizeof(r);
    REQUIRE(clay_dynamic_surface_revision(surface, &r) == CLAY_OK);
    return r;
}

bool same_revision(const clay_surface_revision& a, const clay_surface_revision& b) {
    return a.topology == b.topology && a.geometry == b.geometry && a.attributes == b.attributes;
}

// A Draw stroke along a short arc about `axis`, with the topology DEFAULTS, so
// the relax pass is on -- the configuration the record used to get wrong.
struct Stroke {
    float axis[3] = {0.0f, 0.0f, 1.0f};
    int stamps = 12;
    float strength = 0.3f;
};

clay_mesh_brush_desc stamp_brush(const Stroke& s, int i) {
    clay_mesh_brush_desc b{};
    b.struct_size = sizeof(b);
    REQUIRE(clay_mesh_brush_defaults(&b) == CLAY_OK);
    b.verb = CLAY_MESH_BRUSH_DRAW;
    b.radius = 0.3f;
    b.strength = s.strength;
    const float t = -0.3f + 0.6f * static_cast<float>(i) / static_cast<float>(s.stamps);
    const bool x_major = std::fabs(s.axis[0]) >= 0.5f;
    float c[3] = {s.axis[0] + (x_major ? 0.0f : t), s.axis[1] + (x_major ? t : 0.0f), s.axis[2]};
    const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    for (int k = 0; k < 3; ++k) b.center[k] = c[k] / len;
    return b;
}

clay_dynamic_topology_desc stroke_topology() {
    clay_dynamic_topology_desc t{};
    t.struct_size = sizeof(t);
    REQUIRE(clay_dynamic_topology_defaults(&t) == CLAY_OK);
    REQUIRE(t.relax_after_remesh == 1);
    t.detail_mode = CLAY_DETAIL_BRUSH_RELATIVE;
    t.detail_resolution = 8.0f;
    return t;
}

// Returns how many edges the stroke split, so a caller can require that the
// stroke really changed topology.
uint64_t stroke(clay_dynamic_sculptor* sculptor, const Stroke& s, clay_dynamic_delta* record) {
    const clay_dynamic_topology_desc topo = stroke_topology();
    uint64_t splits = 0;
    for (int i = 0; i < s.stamps; ++i) {
        const clay_mesh_brush_desc b = stamp_brush(s, i);
        clay_dynamic_stamp_report report{};
        report.struct_size = sizeof(report);
        REQUIRE(clay_dynamic_sculptor_stamp_recorded(sculptor, &b, &topo, nullptr, record,
                                                     &report) == CLAY_OK);
        splits += report.split_edges;
    }
    return splits;
}

clay_dynamic_delta_stats stats_of(const clay_dynamic_delta* delta) {
    clay_dynamic_delta_stats s{};
    s.struct_size = sizeof(s);
    REQUIRE(clay_dynamic_delta_stats_get(delta, &s) == CLAY_OK);
    return s;
}

std::vector<uint8_t> serialize(const clay_dynamic_delta* delta) {
    size_t n = 0;
    REQUIRE(clay_dynamic_delta_serialize(delta, nullptr, &n) == CLAY_OK);
    std::vector<uint8_t> bytes(n);
    size_t written = n;
    REQUIRE(clay_dynamic_delta_serialize(delta, bytes.data(), &written) == CLAY_OK);
    REQUIRE(written == n);
    return bytes;
}

// A host's view of the surface, kept ONLY from the chunk stream: every chunk on
// the first frame, then the dirty ones. Triangles are keyed by their exact
// position bytes, so the comparison is about which triangles, not their order.
using Tri = std::array<uint32_t, 9>;

Tri tri_key(const float* p) {
    Tri key{};
    for (int i = 0; i < 9; ++i) std::memcpy(&key[i], &p[i], sizeof(float));
    return key;
}

struct ChunkCache {
    std::map<size_t, std::vector<Tri>> chunks;

    void refresh(const clay_dynamic_sculptor* sculptor, size_t index) {
        clay_dynamic_chunk_info info{};
        info.struct_size = sizeof(info);
        REQUIRE(clay_dynamic_surface_copy_chunk(sculptor, index, nullptr, 0, nullptr, 0, nullptr, 0,
                                                &info) == CLAY_OK);
        std::vector<Tri>& tris = chunks[index];
        tris.clear();
        if (info.vertex_count == 0) return;
        std::vector<float> positions(info.vertex_count * 3);
        std::vector<uint32_t> indices(info.index_count);
        clay_dynamic_chunk_info written{};
        written.struct_size = sizeof(written);
        REQUIRE(clay_dynamic_surface_copy_chunk(sculptor, index, positions.data(),
                                                positions.size(), nullptr, 0, indices.data(),
                                                indices.size(), &written) == CLAY_OK);
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            float p[9];
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) p[k * 3 + a] = positions[indices[t + k] * 3 + a];
            tris.push_back(tri_key(p));
        }
    }

    void load_all(clay_dynamic_sculptor* sculptor) {
        chunks.clear();
        const size_t n = clay_dynamic_surface_chunk_count(sculptor);
        for (size_t c = 0; c < n; ++c) refresh(sculptor, c);
        REQUIRE(clay_dynamic_surface_clear_dirty(sculptor) == CLAY_OK);
    }

    // Returns how many chunks were dirty.
    size_t drain(clay_dynamic_sculptor* sculptor) {
        size_t count = 0;
        REQUIRE(clay_dynamic_surface_dirty_chunks(sculptor, nullptr, &count) == CLAY_OK);
        std::vector<uint32_t> dirty(count);
        if (count > 0)
            REQUIRE(clay_dynamic_surface_dirty_chunks(sculptor, dirty.data(), &count) == CLAY_OK);
        for (uint32_t c : dirty) refresh(sculptor, c);
        REQUIRE(clay_dynamic_surface_clear_dirty(sculptor) == CLAY_OK);
        return count;
    }

    std::multiset<Tri> all() const {
        std::multiset<Tri> out;
        for (const auto& kv : chunks) out.insert(kv.second.begin(), kv.second.end());
        return out;
    }
};

std::multiset<Tri> triangles_of(const Export& e) {
    std::multiset<Tri> out;
    for (size_t t = 0; t + 2 < e.indices.size(); t += 3) {
        float p[9];
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) p[k * 3 + a] = e.positions[e.indices[t + k] * 3 + a];
        out.insert(tri_key(p));
    }
    return out;
}

const Stroke kNorth{{0.0f, 0.0f, 1.0f}};
const Stroke kSouth{{0.0f, 0.0f, -1.0f}};

}  // namespace

TEST_CASE("c dynamic delta: undo and redo give back the exports, byte for byte") {
    Fixture fx;
    const Export before = export_of(fx.surface, fx.sculptor);
    Record record;
    REQUIRE(stroke(fx.sculptor, kNorth, record.delta) > 0);
    const Export after = export_of(fx.surface, fx.sculptor);
    REQUIRE_FALSE(same_export(before, after));

    REQUIRE(clay_dynamic_delta_revert(record.delta, fx.sculptor) == CLAY_OK);
    const Export undone = export_of(fx.surface, fx.sculptor);
    CHECK(normal_differences(undone, before) == 0);
    CHECK(same_export(undone, before));
    CHECK(valid(fx.surface));

    REQUIRE(clay_dynamic_delta_apply(record.delta, fx.sculptor) == CLAY_OK);
    const Export redone = export_of(fx.surface, fx.sculptor);
    CHECK(normal_differences(redone, after) == 0);
    CHECK(same_export(redone, after));
    CHECK(valid(fx.surface));
}

TEST_CASE("c dynamic delta: eight strokes undone in reverse and redone in order") {
    Fixture fx(32);
    const float axes[8][3] = {{0, 0, 1},    {0, 0, -1},   {1, 0, 0},     {-1, 0, 0},
                              {0, 1, 0.2f}, {0, -1, 0.2f}, {0.6f, 0, 0.8f}, {-0.6f, 0, 0.8f}};
    std::vector<Export> exports{export_of(fx.surface, fx.sculptor)};
    std::vector<Record> records(8);
    for (int k = 0; k < 8; ++k) {
        Stroke s;
        std::memcpy(s.axis, axes[k], sizeof(s.axis));
        stroke(fx.sculptor, s, records[k].delta);
        exports.push_back(export_of(fx.surface, fx.sculptor));
    }
    std::size_t undo_normals = 0, redo_normals = 0;
    for (int k = 7; k >= 0; --k) {
        CAPTURE(k);
        REQUIRE(clay_dynamic_delta_revert(records[k].delta, fx.sculptor) == CLAY_OK);
        const Export e = export_of(fx.surface, fx.sculptor);
        undo_normals += normal_differences(e, exports[k]);
        CHECK(same_export(e, exports[k]));
    }
    CHECK(valid(fx.surface));
    for (int k = 0; k < 8; ++k) {
        CAPTURE(k);
        REQUIRE(clay_dynamic_delta_apply(records[k].delta, fx.sculptor) == CLAY_OK);
        const Export e = export_of(fx.surface, fx.sculptor);
        redo_normals += normal_differences(e, exports[k + 1]);
        CHECK(same_export(e, exports[k + 1]));
    }
    CHECK(undo_normals == 0);
    CHECK(redo_normals == 0);
    CHECK(valid(fx.surface));
}

TEST_CASE("c dynamic delta: the byte cost is a count") {
    Fixture fx;
    Record record;
    // Empty: the header alone, and no entries.
    clay_dynamic_delta_stats empty = stats_of(record.delta);
    CHECK(empty.vertices + empty.halfedges + empty.edges + empty.faces == 0);
    CHECK(empty.encoded_bytes == 56);

    stroke(fx.sculptor, kNorth, record.delta);
    const clay_dynamic_delta_stats s = stats_of(record.delta);
    CHECK(s.vertices > 0);
    CHECK(s.halfedges > 0);
    CHECK(s.edges > 0);
    CHECK(s.faces > 0);
    CHECK(s.encoded_bytes ==
          56 + 122 * s.vertices + 114 * s.halfedges + 42 * s.edges + 66 * s.faces);

    size_t query = 0;
    REQUIRE(clay_dynamic_delta_serialize(record.delta, nullptr, &query) == CLAY_OK);
    CHECK(query == s.encoded_bytes);
    // Held in memory, every entry is at least as wide as its encoding.
    CHECK(s.resident_bytes >= s.encoded_bytes - 56);

    // A short buffer is a SIZE answer, retryable, and writes nothing.
    std::vector<uint8_t> tiny(16, 0xAB);
    size_t cap = tiny.size();
    CHECK(clay_dynamic_delta_serialize(record.delta, tiny.data(), &cap) ==
          CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(cap == s.encoded_bytes);
    CHECK(tiny[0] == 0xAB);
    // No count to report into is a malformed call, not a size answer.
    CHECK(clay_dynamic_delta_serialize(record.delta, tiny.data(), nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_dynamic_delta_serialize(record.delta, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // The stats struct is negotiated by size, and a short one is refused.
    clay_dynamic_delta_stats shorter{};
    shorter.struct_size = offsetof(clay_dynamic_delta_stats, resident_bytes);
    CHECK(clay_dynamic_delta_stats_get(record.delta, &shorter) == CLAY_ERROR_INVALID_ARGUMENT);

    REQUIRE(clay_dynamic_delta_clear(record.delta) == CLAY_OK);
    const clay_dynamic_delta_stats cleared = stats_of(record.delta);
    CHECK(cleared.vertices + cleared.halfedges + cleared.edges + cleared.faces == 0);
    CHECK(cleared.encoded_bytes == 56);
}

TEST_CASE("c dynamic delta: a replay onto the wrong state is refused and writes nothing") {
    Fixture fx;
    const Export start = export_of(fx.surface, fx.sculptor);
    Record a, b;
    stroke(fx.sculptor, kNorth, a.delta);
    const Export after_a = export_of(fx.surface, fx.sculptor);
    // The far hemisphere: no overlap in space, and still not independent.
    stroke(fx.sculptor, kSouth, b.delta);
    const Export after_b = export_of(fx.surface, fx.sculptor);

    const clay_surface_revision revs = revision_of(fx.surface);
    CHECK(clay_dynamic_delta_revert(a.delta, fx.sculptor) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(clay_dynamic_delta_apply(a.delta, fx.sculptor) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(same_revision(revision_of(fx.surface), revs));
    CHECK(same_export(export_of(fx.surface, fx.sculptor), after_b));
    CHECK(valid(fx.surface));

    // Already at the target: OK, and nothing advances.
    CHECK(clay_dynamic_delta_apply(b.delta, fx.sculptor) == CLAY_OK);
    CHECK(same_revision(revision_of(fx.surface), revs));

    // Last in, first out.
    REQUIRE(clay_dynamic_delta_revert(b.delta, fx.sculptor) == CLAY_OK);
    CHECK(same_export(export_of(fx.surface, fx.sculptor), after_a));
    REQUIRE(clay_dynamic_delta_revert(a.delta, fx.sculptor) == CLAY_OK);
    CHECK(same_export(export_of(fx.surface, fx.sculptor), start));
    CHECK(valid(fx.surface));

    // Reverting twice is reverting once.
    const clay_surface_revision undone = revision_of(fx.surface);
    CHECK(clay_dynamic_delta_revert(a.delta, fx.sculptor) == CLAY_OK);
    CHECK(same_revision(revision_of(fx.surface), undone));
}

TEST_CASE("c dynamic delta: an unrecorded stamp or another surface is refused") {
    Fixture fx;
    Record record;
    stroke(fx.sculptor, kNorth, record.delta);

    // A stamp without a record, through the SHIPPED entry point.
    const clay_dynamic_topology_desc topo = stroke_topology();
    const clay_mesh_brush_desc south = stamp_brush(kSouth, 0);
    REQUIRE(clay_dynamic_sculptor_stamp(fx.sculptor, &south, &topo, nullptr, nullptr) == CLAY_OK);
    const Export edited = export_of(fx.surface, fx.sculptor);
    const clay_surface_revision revs = revision_of(fx.surface);
    CHECK(clay_dynamic_delta_revert(record.delta, fx.sculptor) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(same_revision(revision_of(fx.surface), revs));
    CHECK(same_export(export_of(fx.surface, fx.sculptor), edited));

    // Capturing more would join two histories into one step: refused, and the
    // stamp is not made.
    const clay_mesh_brush_desc north = stamp_brush(kNorth, 0);
    const clay_dynamic_delta_stats held = stats_of(record.delta);
    CHECK(clay_dynamic_sculptor_stamp_recorded(fx.sculptor, &north, &topo, nullptr, record.delta,
                                               nullptr) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(same_revision(revision_of(fx.surface), revs));
    CHECK(stats_of(record.delta).encoded_bytes == held.encoded_bytes);

    // A second surface with IDENTICAL content is still another surface.
    Fixture other;
    CHECK(clay_dynamic_delta_revert(record.delta, other.sculptor) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(clay_dynamic_sculptor_stamp_recorded(other.sculptor, &north, &topo, nullptr,
                                               record.delta, nullptr) ==
          CLAY_ERROR_SNAPSHOT_MISMATCH);

    // A cleared record binds afresh.
    REQUIRE(clay_dynamic_delta_clear(record.delta) == CLAY_OK);
    CHECK(clay_dynamic_sculptor_stamp_recorded(fx.sculptor, &north, &topo, nullptr, record.delta,
                                               nullptr) == CLAY_OK);

    // Null handles are malformed calls, not mismatches.
    CHECK(clay_dynamic_delta_revert(nullptr, fx.sculptor) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_dynamic_delta_apply(record.delta, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_dynamic_delta_clear(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_dynamic_delta_stats_get(nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("c dynamic delta: after an undo the chunk stream and the next stroke are right") {
    Fixture fx;
    ChunkCache host;
    host.load_all(fx.sculptor);
    CHECK(host.all() == triangles_of(export_of(fx.surface, fx.sculptor)));

    Record record;
    stroke(fx.sculptor, kNorth, record.delta);
    CHECK(host.drain(fx.sculptor) > 0);
    const Export first = export_of(fx.surface, fx.sculptor);
    CHECK(host.all() == triangles_of(first));

    // THE UNDO TELLS THE HOST WHAT TO REDRAW, and what it redraws is the model.
    REQUIRE(clay_dynamic_delta_revert(record.delta, fx.sculptor) == CLAY_OK);
    const size_t dirty = host.drain(fx.sculptor);
    CHECK(dirty > 0);
    CHECK(dirty < clay_dynamic_surface_chunk_count(fx.sculptor));
    CHECK(host.all() == triangles_of(export_of(fx.surface, fx.sculptor)));

    // The same stroke again, on the same sculptor, repeats the first exactly.
    Record again;
    stroke(fx.sculptor, kNorth, again.delta);
    CHECK(same_export(export_of(fx.surface, fx.sculptor), first));
    host.drain(fx.sculptor);
    CHECK(host.all() == triangles_of(first));

    // And redo of a stroke after its undo, through the stream.
    REQUIRE(clay_dynamic_delta_revert(again.delta, fx.sculptor) == CLAY_OK);
    host.drain(fx.sculptor);
    REQUIRE(clay_dynamic_delta_apply(again.delta, fx.sculptor) == CLAY_OK);
    host.drain(fx.sculptor);
    CHECK(host.all() == triangles_of(first));
    CHECK(valid(fx.surface));
}

TEST_CASE("c dynamic delta: a record spills to bytes and replays identically") {
    Fixture fx;
    const Export start = export_of(fx.surface, fx.sculptor);
    Record record;
    stroke(fx.sculptor, kNorth, record.delta);
    const Export stroked = export_of(fx.surface, fx.sculptor);

    const std::vector<uint8_t> bytes = serialize(record.delta);
    clay_dynamic_delta* loaded = nullptr;
    REQUIRE(clay_dynamic_delta_deserialize(bytes.data(), bytes.size(), &loaded) == CLAY_OK);
    REQUIRE(loaded != nullptr);
    CHECK(serialize(loaded) == bytes);
    // The ORIGINAL is destroyed; the spilled copy carries the history alone.
    clay_dynamic_delta_destroy(record.delta);
    record.delta = loaded;

    REQUIRE(clay_dynamic_delta_revert(loaded, fx.sculptor) == CLAY_OK);
    CHECK(same_export(export_of(fx.surface, fx.sculptor), start));
    REQUIRE(clay_dynamic_delta_apply(loaded, fx.sculptor) == CLAY_OK);
    CHECK(same_export(export_of(fx.surface, fx.sculptor), stroked));

    clay_dynamic_delta* refused = loaded;  // a non-null sentinel
    CHECK(clay_dynamic_delta_deserialize(bytes.data(), bytes.size() - 1, &refused) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(refused == nullptr);
    CHECK(clay_dynamic_delta_deserialize(bytes.data(), 12, &refused) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_dynamic_delta_deserialize(nullptr, 0, &refused) == CLAY_ERROR_INVALID_ARGUMENT);
    std::vector<uint8_t> newer = bytes;
    newer[4] = 2;  // the wrapper's version
    CHECK(clay_dynamic_delta_deserialize(newer.data(), newer.size(), &refused) ==
          CLAY_ERROR_FORWARD_VERSION);
    CHECK(refused == nullptr);

    // A surface reloaded from its own bytes is a new identity.
    size_t n = 0;
    REQUIRE(clay_dynamic_surface_serialize(fx.surface, nullptr, &n) == CLAY_OK);
    std::vector<uint8_t> surface_bytes(n);
    REQUIRE(clay_dynamic_surface_serialize(fx.surface, surface_bytes.data(), &n) == CLAY_OK);
    clay_dynamic_surface* reloaded = nullptr;
    REQUIRE(clay_dynamic_surface_deserialize(surface_bytes.data(), n, &reloaded) == CLAY_OK);
    clay_dynamic_sculptor* reloaded_sculptor = nullptr;
    REQUIRE(clay_dynamic_sculptor_create(reloaded, &reloaded_sculptor) == CLAY_OK);
    CHECK(clay_dynamic_delta_revert(loaded, reloaded_sculptor) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    clay_dynamic_sculptor_destroy(reloaded_sculptor);
    clay_dynamic_surface_destroy(reloaded);
}

TEST_CASE("c dynamic delta: a null record stamps exactly like the shipped stamp") {
    Fixture a, b;
    const clay_dynamic_topology_desc topo = stroke_topology();
    for (int i = 0; i < kNorth.stamps; ++i) {
        const clay_mesh_brush_desc brush = stamp_brush(kNorth, i);
        REQUIRE(clay_dynamic_sculptor_stamp(a.sculptor, &brush, &topo, nullptr, nullptr) ==
                CLAY_OK);
        REQUIRE(clay_dynamic_sculptor_stamp_recorded(b.sculptor, &brush, &topo, nullptr, nullptr,
                                                     nullptr) == CLAY_OK);
    }
    CHECK(same_export(export_of(a.surface, a.sculptor), export_of(b.surface, b.sculptor)));
}

// -- a whole stroke as one record (record-a-whole-adaptive-stroke) ------------

namespace {

std::vector<clay_stroke_sample_full> samples_along(const float from[3], const float to[3], int n) {
    std::vector<clay_stroke_sample_full> out(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(n - 1);
        clay_stroke_sample_full& s = out[static_cast<std::size_t>(k)];
        s = clay_stroke_sample_full{};
        for (int a = 0; a < 3; ++a) s.position[a] = from[a] + (to[a] - from[a]) * t;
        s.pressure = 1.0f;
    }
    return out;
}

// A remeshing Draw across the pole, and a Snakehook pulling out of it: the drag
// whose meaning (the re-found anchor) a host loop of stamps does not keep.
struct AbiStroke {
    std::vector<clay_stroke_sample_full> samples;
    clay_stroke_preset preset{};
    clay_mesh_brush_desc brush{};
};

AbiStroke abi_stroke(int32_t verb) {
    AbiStroke s;
    s.preset.struct_size = sizeof(s.preset);
    REQUIRE(clay_stroke_preset_defaults(&s.preset) == CLAY_OK);
    s.preset.radius = 0.3f;
    s.brush.struct_size = sizeof(s.brush);
    REQUIRE(clay_mesh_brush_defaults(&s.brush) == CLAY_OK);
    s.brush.verb = verb;
    if (verb == CLAY_MESH_BRUSH_SNAKEHOOK) {
        const float from[3] = {0.0f, 0.0f, 1.0f}, to[3] = {0.0f, 0.0f, 1.8f};
        s.samples = samples_along(from, to, 32);
        s.preset.spacing = 0.1f;
        s.brush.strength = 1.0f;
    } else {
        const float from[3] = {-0.4f, 0.0f, 1.0f}, to[3] = {0.4f, 0.1f, 1.0f};
        s.samples = samples_along(from, to, 24);
        s.preset.spacing = 0.25f;
        s.brush.strength = 0.4f;
    }
    return s;
}

clay_result run_abi(clay_dynamic_sculptor* sculptor, const AbiStroke& s,
                    const clay_dynamic_topology_desc* topo, clay_dynamic_delta* record,
                    size_t* applied, clay_dynamic_stamp_report* report) {
    return clay_dynamic_sculptor_apply_stroke_recorded(sculptor, s.samples.data(), s.samples.size(),
                                                       &s.preset, &s.brush, topo, nullptr, 0,
                                                       record, applied, report);
}

bool same_stats(const clay_dynamic_delta_stats& a, const clay_dynamic_delta_stats& b) {
    return a.vertices == b.vertices && a.halfedges == b.halfedges && a.edges == b.edges &&
           a.faces == b.faces && a.encoded_bytes == b.encoded_bytes;
}

}  // namespace

TEST_CASE("c dynamic delta: a recorded ABI stroke is the stroke, and undoes as one step") {
    const clay_dynamic_topology_desc topo = stroke_topology();
    for (int32_t verb : {int32_t{CLAY_MESH_BRUSH_DRAW}, int32_t{CLAY_MESH_BRUSH_SNAKEHOOK}}) {
        CAPTURE(verb);
        const AbiStroke s = abi_stroke(verb);
        Fixture recorded, plain;
        const Export before = export_of(recorded.surface, recorded.sculptor);

        Record record;
        size_t applied = 0, plain_applied = 0;
        clay_dynamic_stamp_report report{}, plain_report{};
        report.struct_size = plain_report.struct_size = sizeof(report);
        REQUIRE(run_abi(recorded.sculptor, s, &topo, record.delta, &applied, &report) == CLAY_OK);
        REQUIRE(clay_dynamic_sculptor_apply_stroke(plain.sculptor, s.samples.data(),
                                                   s.samples.size(), &s.preset, &s.brush, &topo,
                                                   nullptr, 0, &plain_applied, &plain_report) ==
                CLAY_OK);
        const Export after = export_of(recorded.surface, recorded.sculptor);
        REQUIRE(report.split_edges > 0);
        CHECK(applied == plain_applied);
        CHECK(report.moved_vertices == plain_report.moved_vertices);
        CHECK(report.split_edges == plain_report.split_edges);
        CHECK(report.collapsed_edges == plain_report.collapsed_edges);
        CHECK(same_export(after, export_of(plain.surface, plain.sculptor)));
        CHECK(stats_of(record.delta).encoded_bytes > 0);

        REQUIRE(clay_dynamic_delta_revert(record.delta, recorded.sculptor) == CLAY_OK);
        CHECK(same_export(export_of(recorded.surface, recorded.sculptor), before));
        CHECK(valid(recorded.surface));
        REQUIRE(clay_dynamic_delta_apply(record.delta, recorded.sculptor) == CLAY_OK);
        CHECK(same_export(export_of(recorded.surface, recorded.sculptor), after));
        CHECK(valid(recorded.surface));
    }
}

TEST_CASE("c dynamic delta: a recorded preset stroke undoes exactly") {
    clay_brush_preset preset{};
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_brush_preset_by_name("Standard", &preset) == CLAY_OK);
    const clay_dynamic_topology_desc topo = stroke_topology();
    const float from[3] = {-0.4f, 0.0f, 1.0f}, to[3] = {0.4f, 0.1f, 1.0f};
    const std::vector<clay_stroke_sample_full> samples = samples_along(from, to, 24);

    Fixture recorded, plain;
    const Export before = export_of(recorded.surface, recorded.sculptor);
    Record record;
    size_t applied = 0, plain_applied = 0;
    REQUIRE(clay_dynamic_sculptor_apply_preset_recorded(recorded.sculptor, samples.data(),
                                                        samples.size(), &preset, nullptr, 0, 0,
                                                        &topo, nullptr, 0, record.delta, &applied,
                                                        nullptr) == CLAY_OK);
    REQUIRE(clay_dynamic_sculptor_apply_preset(plain.sculptor, samples.data(), samples.size(),
                                               &preset, nullptr, 0, 0, &topo, nullptr, 0,
                                               &plain_applied, nullptr) == CLAY_OK);
    REQUIRE(applied > 0);
    CHECK(applied == plain_applied);
    const Export after = export_of(recorded.surface, recorded.sculptor);
    CHECK(same_export(after, export_of(plain.surface, plain.sculptor)));
    REQUIRE_FALSE(same_export(after, before));

    REQUIRE(clay_dynamic_delta_revert(record.delta, recorded.sculptor) == CLAY_OK);
    CHECK(same_export(export_of(recorded.surface, recorded.sculptor), before));
    CHECK(valid(recorded.surface));
    REQUIRE(clay_dynamic_delta_apply(record.delta, recorded.sculptor) == CLAY_OK);
    CHECK(same_export(export_of(recorded.surface, recorded.sculptor), after));
    CHECK(valid(recorded.surface));
}

TEST_CASE("c dynamic delta: a recorded stroke onto a moved surface is a mismatch, applied nothing") {
    const clay_dynamic_topology_desc topo = stroke_topology();
    const AbiStroke s = abi_stroke(CLAY_MESH_BRUSH_DRAW);
    Fixture fx;
    Record record;
    REQUIRE(run_abi(fx.sculptor, s, &topo, record.delta, nullptr, nullptr) == CLAY_OK);
    // An unrecorded stamp in between.
    const clay_mesh_brush_desc dab = stamp_brush(kSouth, 0);
    REQUIRE(clay_dynamic_sculptor_stamp(fx.sculptor, &dab, &topo, nullptr, nullptr) == CLAY_OK);

    const clay_dynamic_delta_stats held = stats_of(record.delta);
    const Export was = export_of(fx.surface, fx.sculptor);
    const clay_surface_revision revision = revision_of(fx.surface);
    size_t applied = 99;
    clay_dynamic_stamp_report report{};
    report.struct_size = sizeof(report);
    report.moved_vertices = 12345;
    CHECK(run_abi(fx.sculptor, s, &topo, record.delta, &applied, &report) ==
          CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(applied == 0);
    CHECK(report.moved_vertices == 12345);  // not written
    CHECK(same_stats(stats_of(record.delta), held));
    CHECK(same_export(export_of(fx.surface, fx.sculptor), was));
    CHECK(same_revision(revision_of(fx.surface), revision));

    // The preset sibling checks the same mark.
    clay_brush_preset preset{};
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_brush_preset_by_name("Standard", &preset) == CLAY_OK);
    CHECK(clay_dynamic_sculptor_apply_preset_recorded(fx.sculptor, s.samples.data(),
                                                      s.samples.size(), &preset, nullptr, 0, 0,
                                                      &topo, nullptr, 0, record.delta, &applied,
                                                      nullptr) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(applied == 0);
    CHECK(same_stats(stats_of(record.delta), held));
    CHECK(same_revision(revision_of(fx.surface), revision));
}

TEST_CASE("c dynamic delta: a malformed recorded stroke is INVALID_ARGUMENT even with a stale record") {
    const clay_dynamic_topology_desc topo = stroke_topology();
    Fixture fx;
    Record record;
    REQUIRE(run_abi(fx.sculptor, abi_stroke(CLAY_MESH_BRUSH_DRAW), &topo, record.delta, nullptr,
                    nullptr) == CLAY_OK);
    const clay_mesh_brush_desc dab = stamp_brush(kSouth, 0);
    REQUIRE(clay_dynamic_sculptor_stamp(fx.sculptor, &dab, &topo, nullptr, nullptr) == CLAY_OK);
    // The record is stale now: a well-formed call would be a mismatch.
    const clay_dynamic_delta_stats held = stats_of(record.delta);
    const clay_surface_revision revision = revision_of(fx.surface);

    SUBCASE("Layer") {
        AbiStroke layer = abi_stroke(CLAY_MESH_BRUSH_DRAW);
        layer.brush.verb = CLAY_MESH_BRUSH_LAYER;
        size_t applied = 99;
        CHECK(run_abi(fx.sculptor, layer, &topo, record.delta, &applied, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(applied == 0);
    }
    SUBCASE("a short report") {
        clay_dynamic_stamp_report shortened{};
        shortened.struct_size = 4;
        size_t applied = 99;
        CHECK(run_abi(fx.sculptor, abi_stroke(CLAY_MESH_BRUSH_DRAW), &topo, record.delta,
                      &applied, &shortened) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(applied == 0);
    }
    CHECK(same_stats(stats_of(record.delta), held));
    CHECK(same_revision(revision_of(fx.surface), revision));
}

TEST_CASE("c dynamic delta: a null record strokes exactly like the shipped stroke") {
    const clay_dynamic_topology_desc topo = stroke_topology();
    const AbiStroke s = abi_stroke(CLAY_MESH_BRUSH_DRAW);
    Fixture a, b;
    size_t applied_a = 0, applied_b = 0;
    REQUIRE(clay_dynamic_sculptor_apply_stroke(a.sculptor, s.samples.data(), s.samples.size(),
                                               &s.preset, &s.brush, &topo, nullptr, 0, &applied_a,
                                               nullptr) == CLAY_OK);
    REQUIRE(run_abi(b.sculptor, s, &topo, nullptr, &applied_b, nullptr) == CLAY_OK);
    CHECK(applied_a > 0);
    CHECK(applied_a == applied_b);
    CHECK(same_export(export_of(a.surface, a.sculptor), export_of(b.surface, b.sculptor)));
}
