// A stroke resolved as it arrives, across the C ABI (#670).
//
// The claims, each asserted as bytes or counts:
//   - a session fed one path in batches of 1, 5, 40 or an uneven schedule
//     holds, once ended, exactly the stamps clay_stroke_resolve_full gives the
//     whole path, for every preset field;
//   - each consumer, fed after every append, leaves its target exactly as the
//     whole-path call leaves it — the SDF node list byte for byte, the voxel
//     cells, the mask, the mesh positions — and one gesture is one undo step;
//   - the gesture is ONE gesture on a sculptor: a grab carries its region
//     across calls, which a stroke per call would not;
//   - a session is bound to one target and one brush, and says so.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"

namespace {

clay_stroke_preset defaults() {
    clay_stroke_preset p;
    p.struct_size = sizeof(p);
    REQUIRE(clay_stroke_preset_defaults(&p) == CLAY_OK);
    return p;
}

// A path with every tablet channel moving. `extras` false leaves azimuth,
// velocity and timestamp at zero, which is what the flat count*5 packing the
// older whole-path consumers take means — so the same path can feed both.
std::vector<clay_stroke_sample_full> path(int n, float from_x, float to_x, bool extras) {
    std::vector<clay_stroke_sample_full> out;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        // Uneven spacing along x, so batches do not line up with stations.
        const float u = t * t * 0.4f + t * 0.6f;
        clay_stroke_sample_full s{};
        s.position[0] = from_x + (to_x - from_x) * u;
        s.position[1] = 0.04f * std::sin(static_cast<float>(i) * 0.4f);
        s.position[2] = 0.03f * std::cos(static_cast<float>(i) * 0.23f);
        s.pressure = 0.4f + 0.5f * static_cast<float>((i * 7) % 11) / 10.0f;
        s.tilt = 0.05f * static_cast<float>(i % 4);
        if (extras) {
            s.azimuth = 0.15f * static_cast<float>(i);
            s.velocity = 0.3f + 0.2f * static_cast<float>(i % 5);
            s.timestamp = 0.004 * static_cast<double>(i);
        }
        out.push_back(s);
    }
    return out;
}

std::vector<float> flat(const std::vector<clay_stroke_sample_full>& samples) {
    std::vector<float> out;
    for (const clay_stroke_sample_full& s : samples)
        out.insert(out.end(), {s.position[0], s.position[1], s.position[2], s.pressure, s.tilt});
    return out;
}

std::vector<std::vector<std::size_t>> schedules(std::size_t n) {
    std::vector<std::vector<std::size_t>> out;
    for (std::size_t size : {n, std::size_t{1}, std::size_t{5}, std::size_t{40}}) {
        std::vector<std::size_t> s;
        for (std::size_t done = 0; done < n; done += size) s.push_back(std::min(size, n - done));
        out.push_back(s);
    }
    std::vector<std::size_t> uneven;
    std::uint32_t h = 0x9E3779B9u;
    for (std::size_t done = 0; done < n;) {
        h = h * 1664525u + 1013904223u;
        const std::size_t size = std::min<std::size_t>(1 + (h >> 24) % 11, n - done);
        uneven.push_back(size);
        done += size;
    }
    out.push_back(uneven);
    return out;
}

std::vector<clay_stamp> stamps_of(const clay_stroke_tx* tx) {
    std::size_t n = 0;
    REQUIRE(clay_stroke_tx_stamps(tx, nullptr, &n) == CLAY_OK);
    std::vector<clay_stamp> out(n);
    REQUIRE(clay_stroke_tx_stamps(tx, out.data(), &n) == CLAY_OK);
    return out;
}

std::vector<clay_stamp> resolve_whole(const std::vector<clay_stroke_sample_full>& samples,
                                      const clay_stroke_preset& preset) {
    std::size_t n = 0;
    REQUIRE(clay_stroke_resolve_full(samples.data(), samples.size(), &preset, nullptr, &n) ==
            CLAY_OK);
    std::vector<clay_stamp> out(n);
    REQUIRE(clay_stroke_resolve_full(samples.data(), samples.size(), &preset, out.data(), &n) ==
            CLAY_OK);
    return out;
}

bool same_stamps(const std::vector<clay_stamp>& a, const std::vector<clay_stamp>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(clay_stamp)) == 0);
}

clay_stroke_tx_status status_of(const clay_stroke_tx* tx) {
    clay_stroke_tx_status s{};
    s.struct_size = sizeof(s);
    REQUIRE(clay_stroke_tx_status_get(tx, &s) == CLAY_OK);
    return s;
}

// Drive a session: after every batch, append and let `apply` consume; then
// end and apply once more, which closes the gesture.
template <typename Apply>
void run_session(clay_stroke_tx* tx, const std::vector<clay_stroke_sample_full>& samples,
                 std::size_t batch, Apply apply) {
    for (std::size_t done = 0; done < samples.size(); done += batch) {
        const std::size_t n = std::min(batch, samples.size() - done);
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + done, n, nullptr, nullptr) == CLAY_OK);
        apply();
    }
    REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);
    apply();
}

clay_stroke_tx* begin(const clay_stroke_preset& preset) {
    clay_stroke_tx* tx = nullptr;
    REQUIRE(clay_stroke_tx_begin(&preset, &tx) == CLAY_OK);
    REQUIRE(tx != nullptr);
    return tx;
}

std::vector<std::uint8_t> saved(const clay_document* doc) {
    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc, &blob) == CLAY_OK);
    const std::uint8_t* d = clay_blob_data(blob);
    std::vector<std::uint8_t> out(d, d + clay_blob_size(blob));
    clay_blob_destroy(blob);
    return out;
}

std::size_t undo_depth(const clay_document* doc) {
    std::int32_t enabled = 0;
    std::size_t depth = 0, redo = 0;
    REQUIRE(clay_document_undo_state(doc, &enabled, &depth, &redo) == CLAY_OK);
    return depth;
}

}  // namespace

TEST_CASE("c stroke session: batches resolve to the whole path's stamps, every preset field") {
    const std::vector<clay_stroke_sample_full> samples = path(80, -1.0f, 1.0f, true);
    struct Case {
        const char* name;
        void (*set)(clay_stroke_preset*);
    };
    const Case cases[] = {
        {"plain", [](clay_stroke_preset*) {}},
        {"spacing", [](clay_stroke_preset* p) { p->spacing = 0.13f; }},
        {"taper", [](clay_stroke_preset* p) { p->taper_start = 0.2f, p->taper_end = 0.35f; }},
        {"steady", [](clay_stroke_preset* p) { p->steady = 0.7f; }},
        {"jitter",
         [](clay_stroke_preset* p) {
             p->jitter_position = 0.3f, p->jitter_size = 0.4f, p->jitter_rotation = 0.9f;
             p->seed = 5;
         }},
        {"pressure",
         [](clay_stroke_preset* p) {
             p->pressure_size = 0.8f, p->pressure_strength = 0.6f, p->pressure_curve = 1.8f;
         }},
        {"velocity",
         [](clay_stroke_preset* p) {
             p->velocity_size = -0.4f, p->velocity_strength = 0.5f, p->velocity_reference = 1.0f;
         }},
        {"rotation", [](clay_stroke_preset* p) { p->rotate_along_stroke = 1; }},
        {"azimuth", [](clay_stroke_preset* p) { p->rotate_to_azimuth = 1; }},
        {"clamped",
         [](clay_stroke_preset* p) {
             p->accumulation = CLAY_ACCUMULATION_CLAMPED, p->taper_end = 0.2f;
         }},
    };
    for (const Case& c : cases) {
        clay_stroke_preset preset = defaults();
        preset.radius = 0.06f;
        c.set(&preset);
        const std::vector<clay_stamp> whole = resolve_whole(samples, preset);
        REQUIRE(whole.size() > 10);
        for (const std::vector<std::size_t>& schedule : schedules(samples.size())) {
            CAPTURE(c.name);
            CAPTURE(schedule.size());
            clay_stroke_tx* tx = begin(preset);
            std::size_t done = 0, total_new = 0;
            for (std::size_t n : schedule) {
                std::size_t fresh = 0, revised = 0;
                REQUIRE(clay_stroke_tx_append(tx, samples.data() + done, n, &fresh, &revised) ==
                        CLAY_OK);
                done += n;
                total_new += fresh;
                CHECK(revised <= status_of(tx).stamps);
            }
            REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);
            CHECK(same_stamps(stamps_of(tx), whole));
            CHECK(total_new == whole.size());
            const clay_stroke_tx_status st = status_of(tx);
            CHECK(st.ended == 1);
            CHECK(st.samples == samples.size());
            CHECK(st.settled == whole.size());
            CHECK(st.bound == 0);
            clay_stroke_tx_destroy(tx);
        }
    }
}

TEST_CASE("c stroke session: the handle's own contract") {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.05f;
    preset.taper_end = 0.25f;
    const std::vector<clay_stroke_sample_full> samples = path(40, -1.0f, 1.0f, false);

    SUBCASE("begin validates the preset as resolve does") {
        clay_stroke_preset bad = preset;
        bad.radius = 0.0f;
        clay_stroke_tx* tx = reinterpret_cast<clay_stroke_tx*>(&bad);
        CHECK(clay_stroke_tx_begin(&bad, &tx) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(tx == nullptr);
        CHECK(clay_stroke_tx_begin(&preset, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        clay_stroke_tx_destroy(nullptr);
    }

    SUBCASE("the end taper waits, and settles at the end") {
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), 20, nullptr, nullptr) == CLAY_OK);
        const clay_stroke_tx_status mid = status_of(tx);
        CHECK(mid.settled > 0);
        CHECK(mid.settled < mid.stamps);
        std::size_t fresh = 0, revised = 0;
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + 20, 20, &fresh, &revised) == CLAY_OK);
        CHECK(fresh > 0);
        CHECK(revised >= mid.settled);
        CHECK(revised < mid.stamps);  // the old tail was tapering and is not now
        REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);
        REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);  // idempotent
        CHECK(status_of(tx).settled == status_of(tx).stamps);
        // Ended: no more samples, and the refusal appends nothing.
        CHECK(clay_stroke_tx_append(tx, samples.data(), 1, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(status_of(tx).samples == samples.size());
        clay_stroke_tx_destroy(tx);
    }

    SUBCASE("stamps follow the size-query pattern") {
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), samples.size(), nullptr, nullptr) ==
                CLAY_OK);
        std::size_t n = 0;
        REQUIRE(clay_stroke_tx_stamps(tx, nullptr, &n) == CLAY_OK);
        REQUIRE(n > 2);
        std::vector<clay_stamp> buf(n);
        std::size_t small = n - 1;
        CHECK(clay_stroke_tx_stamps(tx, buf.data(), &small) == CLAY_ERROR_BUFFER_TOO_SMALL);
        CHECK(small == n);
        CHECK(clay_stroke_tx_stamps(tx, buf.data(), nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_stroke_tx_append(tx, nullptr, 3, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_stroke_tx_append(tx, nullptr, 0, nullptr, nullptr) == CLAY_OK);
        clay_stroke_tx_status small_status{};
        small_status.struct_size = 4;
        CHECK(clay_stroke_tx_status_get(tx, &small_status) == CLAY_ERROR_INVALID_ARGUMENT);
        clay_stroke_tx_destroy(tx);
    }
}

TEST_CASE("c stroke session: a layer gesture is the whole-path node list, as one undo step") {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.08f;
    preset.spacing = 0.3f;
    preset.taper_end = 0.3f;
    preset.jitter_position = 0.2f;
    preset.seed = 3;
    preset.pressure_size = 0.5f;
    const std::vector<clay_stroke_sample_full> samples = path(60, -1.0f, 1.0f, false);
    const std::vector<float> packed = flat(samples);
    const float r[1] = {1.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(item != nullptr);

    auto fresh_doc = [](clay_layer_id* layer) {
        clay_document* doc = clay_document_create();
        REQUIRE(clay_add_sdf_layer(doc, "body", layer) == CLAY_OK);
        REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
        return doc;
    };

    clay_layer_id whole_layer = 0;
    clay_document* whole = fresh_doc(&whole_layer);
    std::size_t whole_nodes = 0;
    REQUIRE(clay_layer_apply_stroke(whole, whole_layer, packed.data(), samples.size(), &preset,
                                    item, nullptr, nullptr, &whole_nodes) == CLAY_OK);
    REQUIRE(whole_nodes > 5);
    const std::vector<std::uint8_t> expected = saved(whole);

    for (std::size_t batch : {std::size_t{1}, std::size_t{7}, std::size_t{25}}) {
        CAPTURE(batch);
        clay_layer_id layer = 0;
        clay_document* doc = fresh_doc(&layer);
        clay_stroke_tx* tx = begin(preset);
        std::size_t nodes = 0, calls_with_nodes = 0;
        run_session(tx, samples, batch, [&] {
            std::size_t n = 0;
            REQUIRE(clay_layer_apply_stroke_tx(doc, layer, tx, item, nullptr, nullptr, &n) ==
                    CLAY_OK);
            nodes += n;
            if (n > 0) ++calls_with_nodes;
        });
        CHECK(nodes == whole_nodes);
        if (batch < 25) CHECK(calls_with_nodes > 1);  // ink arrived under the pen
        CHECK(saved(doc) == expected);
        CHECK(status_of(tx).closed == 1);
        // ONE step for the gesture, however many calls carried it.
        CHECK(undo_depth(doc) == 1);
        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(doc, &undone) == CLAY_OK);
        CHECK(undone == 1);
        CHECK(undo_depth(doc) == 0);
        // A closed gesture applies nothing more.
        std::size_t after = 0;
        CHECK(clay_layer_apply_stroke_tx(doc, layer, tx, item, nullptr, nullptr, &after) ==
              CLAY_OK);
        CHECK(after == 0);
        clay_stroke_tx_destroy(tx);
        clay_document_destroy(doc);
    }

    SUBCASE("the item is copied at the bind; the binding is checked") {
        clay_layer_id layer = 0;
        clay_document* doc = fresh_doc(&layer);
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), 30, nullptr, nullptr) == CLAY_OK);
        std::size_t n = 0;
        REQUIRE(clay_layer_apply_stroke_tx(doc, layer, tx, item, nullptr, nullptr, &n) == CLAY_OK);
        CHECK(status_of(tx).bound == 1);
        // Another layer, another item or a mask that was not there: refused,
        // and nothing applied.
        clay_layer_id other = 0;
        REQUIRE(clay_add_sdf_layer(doc, "other", &other) == CLAY_OK);
        clay_item* item2 = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
        clay_mask* mask = clay_mask_create(0.1f);
        CHECK(clay_layer_apply_stroke_tx(doc, other, tx, item, nullptr, nullptr, &n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_apply_stroke_tx(doc, layer, tx, item2, nullptr, nullptr, &n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_layer_apply_stroke_tx(doc, layer, tx, item, mask, nullptr, &n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_voxel_grid* grid = clay_voxel_grid_create(0.05f);
        std::size_t applied = 0;
        CHECK(clay_voxel_apply_stroke_tx(grid, tx, 0, CLAY_BRUSH_SHAPE_SPHERE,
                                         CLAY_BRUSH_FALLOFF_SMOOTH, nullptr, &applied) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_voxel_grid_destroy(grid);
        clay_mask_destroy(mask);
        clay_item_destroy(item2);
        CHECK(clay_layer_apply_stroke_tx(doc, layer, nullptr, item, nullptr, nullptr, &n) ==
              CLAY_ERROR_INVALID_ARGUMENT);

        // Destroying an open gesture closes its undo group: the document
        // records the nodes applied so far as one step and keeps working.
        clay_stroke_tx_destroy(tx);
        CHECK(undo_depth(doc) == 1);
        std::size_t more = 0;
        REQUIRE(clay_layer_apply_stroke(doc, layer, flat(samples).data(), samples.size(), &preset,
                                        item, nullptr, nullptr, &more) == CLAY_OK);
        CHECK(undo_depth(doc) == 2);
        clay_document_destroy(doc);
    }

    clay_document_destroy(whole);
    clay_item_destroy(item);
}

TEST_CASE("c stroke session: a start taper applies nothing until the pen lifts") {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.08f;
    preset.taper_start = 0.2f;
    const std::vector<clay_stroke_sample_full> samples = path(40, -1.0f, 1.0f, false);
    const float r[1] = {1.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_stroke_tx* tx = begin(preset);
    std::size_t before_end = 0;
    for (std::size_t i = 0; i < samples.size(); i += 8) {
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + i, 8, nullptr, nullptr) == CLAY_OK);
        std::size_t n = 0;
        REQUIRE(clay_layer_apply_stroke_tx(doc, layer, tx, item, nullptr, nullptr, &n) == CLAY_OK);
        before_end += n;
    }
    CHECK(before_end == 0);
    CHECK(status_of(tx).settled == 0);
    REQUIRE(clay_stroke_tx_end(tx) == CLAY_OK);
    std::size_t at_end = 0;
    REQUIRE(clay_layer_apply_stroke_tx(doc, layer, tx, item, nullptr, nullptr, &at_end) == CLAY_OK);
    CHECK(at_end > 5);
    clay_stroke_tx_destroy(tx);
    clay_document_destroy(doc);
    clay_item_destroy(item);
}

namespace {

std::vector<std::int32_t> occupied(const clay_voxel_grid* grid) {
    std::size_t n = 0;
    REQUIRE(clay_voxel_get_occupied(grid, nullptr, nullptr, 0, &n) == CLAY_OK);
    std::vector<std::int32_t> xyz(n * 3), index(n);
    REQUIRE(clay_voxel_get_occupied(grid, xyz.data(), index.data(), n, &n) == CLAY_OK);
    xyz.insert(xyz.end(), index.begin(), index.end());
    return xyz;
}

}  // namespace

TEST_CASE("c stroke session: voxel cells are the whole-path cells, dither included") {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.12f;
    preset.spacing = 0.3f;
    // Below full strength the smooth falloff dithers, and the dither is seeded
    // per stamp by its index in the STROKE.
    preset.strength = 0.55f;
    preset.taper_end = 0.2f;
    const std::vector<clay_stroke_sample_full> samples = path(60, -1.0f, 1.0f, false);
    const std::vector<float> packed = flat(samples);
    const float white[3] = {1.0f, 1.0f, 1.0f};

    auto fresh_grid = [&](std::int32_t* index) {
        clay_voxel_grid* g = clay_voxel_grid_create(0.03f);
        REQUIRE(clay_voxel_palette_add(g, white, index) == CLAY_OK);
        return g;
    };
    std::int32_t index = 0;
    clay_voxel_grid* whole = fresh_grid(&index);
    std::size_t whole_applied = 0;
    REQUIRE(clay_voxel_apply_stroke(whole, packed.data(), samples.size(), &preset, index,
                                    CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH, nullptr,
                                    &whole_applied) == CLAY_OK);
    const std::vector<std::int32_t> expected = occupied(whole);
    REQUIRE(expected.size() > 30);

    for (std::size_t batch : {std::size_t{1}, std::size_t{9}}) {
        CAPTURE(batch);
        std::int32_t gi = 0;
        clay_voxel_grid* grid = fresh_grid(&gi);
        clay_stroke_tx* tx = begin(preset);
        std::size_t applied = 0;
        run_session(tx, samples, batch, [&] {
            std::size_t n = 0;
            REQUIRE(clay_voxel_apply_stroke_tx(grid, tx, gi, CLAY_BRUSH_SHAPE_SPHERE,
                                               CLAY_BRUSH_FALLOFF_SMOOTH, nullptr, &n) == CLAY_OK);
            applied += n;
        });
        CHECK(applied == whole_applied);
        CHECK(occupied(grid) == expected);
        // A different scalar is a different brush.
        std::size_t n = 0;
        CHECK(clay_voxel_apply_stroke_tx(grid, tx, gi, CLAY_BRUSH_SHAPE_CUBE,
                                         CLAY_BRUSH_FALLOFF_SMOOTH, nullptr, &n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_stroke_tx_destroy(tx);
        clay_voxel_grid_destroy(grid);
    }

    SUBCASE("on a document's grid the gesture is one undo step") {
        clay_document* doc = clay_document_create();
        REQUIRE(clay_document_enable_undo(doc) == CLAY_OK);
        clay_layer_id layer = 0;
        clay_voxel_grid* grid = nullptr;
        REQUIRE(clay_document_add_voxel_layer(doc, "v", 0.03f, &layer, &grid) == CLAY_OK);
        std::int32_t gi = 0;
        REQUIRE(clay_voxel_palette_add(grid, white, &gi) == CLAY_OK);
        const std::size_t depth = undo_depth(doc);
        clay_stroke_tx* tx = begin(preset);
        run_session(tx, samples, 4, [&] {
            REQUIRE(clay_voxel_apply_stroke_tx(grid, tx, gi, CLAY_BRUSH_SHAPE_SPHERE,
                                               CLAY_BRUSH_FALLOFF_SMOOTH, nullptr,
                                               nullptr) == CLAY_OK);
        });
        CHECK(undo_depth(doc) == depth + 1);
        CHECK(occupied(grid).size() == expected.size());
        clay_stroke_tx_destroy(tx);
        clay_document_destroy(doc);
    }
    clay_voxel_grid_destroy(whole);
}

TEST_CASE("c stroke session: a mask stroke paints the whole-path mask") {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.1f;
    preset.spacing = 0.2f;
    preset.strength = 0.4f;
    preset.taper_end = 0.3f;
    const std::vector<clay_stroke_sample_full> samples = path(50, -1.0f, 1.0f, false);
    const std::vector<float> packed = flat(samples);

    std::vector<float> probes;
    for (int i = 0; i <= 120; ++i)
        for (int k = -3; k <= 3; ++k)
            probes.insert(probes.end(), {-1.2f + 0.02f * static_cast<float>(i),
                                         0.03f * static_cast<float>(k), 0.0f});
    auto values = [&](const clay_mask* m) {
        std::vector<float> out(probes.size() / 3);
        REQUIRE(clay_mask_sample_many(m, probes.data(), out.size(), out.data()) == CLAY_OK);
        return out;
    };

    clay_mask* whole = clay_mask_create(0.02f);
    std::size_t whole_applied = 0;
    REQUIRE(clay_mask_apply_stroke(whole, packed.data(), samples.size(), &preset, 1.0f,
                                   CLAY_BRUSH_SHAPE_SPHERE, CLAY_BRUSH_FALLOFF_SMOOTH,
                                   &whole_applied) == CLAY_OK);
    const std::vector<float> expected = values(whole);

    clay_mask* mask = clay_mask_create(0.02f);
    clay_stroke_tx* tx = begin(preset);
    std::size_t applied = 0;
    run_session(tx, samples, 6, [&] {
        std::size_t n = 0;
        REQUIRE(clay_mask_apply_stroke_tx(mask, tx, 1.0f, CLAY_BRUSH_SHAPE_SPHERE,
                                          CLAY_BRUSH_FALLOFF_SMOOTH, &n) == CLAY_OK);
        applied += n;
    });
    CHECK(applied == whole_applied);
    const std::vector<float> got = values(mask);
    CHECK(std::memcmp(got.data(), expected.data(), got.size() * sizeof(float)) == 0);
    clay_stroke_tx_destroy(tx);
    clay_mask_destroy(mask);
    clay_mask_destroy(whole);
}

namespace {

clay_mesh* plane_mesh(int n, float half) {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            positions.insert(positions.end(), {-half + step * static_cast<float>(x), 0.0f,
                                               -half + step * static_cast<float>(z)});
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(n); ++z)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(n); ++x) {
            const std::uint32_t a = z * stride + x, b = a + 1, c = a + stride, d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    clay_mesh* m = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &m) == CLAY_OK);
    return m;
}

std::vector<float> positions_of(const clay_mesh* m) {
    const float* p = clay_mesh_positions(m);
    return std::vector<float>(p, p + clay_mesh_vertex_count(m) * 3);
}

bool same_floats(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

clay_mesh_brush_desc mesh_brush(std::int32_t verb, float radius, float strength) {
    clay_mesh_brush_desc d;
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = verb;
    d.radius = radius;
    d.strength = strength;
    return d;
}

// A drag that rises out of the plane as it goes, so a grab has somewhere to
// carry its region and a draw has a surface to push.
std::vector<clay_stroke_sample_full> drag(int n) {
    std::vector<clay_stroke_sample_full> out = path(n, -0.5f, 0.5f, false);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i].position[1] = 0.3f * static_cast<float>(i) / static_cast<float>(out.size());
    return out;
}

// The positions a mesh stroke leaves: whole-path, or through a session fed in
// `batch`-sized appends with a consumer call after each. `frame` declares a
// world frame on the sculptor first.
struct MeshRun {
    std::vector<float> positions;
    std::vector<float> reverted;
    std::size_t applied = 0;
};

MeshRun mesh_run(std::int32_t verb, std::size_t batch, const clay_mesh_frame* frame) {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.25f;
    preset.spacing = 0.2f;
    preset.strength = 0.7f;
    preset.taper_end = 0.2f;
    const std::vector<clay_stroke_sample_full> samples = drag(40);
    const clay_mesh_brush_desc desc = mesh_brush(verb, 0.25f, 0.6f);

    clay_mesh* m = plane_mesh(24, 1.0f);
    const std::vector<float> before = positions_of(m);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
    if (frame) REQUIRE(clay_mesh_sculptor_set_world_frame(s, frame) == CLAY_OK);
    clay_mesh_deltas* deltas = clay_mesh_deltas_create();
    MeshRun out;
    if (batch == 0) {
        const std::vector<float> packed = flat(samples);
        REQUIRE(clay_mesh_sculptor_apply_stroke(s, packed.data(), samples.size(), &preset, &desc,
                                                nullptr, nullptr, 1, deltas,
                                                &out.applied) == CLAY_OK);
    } else {
        clay_stroke_tx* tx = begin(preset);
        run_session(tx, samples, batch, [&] {
            std::size_t n = 0;
            REQUIRE(clay_mesh_sculptor_apply_stroke_tx(s, tx, &desc, nullptr, nullptr, 1, deltas,
                                                       &n) == CLAY_OK);
            out.applied += n;
        });
        CHECK(status_of(tx).closed == 1);
        clay_stroke_tx_destroy(tx);
    }
    out.positions = positions_of(m);
    CHECK_FALSE(same_floats(out.positions, before));
    // One record for the gesture: one revert takes all of it back.
    REQUIRE(clay_mesh_deltas_revert(deltas, s) == CLAY_OK);
    out.reverted = positions_of(m);
    CHECK(same_floats(out.reverted, before));
    clay_mesh_deltas_destroy(deltas);
    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
    return out;
}

}  // namespace

TEST_CASE("c stroke session: a mesh gesture is the whole-path gesture, grab included") {
    for (std::int32_t verb : {CLAY_MESH_BRUSH_DRAW, CLAY_MESH_BRUSH_GRAB, CLAY_MESH_BRUSH_SNAKEHOOK}) {
        CAPTURE(verb);
        const MeshRun whole = mesh_run(verb, 0, nullptr);
        REQUIRE(whole.applied > 3);
        for (std::size_t batch : {std::size_t{1}, std::size_t{6}}) {
            CAPTURE(batch);
            const MeshRun pieces = mesh_run(verb, batch, nullptr);
            CHECK(pieces.applied == whole.applied);
            CHECK(same_floats(pieces.positions, whole.positions));
        }
    }

    SUBCASE("through a declared world frame, sample by sample") {
        clay_mesh_frame frame{};
        frame.struct_size = sizeof(frame);
        frame.position[0] = 0.3f;
        frame.position[1] = -0.2f;
        frame.rotation[3] = 1.0f;
        frame.scale = 1.5f;
        const MeshRun whole = mesh_run(CLAY_MESH_BRUSH_GRAB, 0, &frame);
        const MeshRun pieces = mesh_run(CLAY_MESH_BRUSH_GRAB, 5, &frame);
        CHECK(same_floats(pieces.positions, whole.positions));
    }
}

TEST_CASE("c stroke session: a grab is one gesture, not a stroke per call") {
    // The defect a session exists to remove, made visible: feeding each batch
    // to the WHOLE-PATH call as its own stroke re-gathers the grab's region at
    // every call and lands somewhere else.
    clay_stroke_preset preset = defaults();
    preset.radius = 0.25f;
    preset.spacing = 0.2f;
    const std::vector<clay_stroke_sample_full> samples = drag(40);
    const clay_mesh_brush_desc desc = mesh_brush(CLAY_MESH_BRUSH_GRAB, 0.25f, 0.6f);
    clay_mesh* m = plane_mesh(24, 1.0f);
    clay_mesh_sculptor* s = nullptr;
    REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
    for (std::size_t i = 0; i < samples.size(); i += 8) {
        const std::vector<clay_stroke_sample_full> piece(
            samples.begin() + static_cast<std::ptrdiff_t>(i),
            samples.begin() + static_cast<std::ptrdiff_t>(std::min(i + 8, samples.size())));
        const std::vector<float> packed = flat(piece);
        REQUIRE(clay_mesh_sculptor_apply_stroke(s, packed.data(), piece.size(), &preset, &desc,
                                                nullptr, nullptr, 1, nullptr,
                                                nullptr) == CLAY_OK);
    }
    const std::vector<float> per_call = positions_of(m);
    clay_mesh_sculptor_destroy(s);
    clay_mesh_destroy(m);
    const MeshRun session = mesh_run(CLAY_MESH_BRUSH_GRAB, 8, nullptr);
    CHECK_FALSE(same_floats(per_call, session.positions));
}

namespace {

clay_dynamic_surface* sphere_surface(int n) {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(positions.size() / 3);
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                for (int k = 0; k < 3; ++k) positions.push_back(c[k] / len);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    indices.insert(indices.end(), {a, c2, b, b, c2, d});
                else
                    indices.insert(indices.end(), {a, b, c2, b, d, c2});
            }
    }
    clay_mesh* mesh = nullptr;
    REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                     indices.size(), &mesh) == CLAY_OK);
    clay_dynamic_surface* surface = nullptr;
    std::int32_t err = -1;
    REQUIRE(clay_dynamic_surface_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
    clay_mesh_destroy(mesh);
    return surface;
}

struct DynamicExport {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
};

DynamicExport export_of(const clay_dynamic_surface* surface) {
    clay_mesh* m = nullptr;
    REQUIRE(clay_dynamic_surface_to_mesh(surface, &m) == CLAY_OK);
    DynamicExport out;
    out.positions = positions_of(m);
    const std::uint32_t* idx = clay_mesh_indices(m);
    out.indices.assign(idx, idx + clay_mesh_index_count(m));
    clay_mesh_destroy(m);
    return out;
}

bool same_export(const DynamicExport& a, const DynamicExport& b) {
    return same_floats(a.positions, b.positions) && a.indices == b.indices;
}

struct DynamicRun {
    DynamicExport after;
    bool reverted = false;
    std::size_t applied = 0;
};

DynamicRun dynamic_run(std::int32_t verb, std::size_t batch) {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.3f;
    preset.spacing = 0.25f;
    preset.taper_end = 0.2f;
    std::vector<clay_stroke_sample_full> samples = path(30, -0.4f, 0.4f, true);
    for (clay_stroke_sample_full& s : samples) {
        s.position[1] *= 0.5f;
        s.position[2] = 1.0f + 0.1f * s.position[0];
    }
    clay_mesh_brush_desc brush = mesh_brush(verb, 0.3f, 0.4f);
    clay_dynamic_surface* surface = sphere_surface(16);
    clay_dynamic_sculptor* sculptor = nullptr;
    REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
    const DynamicExport before = export_of(surface);
    clay_dynamic_delta* record = clay_dynamic_delta_create();
    DynamicRun out;
    if (batch == 0) {
        REQUIRE(clay_dynamic_sculptor_apply_stroke_recorded(sculptor, samples.data(),
                                                            samples.size(), &preset, &brush,
                                                            nullptr, nullptr, 0, record,
                                                            &out.applied, nullptr) == CLAY_OK);
    } else {
        clay_stroke_tx* tx = begin(preset);
        run_session(tx, samples, batch, [&] {
            std::size_t n = 0;
            clay_dynamic_stamp_report report{};
            report.struct_size = sizeof(report);
            REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr,
                                                          0, record, &n, &report) == CLAY_OK);
            out.applied += n;
        });
        clay_stroke_tx_destroy(tx);
    }
    out.after = export_of(surface);
    REQUIRE(clay_dynamic_delta_revert(record, sculptor) == CLAY_OK);
    out.reverted = same_export(export_of(surface), before);
    clay_dynamic_delta_destroy(record);
    clay_dynamic_sculptor_destroy(sculptor);
    clay_dynamic_surface_destroy(surface);
    return out;
}

}  // namespace

TEST_CASE("c stroke session: an adaptive gesture is the whole-path gesture, record included") {
    for (std::int32_t verb : {CLAY_MESH_BRUSH_DRAW, CLAY_MESH_BRUSH_GRAB}) {
        CAPTURE(verb);
        const DynamicRun whole = dynamic_run(verb, 0);
        REQUIRE(whole.applied > 3);
        CHECK(whole.reverted);
        const DynamicRun pieces = dynamic_run(verb, 4);
        CHECK(pieces.applied == whole.applied);
        CHECK(same_export(pieces.after, whole.after));
        // One record across every call: one revert takes the gesture back.
        CHECK(pieces.reverted);
    }

    SUBCASE("a record the surface has moved past is refused, and loses no stamps") {
        clay_stroke_preset preset = defaults();
        preset.radius = 0.3f;
        std::vector<clay_stroke_sample_full> samples = path(20, -0.4f, 0.4f, false);
        for (clay_stroke_sample_full& s : samples) s.position[2] = 1.0f;  // on the surface
        clay_mesh_brush_desc brush = mesh_brush(CLAY_MESH_BRUSH_DRAW, 0.3f, 0.4f);
        clay_dynamic_surface* surface = sphere_surface(12);
        clay_dynamic_sculptor* sculptor = nullptr;
        REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
        clay_dynamic_delta* record = clay_dynamic_delta_create();
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), 10, nullptr, nullptr) == CLAY_OK);
        std::size_t first = 0;
        REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr, 0,
                                                      record, &first, nullptr) == CLAY_OK);
        REQUIRE(first > 0);  // the record holds something, so it is bound
        // An unrecorded stamp in between: the record no longer ends here.
        clay_mesh_brush_desc stray = brush;
        stray.center[2] = 1.0f;
        REQUIRE(clay_dynamic_sculptor_stamp(sculptor, &stray, nullptr, nullptr, nullptr) ==
                CLAY_OK);
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + 10, 10, nullptr, nullptr) == CLAY_OK);
        const clay_stroke_tx_status st = status_of(tx);
        std::size_t n = 7;
        CHECK(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr, 0,
                                                    record, &n, nullptr) ==
              CLAY_ERROR_SNAPSHOT_MISMATCH);
        CHECK(n == 0);
        CHECK(status_of(tx).settled == st.settled);
        // Once the host has sorted out its history the held stamps apply: the
        // refusal took none of them.
        REQUIRE(clay_dynamic_delta_clear(record) == CLAY_OK);
        REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr, 0,
                                                      record, &n, nullptr) == CLAY_OK);
        CHECK(n > 0);
        clay_stroke_tx_destroy(tx);
        clay_dynamic_delta_destroy(record);
        clay_dynamic_sculptor_destroy(sculptor);
        clay_dynamic_surface_destroy(surface);
    }

    SUBCASE("a call after the close leaves the record's marks alone") {
        // The multires consumer once re-bound its record on a call after the
        // close. The adaptive one must not re-mark either: a record that the
        // surface has moved past stays refused, however many no-op calls the
        // host makes after the gesture ended.
        clay_stroke_preset preset = defaults();
        preset.radius = 0.3f;
        std::vector<clay_stroke_sample_full> samples = path(20, -0.4f, 0.4f, false);
        for (clay_stroke_sample_full& s : samples) s.position[2] = 1.0f;
        clay_mesh_brush_desc brush = mesh_brush(CLAY_MESH_BRUSH_DRAW, 0.3f, 0.4f);
        clay_dynamic_surface* surface = sphere_surface(12);
        clay_dynamic_sculptor* sculptor = nullptr;
        REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
        clay_dynamic_delta* record = clay_dynamic_delta_create();
        clay_stroke_tx* tx = begin(preset);
        std::size_t applied = 0;
        run_session(tx, samples, 5, [&] {
            std::size_t n = 0;
            REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr,
                                                          0, record, &n, nullptr) == CLAY_OK);
            applied += n;
        });
        REQUIRE(applied > 0);
        clay_mesh_brush_desc stray = brush;
        stray.center[2] = 1.0f;
        REQUIRE(clay_dynamic_sculptor_stamp(sculptor, &stray, nullptr, nullptr, nullptr) ==
                CLAY_OK);
        std::size_t n = 7;
        REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, nullptr, nullptr, 0,
                                                      record, &n, nullptr) == CLAY_OK);
        CHECK(n == 0);
        CHECK(clay_dynamic_delta_revert(record, sculptor) == CLAY_ERROR_SNAPSHOT_MISMATCH);
        clay_stroke_tx_destroy(tx);
        clay_dynamic_delta_destroy(record);
        clay_dynamic_sculptor_destroy(sculptor);
        clay_dynamic_surface_destroy(surface);
    }
}

namespace {

struct Multires {
    clay_mesh* mesh = nullptr;
    clay_multires* surface = nullptr;
    clay_multires_sculptor* sculptor = nullptr;
    Multires() {
        mesh = plane_mesh(5, 2.0f);
        clay_multires_desc desc{};
        desc.struct_size = sizeof(desc);
        REQUIRE(clay_multires_defaults(&desc) == CLAY_OK);
        std::int32_t err = -1;
        REQUIRE(clay_multires_from_mesh(mesh, &desc, &surface, &err) == CLAY_OK);
        for (int i = 0; i < 2; ++i) REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
        REQUIRE(clay_multires_set_sculpt_level(surface, 2) == CLAY_OK);
        REQUIRE(clay_multires_sculptor_create(surface, &sculptor) == CLAY_OK);
    }
    ~Multires() {
        clay_multires_sculptor_destroy(sculptor);
        clay_multires_destroy(surface);
        clay_mesh_destroy(mesh);
    }
    Multires(const Multires&) = delete;
    Multires& operator=(const Multires&) = delete;
    std::vector<float> level() const {
        clay_mesh* m = nullptr;
        REQUIRE(clay_multires_copy_level_mesh(surface, 2, &m) == CLAY_OK);
        std::vector<float> out = positions_of(m);
        clay_mesh_destroy(m);
        return out;
    }
};

}  // namespace

TEST_CASE("c stroke session: a multires gesture is the whole-path gesture") {
    clay_stroke_preset preset = defaults();
    preset.radius = 0.5f;
    preset.spacing = 0.2f;
    preset.taper_end = 0.25f;
    std::vector<clay_stroke_sample_full> samples = path(30, -1.0f, 1.0f, false);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i].position[1] = 0.4f * static_cast<float>(i) / static_cast<float>(samples.size());
    const std::vector<float> packed = flat(samples);
    for (std::int32_t verb : {CLAY_MESH_BRUSH_DRAW, CLAY_MESH_BRUSH_GRAB}) {
        CAPTURE(verb);
        const clay_mesh_brush_desc brush = mesh_brush(verb, 0.5f, 0.5f);
        Multires whole;
        std::size_t whole_applied = 0;
        REQUIRE(clay_multires_sculptor_apply_stroke(whole.sculptor, packed.data(), samples.size(),
                                                    &preset, &brush, nullptr, nullptr, 1,
                                                    &whole_applied, nullptr) == CLAY_OK);
        REQUIRE(whole_applied > 3);

        Multires pieces;
        clay_stroke_tx* tx = begin(preset);
        std::size_t applied = 0;
        run_session(tx, samples, 5, [&] {
            std::size_t n = 0;
            clay_multires_stamp_report report{};
            report.struct_size = sizeof(report);
            REQUIRE(clay_multires_sculptor_apply_stroke_tx(pieces.sculptor, tx, &brush, nullptr,
                                                           nullptr, 1, nullptr, &n,
                                                           &report) == CLAY_OK);
            CHECK(report.level == 2);
            applied += n;
        });
        CHECK(applied == whole_applied);
        CHECK(same_floats(pieces.level(), whole.level()));
        clay_stroke_tx_destroy(tx);
    }
}

TEST_CASE("c stroke session: a sculptor's descriptors are part of the binding") {
    // The header promises a later call that changes the brush is refused. The
    // descriptors are decoded by every call and compared field by field with
    // the bind's, so a changed field, a NULL descriptor or a frame the bind did
    // not have is refused and applies nothing, while a copy with the same
    // values is the same brush.
    clay_stroke_preset preset = defaults();
    preset.radius = 0.25f;
    preset.spacing = 0.2f;

    SUBCASE("mesh: brush and frame") {
        const std::vector<clay_stroke_sample_full> samples = drag(40);
        const clay_mesh_brush_desc desc = mesh_brush(CLAY_MESH_BRUSH_DRAW, 0.25f, 0.6f);
        clay_mesh* m = plane_mesh(24, 1.0f);
        clay_mesh_sculptor* s = nullptr;
        REQUIRE(clay_mesh_sculptor_create(m, -1.0f, &s) == CLAY_OK);
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), 20, nullptr, nullptr) == CLAY_OK);
        std::size_t n = 0;
        REQUIRE(clay_mesh_sculptor_apply_stroke_tx(s, tx, &desc, nullptr, nullptr, 1, nullptr,
                                                   &n) == CLAY_OK);
        REQUIRE(n > 0);
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + 20, 10, nullptr, nullptr) == CLAY_OK);
        const std::vector<float> held = positions_of(m);

        clay_mesh_brush_desc stronger = desc;
        stronger.strength = 0.9f;
        clay_mesh_brush_desc wider = desc;  // a field the stroke replaces per stamp
        wider.radius = 0.5f;
        clay_mesh_frame frame{};
        frame.struct_size = sizeof(frame);
        frame.rotation[3] = 1.0f;
        frame.position[0] = 0.25f;
        const clay_mesh_brush_desc* refused[] = {&stronger, &wider, nullptr};
        for (const clay_mesh_brush_desc* other : refused) {
            n = 7;
            CHECK(clay_mesh_sculptor_apply_stroke_tx(s, tx, other, nullptr, nullptr, 1, nullptr,
                                                     &n) == CLAY_ERROR_INVALID_ARGUMENT);
            CHECK(n == 0);
        }
        n = 7;
        CHECK(clay_mesh_sculptor_apply_stroke_tx(s, tx, &desc, nullptr, &frame, 1, nullptr, &n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(n == 0);
        CHECK(same_floats(positions_of(m), held));

        // Another descriptor holding the same values is the same brush, and
        // the stamps the refusals held back are still there to apply.
        const clay_mesh_brush_desc copy = desc;
        REQUIRE(clay_mesh_sculptor_apply_stroke_tx(s, tx, &copy, nullptr, nullptr, 1, nullptr,
                                                   &n) == CLAY_OK);
        CHECK(n > 0);
        CHECK_FALSE(same_floats(positions_of(m), held));
        clay_stroke_tx_destroy(tx);
        clay_mesh_sculptor_destroy(s);
        clay_mesh_destroy(m);
    }

    SUBCASE("adaptive: topology") {
        std::vector<clay_stroke_sample_full> samples = path(20, -0.4f, 0.4f, false);
        for (clay_stroke_sample_full& sample : samples) sample.position[2] = 1.0f;
        const clay_mesh_brush_desc brush = mesh_brush(CLAY_MESH_BRUSH_DRAW, 0.3f, 0.4f);
        clay_dynamic_topology_desc topology{};
        topology.struct_size = sizeof(topology);
        REQUIRE(clay_dynamic_topology_defaults(&topology) == CLAY_OK);
        clay_dynamic_surface* surface = sphere_surface(12);
        clay_dynamic_sculptor* sculptor = nullptr;
        REQUIRE(clay_dynamic_sculptor_create(surface, &sculptor) == CLAY_OK);
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), 10, nullptr, nullptr) == CLAY_OK);
        std::size_t n = 0;
        REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, &topology, nullptr, 0,
                                                      nullptr, &n, nullptr) == CLAY_OK);
        REQUIRE(n > 0);
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + 10, 10, nullptr, nullptr) == CLAY_OK);
        clay_dynamic_topology_desc fewer = topology;
        fewer.max_passes = topology.max_passes + 1;
        n = 7;
        CHECK(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, &fewer, nullptr, 0,
                                                    nullptr, &n, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(n == 0);
        clay_mesh_brush_desc weaker = brush;
        weaker.strength = 0.1f;
        CHECK(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &weaker, &topology, nullptr, 0,
                                                    nullptr, &n, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(n == 0);
        REQUIRE(clay_dynamic_sculptor_apply_stroke_tx(sculptor, tx, &brush, &topology, nullptr, 0,
                                                      nullptr, &n, nullptr) == CLAY_OK);
        CHECK(n > 0);
        clay_stroke_tx_destroy(tx);
        clay_dynamic_sculptor_destroy(sculptor);
        clay_dynamic_surface_destroy(surface);
    }

    SUBCASE("multires: brush and frame") {
        preset.radius = 0.5f;
        std::vector<clay_stroke_sample_full> samples = path(30, -1.0f, 1.0f, false);
        const clay_mesh_brush_desc brush = mesh_brush(CLAY_MESH_BRUSH_DRAW, 0.5f, 0.5f);
        Multires h;
        clay_stroke_tx* tx = begin(preset);
        REQUIRE(clay_stroke_tx_append(tx, samples.data(), 15, nullptr, nullptr) == CLAY_OK);
        std::size_t n = 0;
        REQUIRE(clay_multires_sculptor_apply_stroke_tx(h.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                       nullptr, &n, nullptr) == CLAY_OK);
        REQUIRE(n > 0);
        REQUIRE(clay_stroke_tx_append(tx, samples.data() + 15, 15, nullptr, nullptr) == CLAY_OK);
        const std::vector<float> held = h.level();
        clay_mesh_brush_desc stronger = brush;
        stronger.strength = 0.8f;
        n = 7;
        CHECK(clay_multires_sculptor_apply_stroke_tx(h.sculptor, tx, &stronger, nullptr, nullptr, 1,
                                                     nullptr, &n,
                                                     nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(n == 0);
        clay_mesh_frame frame{};
        frame.struct_size = sizeof(frame);
        frame.rotation[3] = 1.0f;
        frame.scale = 2.0f;
        CHECK(clay_multires_sculptor_apply_stroke_tx(h.sculptor, tx, &brush, nullptr, &frame, 1,
                                                     nullptr, &n,
                                                     nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(n == 0);
        CHECK(same_floats(h.level(), held));
        REQUIRE(clay_multires_sculptor_apply_stroke_tx(h.sculptor, tx, &brush, nullptr, nullptr, 1,
                                                       nullptr, &n, nullptr) == CLAY_OK);
        CHECK(n > 0);
        clay_stroke_tx_destroy(tx);
    }
}
