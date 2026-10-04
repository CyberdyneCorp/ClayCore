// Undo for a multiresolution gesture across the C ABI (c-abi spec,
// undo-a-multires-gesture-across-the-abi, issue #671).
//
// What is gated here, and none of it as a clock:
//
//   - a revert gives back the hierarchy as the gesture found it -- the base
//     detail, every sculpt pass and every evaluated level, bit for bit -- and an
//     apply gives back the hierarchy as the gesture left it. Through the plain
//     sculptor and through a sculpt-layer pass, on a uniform hierarchy and on a
//     regionally refined one, at level 0 and above it, with and without a
//     mirrored second stamp;
//   - a record paired with another hierarchy, or with this one after its levels
//     changed, is CLAY_ERROR_SNAPSHOT_MISMATCH and writes nothing;
//   - the record's size follows the vertices reached, not the stamps taken;
//   - the bytes round-trip, and encoded_bytes is the documented formula;
//   - a replay marks what moved, so a host re-copying its dirty blocks sees it.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

void plane(int n, float half, std::vector<float>* positions, std::vector<uint32_t>* indices) {
    positions->clear();
    indices->clear();
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            positions->push_back(-half + step * static_cast<float>(x));
            positions->push_back(0.0f);
            positions->push_back(-half + step * static_cast<float>(z));
        }
    const uint32_t stride = static_cast<uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = static_cast<uint32_t>(z) * stride + static_cast<uint32_t>(x);
            const uint32_t b = a + 1, c = a + stride + 1, d = a + stride;
            indices->insert(indices->end(), {a, b, c, a, c, d});
        }
}

// One hierarchy and the two ways a host writes it.
struct Fixture {
    clay_mesh* mesh = nullptr;
    clay_multires* surface = nullptr;
    clay_multires_sculptor* sculptor = nullptr;

    // `levels` uniform levels on an n-by-n cage over [-1, 1]^2.
    explicit Fixture(int n = 6, uint32_t levels = 2) {
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        plane(n, 1.0f, &positions, &indices);
        REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                         indices.size(), &mesh) == CLAY_OK);
        int32_t err = -1;
        REQUIRE(clay_multires_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
        for (uint32_t i = 0; i < levels; ++i)
            REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
        REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);
    }
    ~Fixture() {
        clay_multires_sculptor_destroy(sculptor);
        clay_multires_destroy(surface);
        clay_mesh_destroy(mesh);
    }

    uint64_t add_layer() {
        uint64_t id = 0;
        int32_t err = -1;
        REQUIRE(clay_multires_add_sculpt_layer(surface, "pass", &id, &err) == CLAY_OK);
        return id;
    }
};

// EVERYTHING A HOST CAN SEE of the hierarchy, as bytes: the base detail, every
// sculpt pass, and every level's evaluated positions -- the cage's included,
// which the detail checksum does not hash.
struct Snapshot {
    uint64_t detail = 0;
    uint64_t layers = 0;
    std::vector<std::vector<float>> positions;

    bool operator==(const Snapshot& o) const {
        return detail == o.detail && layers == o.layers && positions == o.positions;
    }
};

Snapshot snapshot(clay_multires* surface) {
    Snapshot s;
    REQUIRE(clay_multires_detail_checksum(surface, &s.detail) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_checksum(surface, &s.layers) == CLAY_OK);
    const uint32_t count = clay_multires_level_count(surface);
    for (uint32_t level = 0; level < count; ++level) {
        clay_mesh* m = nullptr;
        REQUIRE(clay_multires_copy_level_mesh(surface, level, &m) == CLAY_OK);
        const float* raw = clay_mesh_positions(m);
        s.positions.emplace_back(raw, raw + clay_mesh_vertex_count(m) * 3);
        clay_mesh_destroy(m);
    }
    return s;
}

clay_mesh_brush_desc draw_at(float x, float z, float radius, float strength) {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.center[0] = x;
    d.center[1] = 0.0f;
    d.center[2] = z;
    d.direction[0] = 0.0f;
    d.direction[1] = 1.0f;
    d.direction[2] = 0.0f;
    d.radius = radius;
    d.strength = strength;
    return d;
}

clay_multires_delta_stats stats_of(const clay_multires_delta* d) {
    clay_multires_delta_stats s{};
    s.struct_size = sizeof(s);
    REQUIRE(clay_multires_delta_stats_get(d, &s) == CLAY_OK);
    return s;
}

std::vector<uint32_t> levels_of(const clay_multires_delta* d) {
    size_t count = 0;
    REQUIRE(clay_multires_delta_levels(d, nullptr, &count) == CLAY_OK);
    std::vector<uint32_t> out(count);
    if (count > 0) REQUIRE(clay_multires_delta_levels(d, out.data(), &count) == CLAY_OK);
    return out;
}

// The documented byte formula, from the four counts alone.
uint64_t formula(const clay_multires_delta_stats& s) {
    uint64_t bytes = 40;
    if (s.detail_entries + s.cage_entries > 0)
        bytes += 16 + 32 * s.detail_entries + 28 * s.cage_entries;
    if (s.layer_detail_entries + s.layer_mask_entries > 0)
        bytes += 24 + 32 * s.layer_detail_entries + 16 * s.layer_mask_entries;
    return bytes;
}

// Undo, then redo, then undo again: each lands exactly where it should, and a
// second revert is a no-op rather than an error.
void check_round_trip(clay_multires* surface, const clay_multires_delta* record,
                      const Snapshot& before, const Snapshot& after) {
    REQUIRE(clay_multires_delta_revert(record, surface) == CLAY_OK);
    CHECK(snapshot(surface) == before);
    REQUIRE(clay_multires_delta_revert(record, surface) == CLAY_OK);
    CHECK(snapshot(surface) == before);
    REQUIRE(clay_multires_delta_apply(record, surface) == CLAY_OK);
    CHECK(snapshot(surface) == after);
    REQUIRE(clay_multires_delta_revert(record, surface) == CLAY_OK);
    CHECK(snapshot(surface) == before);
}

const float kStroke[] = {-0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.1f,
                         1.0f,  0.0f, 0.5f, 0.0f, 0.2f, 1.0f, 0.0f};

clay_stroke_preset stroke_preset() {
    clay_stroke_preset p{};
    p.struct_size = sizeof(p);
    REQUIRE(clay_stroke_preset_defaults(&p) == CLAY_OK);
    p.spacing = 0.2f;
    p.radius = 0.4f;
    return p;
}

}  // namespace

TEST_CASE("c multires delta: a recorded stroke undoes and redoes bit for bit") {
    Fixture f(6, 2);
    const Snapshot before = snapshot(f.surface);
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);

    clay_multires_delta* record = clay_multires_delta_create();
    REQUIRE(record != nullptr);
    size_t applied = 0;
    REQUIRE(clay_multires_sculptor_apply_stroke_recorded(f.sculptor, kStroke, 3, &preset, &brush,
                                                         nullptr, nullptr, 0, record, &applied,
                                                         nullptr) == CLAY_OK);
    CHECK(applied > 3);
    const Snapshot after = snapshot(f.surface);
    REQUIRE_FALSE(after == before);

    const clay_multires_delta_stats s = stats_of(record);
    CHECK(s.detail_entries > 0);
    CHECK(s.cage_entries == 0);
    CHECK(s.layer_detail_entries == 0);
    CHECK(s.sculpt_layer == CLAY_NO_SCULPT_LAYER);
    CHECK(levels_of(record) == std::vector<uint32_t>{2});

    check_round_trip(f.surface, record, before, after);

    // THE SCULPTOR SURVIVES THE UNDO. The same stroke again, through the same
    // sculptor, lands exactly where the first one did -- a stale level mesh or
    // chunk bound would put it somewhere else.
    REQUIRE(clay_multires_sculptor_apply_stroke(f.sculptor, kStroke, 3, &preset, &brush, nullptr,
                                                nullptr, 0, nullptr, nullptr) == CLAY_OK);
    CHECK(snapshot(f.surface) == after);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a recorded stroke is the unrecorded stroke") {
    Fixture recorded(6, 2), plain(6, 2);
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    clay_multires_delta* record = clay_multires_delta_create();
    size_t a = 0, b = 0;
    clay_multires_stamp_report ra{}, rb{};
    ra.struct_size = sizeof(ra);
    rb.struct_size = sizeof(rb);
    REQUIRE(clay_multires_sculptor_apply_stroke_recorded(recorded.sculptor, kStroke, 3, &preset,
                                                         &brush, nullptr, nullptr, 0, record, &a,
                                                         &ra) == CLAY_OK);
    REQUIRE(clay_multires_sculptor_apply_stroke(plain.sculptor, kStroke, 3, &preset, &brush,
                                                nullptr, nullptr, 0, &b, &rb) == CLAY_OK);
    CHECK(a == b);
    CHECK(ra.moved_vertices == rb.moved_vertices);
    CHECK(snapshot(recorded.surface) == snapshot(plain.surface));

    // A NULL record is the unrecorded call, exactly.
    Fixture null_record(6, 2);
    REQUIRE(clay_multires_sculptor_apply_stroke_recorded(null_record.sculptor, kStroke, 3,
                                                         &preset, &brush, nullptr, nullptr, 0,
                                                         nullptr, nullptr, nullptr) == CLAY_OK);
    CHECK(snapshot(null_record.surface) == snapshot(plain.surface));
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a mirrored stroke is one step whose coverage is both sides") {
    Fixture f(6, 2), single(6, 2);
    const Snapshot before = snapshot(f.surface);
    clay_multires_delta* one_side = clay_multires_delta_create();
    clay_multires_delta* both = clay_multires_delta_create();

    // A host's symmetry: every stamp and its reflection through x = 0, into
    // one record. The cage is symmetric about x = 0 and the two discs do not
    // meet, so the mirrored side reaches exactly as many entries as the first.
    const clay_mesh_brush_desc right = draw_at(0.5f, 0.0f, 0.3f, 0.3f);
    const clay_mesh_brush_desc left = draw_at(-0.5f, 0.0f, 0.3f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(single.sculptor, &right, nullptr, one_side,
                                                  nullptr) == CLAY_OK);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &right, nullptr, both, nullptr) ==
            CLAY_OK);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &left, nullptr, both, nullptr) ==
            CLAY_OK);
    const Snapshot after = snapshot(f.surface);

    CHECK(stats_of(one_side).detail_entries > 0);
    CHECK(stats_of(both).detail_entries == 2 * stats_of(one_side).detail_entries);
    check_round_trip(f.surface, both, before, after);
    clay_multires_delta_destroy(one_side);
    clay_multires_delta_destroy(both);
}

TEST_CASE("c multires delta: a level-0 stroke records the cage and puts it back") {
    Fixture f(6, 2);
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 0) == CLAY_OK);
    const Snapshot before = snapshot(f.surface);
    clay_multires_delta* record = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.6f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);
    const Snapshot after = snapshot(f.surface);
    REQUIRE_FALSE(after == before);

    const clay_multires_delta_stats s = stats_of(record);
    CHECK(s.cage_entries > 0);
    CHECK(s.detail_entries == 0);
    CHECK(levels_of(record) == std::vector<uint32_t>{0});
    check_round_trip(f.surface, record, before, after);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a stroke across a refined region's rim undoes on every level") {
    // Ten cells across with the middle two-by-two refined to level 3; the
    // stroke sits on the rim, so it writes the refined level AND the coarser
    // patches beside it.
    Fixture f(10, 0);
    const auto cell = [](uint32_t x, uint32_t z) { return 2u * (z * 10u + x); };
    const uint32_t block[8] = {cell(4, 4), cell(4, 4) + 1, cell(5, 4), cell(5, 4) + 1,
                               cell(4, 5), cell(4, 5) + 1, cell(5, 5), cell(5, 5) + 1};
    int32_t err = -1;
    REQUIRE(clay_multires_refine_patches_to_level(f.surface, block, 8, 3, nullptr, &err) ==
            CLAY_OK);
    int32_t uniform = -1;
    REQUIRE(clay_multires_uniform_depth(f.surface, &uniform) == CLAY_OK);
    REQUIRE(uniform == 0);
    REQUIRE(clay_multires_set_sculpt_level(f.surface, 3) == CLAY_OK);

    const Snapshot before = snapshot(f.surface);
    clay_multires_delta* record = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.2f, 0.0f, 0.5f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);
    const Snapshot after = snapshot(f.surface);
    REQUIRE_FALSE(after == before);
    CHECK(levels_of(record).size() > 1);
    check_round_trip(f.surface, record, before, after);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a sculpt-layer pass commits into a record and undoes") {
    Fixture f(6, 2);
    const uint64_t layer = f.add_layer();
    const Snapshot before = snapshot(f.surface);

    clay_multires_sculpt_layer_stroke* stroke = nullptr;
    REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, nullptr) == CLAY_OK);
    const clay_mesh_brush_desc right = draw_at(0.4f, 0.0f, 0.3f, 0.3f);
    const clay_mesh_brush_desc left = draw_at(-0.4f, 0.0f, 0.3f, 0.3f);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(clay_multires_sculpt_layer_stroke_stamp(stroke, &right, nullptr, nullptr) ==
                CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_stroke_stamp(stroke, &left, nullptr, nullptr) ==
                CLAY_OK);
    }
    size_t open_entries = 0;
    REQUIRE(clay_multires_sculpt_layer_stroke_record_size(stroke, &open_entries) == CLAY_OK);

    clay_multires_delta* record = clay_multires_delta_create();
    size_t entries = 0;
    REQUIRE(clay_multires_sculpt_layer_stroke_commit_into(stroke, record, &entries) == CLAY_OK);
    CHECK(entries == open_entries);
    const Snapshot after = snapshot(f.surface);
    CHECK(after.detail == before.detail);  // a pass, not the form
    CHECK(after.layers != before.layers);

    const clay_multires_delta_stats s = stats_of(record);
    CHECK(s.sculpt_layer == layer);
    CHECK(s.layer_detail_entries + s.layer_mask_entries == entries);
    CHECK(s.detail_entries + s.cage_entries == 0);
    check_round_trip(f.surface, record, before, after);

    // The transaction is closed: committing again is refused, and the record
    // stays as it was.
    CHECK(clay_multires_sculpt_layer_stroke_commit_into(stroke, record, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(stats_of(record).layer_detail_entries == s.layer_detail_entries);
    clay_multires_sculpt_layer_stroke_destroy(stroke);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: the plain sculptor records the active pass, not nothing") {
    // The plain sculptor writes the ACTIVE layer when there is one. A record
    // that only held the base half would come back empty and undo nothing.
    Fixture f(6, 2);
    const uint64_t layer = f.add_layer();
    const Snapshot before = snapshot(f.surface);
    clay_multires_delta* record = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);
    const Snapshot after = snapshot(f.surface);
    const clay_multires_delta_stats s = stats_of(record);
    CHECK(s.sculpt_layer == layer);
    CHECK(s.layer_detail_entries > 0);
    check_round_trip(f.surface, record, before, after);

    // Continuing the record into a DIFFERENT pass would make one step of two
    // channels' halves under one layer id: refused, and nothing stamped.
    f.add_layer();
    const Snapshot moved = snapshot(f.surface);
    CHECK(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
          CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(snapshot(f.surface) == moved);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a recorded stroke records the active pass too") {
    // The stroke path, not just the stamp: a stroke is fed through the
    // multires stroke gesture, which has to hand the pass half to every stamp.
    // Dropped there, the record comes back with no pass entries and a revert
    // leaves the stroke on the surface.
    Fixture f(6, 2);
    const uint64_t layer = f.add_layer();
    const Snapshot before = snapshot(f.surface);
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    clay_multires_delta* record = clay_multires_delta_create();
    size_t applied = 0;
    REQUIRE(clay_multires_sculptor_apply_stroke_recorded(f.sculptor, kStroke, 3, &preset, &brush,
                                                         nullptr, nullptr, 0, record, &applied,
                                                         nullptr) == CLAY_OK);
    CHECK(applied > 3);
    const Snapshot after = snapshot(f.surface);
    REQUIRE_FALSE(after == before);
    const clay_multires_delta_stats s = stats_of(record);
    CHECK(s.sculpt_layer == layer);
    CHECK(s.layer_detail_entries > 0);
    check_round_trip(f.surface, record, before, after);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a record refuses another hierarchy and a changed structure") {
    Fixture f(6, 2), twin(6, 2);
    clay_multires_delta* record = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);

    // A TWIN: the same cage, the same levels, every count equal. Only the
    // binding tells them apart, and it must.
    const Snapshot twin_before = snapshot(twin.surface);
    CHECK(clay_multires_delta_revert(record, twin.surface) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(clay_multires_delta_apply(record, twin.surface) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(snapshot(twin.surface) == twin_before);

    // ...and continuing a record onto the twin is refused before a stamp.
    CHECK(clay_multires_sculptor_stamp_recorded(twin.sculptor, &brush, nullptr, record,
                                                nullptr) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(snapshot(twin.surface) == twin_before);

    // THE SAME HIERARCHY AFTER ITS LEVELS CHANGED. Removing the top level and
    // adding it back leaves every count where it was, which is exactly why
    // counts are not the check.
    int32_t err = -1;
    REQUIRE(clay_multires_remove_highest_level(f.surface, &err) == CLAY_OK);
    REQUIRE(clay_multires_add_level(f.surface, nullptr, &err) == CLAY_OK);
    const Snapshot relevelled = snapshot(f.surface);
    CHECK(clay_multires_delta_revert(record, f.surface) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(clay_multires_delta_apply(record, f.surface) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(snapshot(f.surface) == relevelled);

    // Null handles are malformed, not retryable.
    CHECK(clay_multires_delta_revert(nullptr, f.surface) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_multires_delta_revert(record, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a removed pass refuses its record and writes nothing") {
    Fixture f(6, 2);
    const uint64_t layer = f.add_layer();
    clay_multires_delta* record = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);
    int32_t err = -1;
    REQUIRE(clay_multires_remove_sculpt_layer(f.surface, layer, &err) == CLAY_OK);
    const Snapshot removed = snapshot(f.surface);
    CHECK(clay_multires_delta_revert(record, f.surface) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(snapshot(f.surface) == removed);
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: the record follows the vertices reached, not the stamps") {
    Fixture once(6, 2), many(6, 2);
    clay_multires_delta* one = clay_multires_delta_create();
    clay_multires_delta* forty = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.05f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(once.sculptor, &brush, nullptr, one, nullptr) ==
            CLAY_OK);
    for (int i = 0; i < 40; ++i)
        REQUIRE(clay_multires_sculptor_stamp_recorded(many.sculptor, &brush, nullptr, forty,
                                                      nullptr) == CLAY_OK);
    const clay_multires_delta_stats a = stats_of(one), b = stats_of(forty);
    CHECK(a.detail_entries > 0);
    CHECK(b.detail_entries == a.detail_entries);
    CHECK(b.encoded_bytes == a.encoded_bytes);
    clay_multires_delta_destroy(one);
    clay_multires_delta_destroy(forty);
}

TEST_CASE("c multires delta: the byte cost is the formula, and the bytes round-trip") {
    Fixture f(6, 2);
    f.add_layer();
    const Snapshot before = snapshot(f.surface);
    clay_multires_delta* record = clay_multires_delta_create();
    // A layer half from the active pass, then a base half once the pass is
    // gone from the active slot: one record holding both.
    const clay_mesh_brush_desc brush = draw_at(0.3f, 0.0f, 0.3f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);
    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, CLAY_NO_SCULPT_LAYER, nullptr) ==
            CLAY_OK);
    const clay_mesh_brush_desc other = draw_at(-0.3f, 0.0f, 0.3f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &other, nullptr, record, nullptr) ==
            CLAY_OK);
    const Snapshot after = snapshot(f.surface);

    const clay_multires_delta_stats s = stats_of(record);
    CHECK(s.detail_entries > 0);
    CHECK(s.layer_detail_entries > 0);
    CHECK(s.encoded_bytes == formula(s));
    CHECK(s.resident_bytes >= 32 * (s.detail_entries + s.layer_detail_entries));

    size_t need = 0;
    REQUIRE(clay_multires_delta_serialize(record, nullptr, &need) == CLAY_OK);
    CHECK(need == s.encoded_bytes);
    std::vector<uint8_t> bytes(need);
    size_t short_count = need - 1;
    CHECK(clay_multires_delta_serialize(record, bytes.data(), &short_count) ==
          CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(short_count == need);
    size_t count = need;
    REQUIRE(clay_multires_delta_serialize(record, bytes.data(), &count) == CLAY_OK);
    CHECK(count == need);

    // SPILLED AND RELOADED, it replays exactly as the original.
    clay_multires_delta* back = nullptr;
    REQUIRE(clay_multires_delta_deserialize(bytes.data(), bytes.size(), &back) == CLAY_OK);
    const clay_multires_delta_stats t = stats_of(back);
    CHECK(t.detail_entries == s.detail_entries);
    CHECK(t.layer_detail_entries == s.layer_detail_entries);
    CHECK(t.sculpt_layer == s.sculpt_layer);
    CHECK(levels_of(back) == levels_of(record));
    clay_multires_delta_destroy(record);
    check_round_trip(f.surface, back, before, after);

    // Truncated, trailing or from a newer version: refused, nothing handed out.
    clay_multires_delta* bad = reinterpret_cast<clay_multires_delta*>(&count);
    CHECK(clay_multires_delta_deserialize(bytes.data(), bytes.size() - 1, &bad) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(bad == nullptr);
    std::vector<uint8_t> trailing = bytes;
    trailing.push_back(0);
    CHECK(clay_multires_delta_deserialize(trailing.data(), trailing.size(), &bad) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    std::vector<uint8_t> newer = bytes;
    newer[4] = 0xff;  // the version word follows the four-byte magic
    CHECK(clay_multires_delta_deserialize(newer.data(), newer.size(), &bad) ==
          CLAY_ERROR_FORWARD_VERSION);
    CHECK(bad == nullptr);
    CHECK(clay_multires_delta_deserialize(nullptr, 8, &bad) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_multires_delta_destroy(back);
}

TEST_CASE("c multires delta: a replay marks what moved for the block transport") {
    Fixture f(6, 2);
    const uint64_t layer = f.add_layer();
    clay_multires_delta* base = clay_multires_delta_create();
    clay_multires_delta* pass = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, pass, nullptr) ==
            CLAY_OK);
    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, CLAY_NO_SCULPT_LAYER, nullptr) ==
            CLAY_OK);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, base, nullptr) ==
            CLAY_OK);
    CHECK(stats_of(pass).sculpt_layer == layer);

    uint64_t evaluated_before = 0, evaluated_after = 0;
    for (clay_multires_delta* record : {base, pass}) {
        REQUIRE(clay_multires_clear_dirty(f.surface) == CLAY_OK);
        REQUIRE(clay_multires_revision(f.surface, nullptr, nullptr, &evaluated_before) == CLAY_OK);
        REQUIRE(clay_multires_delta_revert(record, f.surface) == CLAY_OK);
        REQUIRE(clay_multires_revision(f.surface, nullptr, nullptr, &evaluated_after) == CLAY_OK);
        CHECK(evaluated_after != evaluated_before);
        CHECK(clay_multires_dirty_block_count(f.surface) > 0);
    }
    clay_multires_delta_destroy(base);
    clay_multires_delta_destroy(pass);
}

TEST_CASE("c multires delta: an empty record, a cleared record and the size queries") {
    Fixture f(6, 2);
    clay_multires_delta* record = clay_multires_delta_create();
    const Snapshot before = snapshot(f.surface);
    // An empty record is bound to nothing and replays as a no-op anywhere.
    CHECK(clay_multires_delta_revert(record, f.surface) == CLAY_OK);
    CHECK(clay_multires_delta_apply(record, f.surface) == CLAY_OK);
    CHECK(snapshot(f.surface) == before);
    CHECK(levels_of(record).empty());
    CHECK(stats_of(record).encoded_bytes == 40);

    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, nullptr) ==
            CLAY_OK);
    uint32_t one = 0;
    size_t zero = 0;
    CHECK(clay_multires_delta_levels(record, &one, &zero) == CLAY_ERROR_BUFFER_TOO_SMALL);
    CHECK(zero == 1);

    // A host reusing one record per stroke clears it, and the next capture
    // binds afresh -- here to a different hierarchy, which is now fine.
    REQUIRE(clay_multires_delta_clear(record) == CLAY_OK);
    CHECK(stats_of(record).detail_entries == 0);
    Fixture other(6, 2);
    CHECK(clay_multires_sculptor_stamp_recorded(other.sculptor, &brush, nullptr, record,
                                                nullptr) == CLAY_OK);

    // A malformed descriptor is INVALID_ARGUMENT before the record is looked
    // at, and nothing is stamped.
    const Snapshot held = snapshot(f.surface);
    clay_multires_stamp_report report{};
    report.struct_size = 3;
    CHECK(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, record, &report) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_mesh_brush_desc broken = brush;
    broken.struct_size = 3;
    CHECK(clay_multires_sculptor_stamp_recorded(f.sculptor, &broken, nullptr, record, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(snapshot(f.surface) == held);

    CHECK(clay_multires_delta_clear(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_multires_delta_stats st{};
    st.struct_size = sizeof(st);
    CHECK(clay_multires_delta_stats_get(nullptr, &st) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_multires_delta_destroy(record);
    clay_multires_delta_destroy(nullptr);
}

TEST_CASE("c multires delta: commit_into refuses a record that already holds a gesture") {
    Fixture f(6, 2);
    f.add_layer();
    clay_multires_delta* held = clay_multires_delta_create();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    REQUIRE(clay_multires_sculptor_stamp_recorded(f.sculptor, &brush, nullptr, held, nullptr) ==
            CLAY_OK);
    const clay_multires_delta_stats kept = stats_of(held);

    clay_multires_sculpt_layer_stroke* stroke = nullptr;
    REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, nullptr) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_stroke_stamp(stroke, &brush, nullptr, nullptr) == CLAY_OK);
    CHECK(clay_multires_sculpt_layer_stroke_commit_into(stroke, held, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(stats_of(held).layer_detail_entries == kept.layer_detail_entries);
    // ...and the gesture is still open, so the host can still keep it.
    clay_multires_delta* fresh = clay_multires_delta_create();
    CHECK(clay_multires_sculpt_layer_stroke_commit_into(stroke, fresh, nullptr) == CLAY_OK);
    CHECK(stats_of(fresh).layer_detail_entries > 0);
    clay_multires_sculpt_layer_stroke_destroy(stroke);
    clay_multires_delta_destroy(held);
    clay_multires_delta_destroy(fresh);
}

// -- a gesture fed by a stroke session (add-stroke-session, #670) -------------
//
// clay_multires_sculptor_apply_stroke_tx is the whole-path stroke delivered in
// pieces, so its record has to be the whole-path call's record: one gesture,
// both halves, continued across every call, and refused BEFORE the session's
// stamps are taken so that a refusal loses none of them.

namespace {

// A drag across the cage in more samples than one batch, so the gesture is
// applied over several calls.
std::vector<clay_stroke_sample_full> session_path() {
    std::vector<clay_stroke_sample_full> out;
    const int n = 24;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        clay_stroke_sample_full s{};
        s.position[0] = -0.6f + 1.2f * t;
        s.position[2] = 0.15f * t * t;
        s.pressure = 0.6f + 0.4f * t;
        out.push_back(s);
    }
    return out;
}

std::vector<float> packed(const std::vector<clay_stroke_sample_full>& samples) {
    std::vector<float> out;
    for (const clay_stroke_sample_full& s : samples)
        out.insert(out.end(), {s.position[0], s.position[1], s.position[2], s.pressure, s.tilt});
    return out;
}

// Feed `samples` in batches of `batch`, applying after every append and once
// after the end. Returns the stamps applied; `*writing_calls` counts the calls
// that applied any.
size_t feed_session(clay_multires_sculptor* sculptor, clay_stroke_tx* tx,
                    const std::vector<clay_stroke_sample_full>& samples, size_t batch,
                    const clay_mesh_brush_desc& brush, clay_multires_delta* record,
                    size_t* writing_calls) {
    size_t applied = 0;
    *writing_calls = 0;
    auto apply = [&] {
        size_t n = 0;
        clay_multires_stamp_report report{};
        report.struct_size = sizeof(report);
        REQUIRE(clay_multires_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr, 1,
                                                       record, &n, &report) == CLAY_OK);
        applied += n;
        if (n > 0) ++*writing_calls;
    };
    for (size_t done = 0; done < samples.size(); done += batch) {
        const size_t n = std::min(batch, samples.size() - done);
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + done, n, nullptr, nullptr) == CLAY_OK);
        apply();
    }
    REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);
    apply();
    return applied;
}

clay_stroke_tx* session(const clay_stroke_preset& preset) {
    clay_stroke_tx* tx = nullptr;
    REQUIRE(clay_stroke_tx_begin(&preset, &tx) == CLAY_OK);
    return tx;
}

void check_same_record(const clay_multires_delta* a, const clay_multires_delta* b) {
    const clay_multires_delta_stats sa = stats_of(a), sb = stats_of(b);
    CHECK(sa.detail_entries == sb.detail_entries);
    CHECK(sa.cage_entries == sb.cage_entries);
    CHECK(sa.layer_detail_entries == sb.layer_detail_entries);
    CHECK(sa.layer_mask_entries == sb.layer_mask_entries);
    CHECK(sa.encoded_bytes == sb.encoded_bytes);
    CHECK(levels_of(a) == levels_of(b));
}

}  // namespace

TEST_CASE(
    "c multires delta: a session-fed stroke records the whole-path "
    "stroke's record") {
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    const std::vector<clay_stroke_sample_full> samples = session_path();
    const std::vector<float> flat = packed(samples);
    for (bool with_pass : {true, false}) {
        CAPTURE(with_pass);
        Fixture whole(6, 2), pieces(6, 2);
        const uint64_t layer = with_pass ? whole.add_layer() : CLAY_NO_SCULPT_LAYER;
        if (with_pass) REQUIRE(pieces.add_layer() == layer);
        const Snapshot before = snapshot(pieces.surface);

        clay_multires_delta* expected = clay_multires_delta_create();
        size_t whole_applied = 0;
        REQUIRE(clay_multires_sculptor_apply_stroke_recorded(
                    whole.sculptor, flat.data(), samples.size(), &preset, &brush, nullptr, nullptr,
                    1, expected, &whole_applied, nullptr) == CLAY_OK);
        REQUIRE(whole_applied > 4);

        clay_multires_delta* record = clay_multires_delta_create();
        clay_stroke_tx* tx = session(preset);
        size_t writing_calls = 0;
        const size_t applied =
            feed_session(pieces.sculptor, tx, samples, 4, brush, record, &writing_calls);
        clay_stroke_tx_destroy(tx);
        CHECK(applied == whole_applied);
        // The record is CONTINUED: several calls wrote into it, and it is one
        // gesture rather than the last call's.
        CHECK(writing_calls > 2);
        const Snapshot after = snapshot(pieces.surface);
        CHECK(after == snapshot(whole.surface));

        const clay_multires_delta_stats s = stats_of(record);
        CHECK(s.sculpt_layer == layer);
        if (with_pass)
            CHECK(s.layer_detail_entries > 0);
        else
            CHECK(s.detail_entries > 0);
        check_same_record(record, expected);

        // ONE revert takes the whole gesture back, on every level, bit for bit.
        check_round_trip(pieces.surface, record, before, after);
        clay_multires_delta_destroy(record);
        clay_multires_delta_destroy(expected);
    }
}

TEST_CASE(
    "c multires delta: a session with a NULL record records nothing and "
    "stamps the same") {
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    const std::vector<clay_stroke_sample_full> samples = session_path();
    const std::vector<float> flat = packed(samples);
    Fixture plain(6, 2), unrecorded(6, 2), recorded(6, 2);
    plain.add_layer();
    unrecorded.add_layer();
    recorded.add_layer();
    REQUIRE(clay_multires_sculptor_apply_stroke(plain.sculptor, flat.data(), samples.size(),
                                                &preset, &brush, nullptr, nullptr, 1, nullptr,
                                                nullptr) == CLAY_OK);
    size_t calls = 0;
    clay_stroke_tx* tx = session(preset);
    feed_session(unrecorded.sculptor, tx, samples, 4, brush, nullptr, &calls);
    clay_stroke_tx_destroy(tx);
    clay_multires_delta* record = clay_multires_delta_create();
    tx = session(preset);
    feed_session(recorded.sculptor, tx, samples, 4, brush, record, &calls);
    clay_stroke_tx_destroy(tx);
    CHECK(snapshot(unrecorded.surface) == snapshot(plain.surface));
    CHECK(snapshot(recorded.surface) == snapshot(plain.surface));
    clay_multires_delta_destroy(record);
}

TEST_CASE("c multires delta: a session's record is part of its binding") {
    // Recorded or not, and into which record, is fixed by the first call, as
    // it is for the adaptive consumer: half a gesture in one record and half
    // in another would be two undo steps that each restore a torn stroke.
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    const std::vector<clay_stroke_sample_full> samples = session_path();
    Fixture f(6, 2);
    f.add_layer();
    clay_multires_delta* record = clay_multires_delta_create();
    clay_multires_delta* other = clay_multires_delta_create();
    clay_stroke_tx* tx = session(preset);
    REQUIRE(clay_stroke_tx_append(tx, samples.data(), 12, nullptr, nullptr) == CLAY_OK);
    size_t n = 0;
    REQUIRE(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                   record, &n, nullptr) == CLAY_OK);
    REQUIRE(n > 0);
    REQUIRE(clay_stroke_tx_append(tx, samples.data() + 12, 12, nullptr, nullptr) == CLAY_OK);
    const Snapshot held = snapshot(f.surface);
    for (clay_multires_delta* wrong : {other, static_cast<clay_multires_delta*>(nullptr)}) {
        n = 7;
        CHECK(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                     wrong, &n,
                                                     nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(n == 0);
    }
    CHECK(snapshot(f.surface) == held);
    CHECK(stats_of(other).encoded_bytes == 40);
    REQUIRE(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                   record, &n, nullptr) == CLAY_OK);
    CHECK(n > 0);
    clay_stroke_tx_destroy(tx);
    clay_multires_delta_destroy(other);
    clay_multires_delta_destroy(record);
}

TEST_CASE(
    "c multires delta: a refused session call loses none of the "
    "session's stamps") {
    // The record is checked BEFORE the stamps are taken. Mid-gesture the host
    // makes another pass active: continuing the record would put that pass's
    // coefficients under the first pass's id, so the call is refused with
    // CLAY_ERROR_SNAPSHOT_MISMATCH and stamps nothing. Once the first pass is
    // active again the held stamps apply, and the gesture ends exactly where
    // the whole-path stroke does, as one record. A malformed report is refused
    // before the stamps too.
    const clay_stroke_preset preset = stroke_preset();
    const clay_mesh_brush_desc brush = draw_at(0.0f, 0.0f, 0.4f, 0.3f);
    const std::vector<clay_stroke_sample_full> samples = session_path();
    const std::vector<float> flat = packed(samples);

    Fixture whole(6, 2), f(6, 2);
    int32_t err = -1;
    const uint64_t first = f.add_layer();
    const uint64_t second = f.add_layer();
    REQUIRE(whole.add_layer() == first);
    REQUIRE(whole.add_layer() == second);
    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, first, &err) == CLAY_OK);
    REQUIRE(clay_multires_set_active_sculpt_layer(whole.surface, first, &err) == CLAY_OK);
    const Snapshot before = snapshot(f.surface);
    clay_multires_delta* expected = clay_multires_delta_create();
    REQUIRE(clay_multires_sculptor_apply_stroke_recorded(
                whole.sculptor, flat.data(), samples.size(), &preset, &brush, nullptr, nullptr, 1,
                expected, nullptr, nullptr) == CLAY_OK);

    clay_multires_delta* record = clay_multires_delta_create();
    clay_stroke_tx* tx = session(preset);
    REQUIRE(clay_stroke_tx_append(tx, samples.data(), 12, nullptr, nullptr) == CLAY_OK);
    size_t n = 0;
    REQUIRE(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                   record, &n, nullptr) == CLAY_OK);
    REQUIRE(n > 0);
    REQUIRE(stats_of(record).sculpt_layer == first);
    REQUIRE(clay_stroke_tx_append(tx, samples.data() + 12, 12, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);

    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, second, &err) == CLAY_OK);
    const Snapshot held = snapshot(f.surface);
    n = 7;
    CHECK(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                 record, &n,
                                                 nullptr) == CLAY_ERROR_SNAPSHOT_MISMATCH);
    CHECK(n == 0);
    CHECK(snapshot(f.surface) == held);
    CHECK(stats_of(record).sculpt_layer == first);

    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, first, &err) == CLAY_OK);
    clay_multires_stamp_report broken{};
    broken.struct_size = 3;
    n = 7;
    CHECK(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                 record, &n,
                                                 &broken) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(n == 0);
    CHECK(snapshot(f.surface) == held);

    REQUIRE(clay_multires_sculptor_apply_stroke_tx(f.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                   record, &n, nullptr) == CLAY_OK);
    CHECK(n > 0);
    clay_stroke_tx_destroy(tx);
    const Snapshot after = snapshot(f.surface);
    CHECK(after == snapshot(whole.surface));
    check_same_record(record, expected);
    check_round_trip(f.surface, record, before, after);
    clay_multires_delta_destroy(record);
    clay_multires_delta_destroy(expected);
}
