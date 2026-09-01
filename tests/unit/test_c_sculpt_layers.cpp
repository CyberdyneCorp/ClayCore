// THE SCULPT LAYER STACK ACROSS THE C ABI (c-abi spec, add-mesh-sculpt-layers).
//
// The C++ cases prove the STACK behaves; these prove the BOUNDARY does, and the
// three things gated here cannot be reached from C++ at all:
//
//   * THE TYPED REFUSAL. The C++ API returns bool, and a host UI has to be able
//     to say three different sentences — "no layer", "that layer is locked",
//     "finish the stroke first". Every mutator carries an `out_error` for
//     exactly that, and getting the two flags behind it wrong is how a lock
//     starts meaning "hide from the UI". Each of the three is provoked here.
//   * THE NAME CROSSING INTO A CALLER'S BUFFER. A pointer into an engine-owned
//     std::string has no lifetime a host can reason about — the next rename
//     frees it — so the name goes through the size-query pattern, and the
//     short-buffer case is the one a host actually hits.
//   * THE DESCRIPTORS. `clay_multires_memory` GREW in this ABI, and its two new
//     rows sit at the TAIL: a descriptor grows at its tail or every host
//     compiled against the older layout reads the wrong field. A caller
//     declaring the older size must come back with the older fields filled and
//     nothing written past what it declared.
//
// The identity case is the one worth stating twice: an id is not an index, so
// the case reorders the stack and dials a slider through an id taken BEFORE the
// reorder. An implementation keying on index passes every other case here.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
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

struct Fixture {
    clay_mesh* mesh = nullptr;
    clay_multires* surface = nullptr;

    explicit Fixture(int n = 4, uint32_t levels = 2) {
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        plane(n, 1.0f, &positions, &indices);
        REQUIRE(clay_mesh_from_triangles(positions.data(), positions.size() / 3, indices.data(),
                                         indices.size(), &mesh) == CLAY_OK);
        int32_t err = -1;
        REQUIRE(clay_multires_from_mesh(mesh, nullptr, &surface, &err) == CLAY_OK);
        for (uint32_t i = 0; i < levels; ++i)
            REQUIRE(clay_multires_add_level(surface, nullptr, &err) == CLAY_OK);
        REQUIRE(clay_multires_set_sculpt_level(surface, levels) == CLAY_OK);
    }
    ~Fixture() {
        clay_multires_destroy(surface);
        clay_mesh_destroy(mesh);
    }

    uint64_t add(const char* name) {
        uint64_t id = 0;
        int32_t err = -1;
        REQUIRE(clay_multires_add_sculpt_layer(surface, name, &id, &err) == CLAY_OK);
        CHECK(err == CLAY_MULTIRES_OK);
        CHECK(id != CLAY_NO_SCULPT_LAYER);
        return id;
    }
    void write(uint64_t id, uint32_t level, uint32_t vertex, float normal) {
        const float tbn[3] = {0.0f, 0.0f, normal};
        int32_t err = -1;
        REQUIRE(clay_multires_set_sculpt_layer_detail(surface, id, level, vertex, tbn, &err) ==
                CLAY_OK);
    }
    // Every level-2 position, as a host would read them for a redraw.
    std::vector<float> positions(uint32_t level = 2) {
        clay_mesh* level_mesh = nullptr;
        REQUIRE(clay_multires_copy_level_mesh(surface, level, &level_mesh) == CLAY_OK);
        const size_t n = clay_mesh_vertex_count(level_mesh);
        const float* raw = clay_mesh_positions(level_mesh);
        std::vector<float> out(raw, raw + n * 3);
        clay_mesh_destroy(level_mesh);
        return out;
    }
};

clay_mesh_brush_desc draw_brush(float x, float radius, float strength) {
    clay_mesh_brush_desc d{};
    d.struct_size = sizeof(d);
    REQUIRE(clay_mesh_brush_defaults(&d) == CLAY_OK);
    d.verb = CLAY_MESH_BRUSH_DRAW;
    d.center[0] = x;
    d.center[1] = 0.0f;
    d.center[2] = 0.0f;
    d.radius = radius;
    d.strength = strength;
    return d;
}

}  // namespace

TEST_CASE("c sculpt layers: the stack is listed, named and dialled through ids") {
    Fixture f;
    const uint64_t lower = f.add("lower");
    const uint64_t upper = f.add("a much longer pass name");
    CHECK(lower != upper);

    size_t count = 0;
    REQUIRE(clay_multires_sculpt_layer_count(f.surface, &count) == CLAY_OK);
    CHECK(count == 2);
    uint64_t at0 = 0, at1 = 0;
    REQUIRE(clay_multires_sculpt_layer_id_at(f.surface, 0, &at0) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_id_at(f.surface, 1, &at1) == CLAY_OK);
    CHECK(at0 == lower);
    CHECK(at1 == upper);
    CHECK(clay_multires_sculpt_layer_id_at(f.surface, 2, &at0) == CLAY_ERROR_NOT_FOUND);

    clay_sculpt_layer_info info{};
    info.struct_size = sizeof(info);
    REQUIRE(clay_multires_sculpt_layer_info(f.surface, upper, &info) == CLAY_OK);
    CHECK(info.id == upper);
    CHECK(info.index == 1);
    CHECK(info.kind == CLAY_SCULPT_LAYER_SAMPLED);
    CHECK(info.strength == doctest::Approx(1.0f));
    CHECK(info.visible == 1);
    CHECK(info.locked == 0);
    CHECK(info.name_bytes == std::strlen("a much longer pass name") + 1);

    SUBCASE("the name crosses into a buffer the caller owns") {
        size_t needed = 0;
        REQUIRE(clay_multires_sculpt_layer_name(f.surface, upper, nullptr, &needed) == CLAY_OK);
        CHECK(needed == info.name_bytes);

        // A SHORT BUFFER IS REFUSED and told what it needs, rather than filled
        // to its size and left unterminated.
        std::vector<char> tight(needed - 1, '\0');
        size_t tight_size = tight.size();
        CHECK(clay_multires_sculpt_layer_name(f.surface, upper, tight.data(), &tight_size) ==
              CLAY_ERROR_BUFFER_TOO_SMALL);
        CHECK(tight_size == needed);

        std::vector<char> buffer(needed, '\xcc');
        size_t size = buffer.size();
        REQUIRE(clay_multires_sculpt_layer_name(f.surface, upper, buffer.data(), &size) ==
                CLAY_OK);
        CHECK(size == needed);
        CHECK(std::string(buffer.data()) == "a much longer pass name");
    }
    SUBCASE("an unknown id is NOT FOUND rather than a zeroed descriptor") {
        // A zeroed descriptor is indistinguishable from a real layer at
        // strength 0, which is a state a slider reaches.
        clay_sculpt_layer_info missing{};
        missing.struct_size = sizeof(missing);
        CHECK(clay_multires_sculpt_layer_info(f.surface, upper + 1000, &missing) ==
              CLAY_ERROR_NOT_FOUND);
        size_t size = 0;
        CHECK(clay_multires_sculpt_layer_name(f.surface, upper + 1000, nullptr, &size) ==
              CLAY_ERROR_NOT_FOUND);
    }
    SUBCASE("an id still names its own pass after a reorder") {
        // THE CASE AN INDEX-KEYED IMPLEMENTATION PASSES EVERY OTHER TEST AND
        // FAILS. `upper` is taken while it is at index 1 and used after it has
        // been dragged to index 0.
        int32_t err = -1;
        REQUIRE(clay_multires_move_sculpt_layer(f.surface, upper, 0, &err) == CLAY_OK);
        CHECK(err == CLAY_MULTIRES_OK);
        REQUIRE(clay_multires_sculpt_layer_id_at(f.surface, 0, &at0) == CLAY_OK);
        CHECK(at0 == upper);

        REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, upper, 0.25f, &err) ==
                CLAY_OK);
        clay_sculpt_layer_info after{};
        after.struct_size = sizeof(after);
        REQUIRE(clay_multires_sculpt_layer_info(f.surface, upper, &after) == CLAY_OK);
        CHECK(after.index == 0);
        CHECK(after.strength == doctest::Approx(0.25f));
        // ...and the other layer's slider did not move.
        clay_sculpt_layer_info other{};
        other.struct_size = sizeof(other);
        REQUIRE(clay_multires_sculpt_layer_info(f.surface, lower, &other) == CLAY_OK);
        CHECK(other.strength == doctest::Approx(1.0f));
    }
}

TEST_CASE("c sculpt layers: each refusal says which of the three sentences it is") {
    Fixture f;
    const uint64_t id = f.add("pass");
    f.write(id, 2, 40, 0.05f);
    int32_t err = -1;

    SUBCASE("no such layer") {
        err = -1;
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id + 999, 0.5f, &err) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(err == CLAY_MULTIRES_NO_SUCH_SCULPT_LAYER);
        err = -1;
        CHECK(clay_multires_remove_sculpt_layer(f.surface, id + 999, &err) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(err == CLAY_MULTIRES_NO_SUCH_SCULPT_LAYER);
        CHECK(std::strlen(clay_multires_error_text(CLAY_MULTIRES_NO_SUCH_SCULPT_LAYER)) > 0);
    }
    SUBCASE("this layer is locked, and only for what a lock covers") {
        REQUIRE(clay_multires_set_sculpt_layer_locked(f.surface, id, 1, &err) == CLAY_OK);
        const float tbn[3] = {0.0f, 0.0f, 0.1f};
        err = -1;
        CHECK(clay_multires_set_sculpt_layer_detail(f.surface, id, 2, 41, tbn, &err) !=
              CLAY_OK);
        CHECK(err == CLAY_MULTIRES_SCULPT_LAYER_LOCKED);
        float read[3] = {9, 9, 9};
        REQUIRE(clay_multires_sculpt_layer_detail(f.surface, id, 2, 41, read) == CLAY_OK);
        CHECK(read[2] == 0.0f);

        // A lock is a permission on the COEFFICIENTS and not on the channel.
        err = -1;
        CHECK(clay_multires_rename_sculpt_layer(f.surface, id, "finished", &err) == CLAY_OK);
        CHECK(err == CLAY_MULTIRES_OK);
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) == CLAY_OK);
        CHECK(clay_multires_set_sculpt_layer_visible(f.surface, id, 0, &err) == CLAY_OK);
        CHECK(std::strlen(clay_multires_error_text(CLAY_MULTIRES_SCULPT_LAYER_LOCKED)) > 0);
    }
    SUBCASE("finish the stroke first") {
        REQUIRE(clay_multires_hold_sculpt_layer_composition(f.surface, 1) == CLAY_OK);
        err = -1;
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) != CLAY_OK);
        CHECK(err == CLAY_MULTIRES_SCULPT_LAYER_STROKE_OPEN);
        err = -1;
        CHECK(clay_multires_remove_sculpt_layer(f.surface, id, &err) != CLAY_OK);
        CHECK(err == CLAY_MULTIRES_SCULPT_LAYER_STROKE_OPEN);

        // Rename and set-active move no vertex, so neither is held.
        err = -1;
        CHECK(clay_multires_rename_sculpt_layer(f.surface, id, "still fine", &err) == CLAY_OK);
        CHECK(err == CLAY_MULTIRES_OK);
        CHECK(clay_multires_set_active_sculpt_layer(f.surface, id, &err) == CLAY_OK);

        REQUIRE(clay_multires_hold_sculpt_layer_composition(f.surface, 0) == CLAY_OK);
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) == CLAY_OK);
    }
    SUBCASE("and a successful call clears the caller's error slot") {
        err = 12345;
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.75f, &err) == CLAY_OK);
        CHECK(err == CLAY_MULTIRES_OK);
    }
}

TEST_CASE("c sculpt layers: the sliders move the surface and the coefficients stay put") {
    Fixture f;
    const std::vector<float> flat = f.positions();
    const uint64_t id = f.add("pass");
    for (uint32_t v = 30; v < 60; ++v) f.write(id, 2, v, 0.04f);
    const std::vector<float> full = f.positions();
    CHECK(full != flat);

    int32_t err = -1;
    REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) == CLAY_OK);
    const std::vector<float> half = f.positions();
    // Half the offset, and the STORED value untouched: strength is composition
    // rather than a scale on the pen, so raising it back restores the whole
    // pass instead of a scaled-down remnant.
    for (size_t i = 0; i < flat.size(); ++i)
        CHECK(half[i] - flat[i] == doctest::Approx(0.5f * (full[i] - flat[i])).epsilon(1e-4));
    float read[3] = {0, 0, 0};
    REQUIRE(clay_multires_sculpt_layer_detail(f.surface, id, 2, 40, read) == CLAY_OK);
    CHECK(read[2] == doctest::Approx(0.04f));

    SUBCASE("invisible contributes nothing, to the bit") {
        REQUIRE(clay_multires_set_sculpt_layer_visible(f.surface, id, 0, &err) == CLAY_OK);
        CHECK(f.positions() == flat);
    }
    SUBCASE("strength zero contributes nothing, to the bit") {
        REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.0f, &err) == CLAY_OK);
        CHECK(f.positions() == flat);
    }
    SUBCASE("the mask hides where the layer contributes, and 1 releases it") {
        REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, id, 1.0f, &err) == CLAY_OK);
        float weight = 0.0f;
        REQUIRE(clay_multires_sculpt_layer_mask(f.surface, id, 2, 40, &weight) == CLAY_OK);
        // The identity is ONE: a mask the artist never touched must not erase
        // the layer it belongs to.
        CHECK(weight == doctest::Approx(1.0f));
        REQUIRE(clay_multires_set_sculpt_layer_mask(f.surface, id, 2, 40, 0.0f, &err) == CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_mask(f.surface, id, 2, 40, &weight) == CLAY_OK);
        CHECK(weight == doctest::Approx(0.0f));
        const std::vector<float> masked = f.positions();
        CHECK(masked != full);
        REQUIRE(clay_multires_set_sculpt_layer_mask(f.surface, id, 2, 40, 1.0f, &err) == CLAY_OK);
        CHECK(f.positions() == full);
    }
    SUBCASE("the three revisions say which of three things happened") {
        uint64_t m0 = 0, c0 = 0, k0 = 0;
        REQUIRE(clay_multires_sculpt_layer_revision(f.surface, &m0, &c0, &k0) == CLAY_OK);
        REQUIRE(clay_multires_rename_sculpt_layer(f.surface, id, "renamed", &err) == CLAY_OK);
        uint64_t m1 = 0, c1 = 0, k1 = 0;
        REQUIRE(clay_multires_sculpt_layer_revision(f.surface, &m1, &c1, &k1) == CLAY_OK);
        // A RENAME INVALIDATES NOTHING: a host keyed on composition or content
        // does not re-evaluate a model because a layer was renamed.
        CHECK(m1 > m0);
        CHECK(c1 == c0);
        CHECK(k1 == k0);

        REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.3f, &err) == CLAY_OK);
        uint64_t c2 = 0, k2 = 0;
        REQUIRE(clay_multires_sculpt_layer_revision(f.surface, nullptr, &c2, &k2) == CLAY_OK);
        CHECK(c2 > c1);
        CHECK(k2 == k1);

        f.write(id, 2, 41, 0.01f);
        uint64_t k3 = 0;
        REQUIRE(clay_multires_sculpt_layer_revision(f.surface, nullptr, nullptr, &k3) == CLAY_OK);
        CHECK(k3 > k2);
    }
    SUBCASE("merge down leaves the surface where it was") {
        const uint64_t upper = f.add("upper");
        for (uint32_t v = 30; v < 60; ++v) f.write(upper, 2, v, 0.02f);
        REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, upper, 0.6f, &err) == CLAY_OK);
        const std::vector<float> before = f.positions();
        REQUIRE(clay_multires_merge_sculpt_layer_down(f.surface, upper, &err) == CLAY_OK);
        size_t count = 0;
        REQUIRE(clay_multires_sculpt_layer_count(f.surface, &count) == CLAY_OK);
        CHECK(count == 1);
        const std::vector<float> after = f.positions();
        for (size_t i = 0; i < before.size(); ++i)
            CHECK(after[i] == doctest::Approx(before[i]).epsilon(1e-5));
    }
    SUBCASE("bake to base moves the pass into the form and leaves the surface alone") {
        uint64_t base_before = 0;
        REQUIRE(clay_multires_detail_checksum(f.surface, &base_before) == CLAY_OK);
        const std::vector<float> before = f.positions();
        REQUIRE(clay_multires_bake_sculpt_layer_to_base(f.surface, id, &err) == CLAY_OK);
        size_t count = 0;
        REQUIRE(clay_multires_sculpt_layer_count(f.surface, &count) == CLAY_OK);
        CHECK(count == 0);
        uint64_t base_after = 0;
        REQUIRE(clay_multires_detail_checksum(f.surface, &base_after) == CLAY_OK);
        CHECK(base_after != base_before);
        const std::vector<float> after = f.positions();
        for (size_t i = 0; i < before.size(); ++i)
            CHECK(after[i] == doctest::Approx(before[i]).epsilon(1e-5));
    }
}

TEST_CASE("c sculpt layers: the memory descriptor grew at its tail and nowhere else") {
    Fixture f;
    const uint64_t id = f.add("pass");
    for (uint32_t v = 0; v < 200; ++v) f.write(id, 2, v, 0.01f);
    f.positions();  // materialize the composed field

    clay_multires_memory memory{};
    memory.struct_size = sizeof(memory);
    REQUIRE(clay_multires_memory_get(f.surface, &memory) == CLAY_OK);
    CHECK(memory.sculpt_layers > 0);
    CHECK(memory.composed > 0);
    // The stack's bytes are AUTHORITATIVE and reported apart from the base
    // detail; the composed field is derived and droppable.
    CHECK(memory.authoritative >= memory.sculpt_layers + memory.detail);
    CHECK(memory.rebuildable >= memory.composed);
    CHECK(memory.total == memory.authoritative + memory.rebuildable);

    // A HOST COMPILED AGAINST THE OLDER LAYOUT: it declares the size it knew
    // and must come back with the fields it knew filled, and nothing written
    // past the size it declared.
    struct OldLayout {
        clay_multires_memory fields;
        uint64_t canary;
    };
    const uint32_t old_size =
        static_cast<uint32_t>(offsetof(clay_multires_memory, sculpt_layers));
    OldLayout probe{};
    probe.canary = 0xfeedfacecafebeefull;
    std::memset(&probe.fields, 0, sizeof(probe.fields));
    probe.fields.struct_size = old_size;
    REQUIRE(clay_multires_memory_get(f.surface, &probe.fields) == CLAY_OK);
    CHECK(probe.fields.authoritative == memory.authoritative);
    CHECK(probe.fields.total == memory.total);
    CHECK(probe.fields.sculpt_layers == 0);
    CHECK(probe.fields.composed == 0);
    CHECK(probe.canary == 0xfeedfacecafebeefull);
}

TEST_CASE("c sculpt layers: the stack survives a save and a load, ids and all") {
    Fixture f;
    const uint64_t first = f.add("first");
    const uint64_t second = f.add("second");
    f.write(first, 2, 40, 0.05f);
    f.write(second, 2, 41, -0.02f);
    int32_t err = -1;
    REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, first, 0.5f, &err) == CLAY_OK);
    // A reorder BEFORE the save, so the case says something about ids rather
    // than about positions in a list.
    REQUIRE(clay_multires_move_sculpt_layer(f.surface, second, 0, &err) == CLAY_OK);
    uint64_t checksum = 0;
    REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &checksum) == CLAY_OK);

    size_t size = 0;
    REQUIRE(clay_multires_serialize(f.surface, nullptr, &size) == CLAY_OK);
    std::vector<uint8_t> bytes(size);
    REQUIRE(clay_multires_serialize(f.surface, bytes.data(), &size) == CLAY_OK);

    clay_multires* loaded = nullptr;
    REQUIRE(clay_multires_deserialize(bytes.data(), bytes.size(), &loaded) == CLAY_OK);
    size_t count = 0;
    REQUIRE(clay_multires_sculpt_layer_count(loaded, &count) == CLAY_OK);
    CHECK(count == 2);
    uint64_t at0 = 0;
    REQUIRE(clay_multires_sculpt_layer_id_at(loaded, 0, &at0) == CLAY_OK);
    CHECK(at0 == second);
    clay_sculpt_layer_info info{};
    info.struct_size = sizeof(info);
    REQUIRE(clay_multires_sculpt_layer_info(loaded, first, &info) == CLAY_OK);
    CHECK(info.strength == doctest::Approx(0.5f));
    uint64_t loaded_checksum = 0;
    REQUIRE(clay_multires_sculpt_layer_checksum(loaded, &loaded_checksum) == CLAY_OK);
    CHECK(loaded_checksum == checksum);
    clay_multires_destroy(loaded);

    SUBCASE("a truncated or hostile stream is refused rather than half loaded") {
        clay_multires* out = nullptr;
        CHECK(clay_multires_deserialize(bytes.data(), bytes.size() - 16, &out) != CLAY_OK);
        CHECK(out == nullptr);
        std::vector<uint8_t> hostile = bytes;
        hostile[4] ^= 0xffu;
        CHECK(clay_multires_deserialize(hostile.data(), hostile.size(), &out) != CLAY_OK);
        CHECK(out == nullptr);
    }
}

TEST_CASE("c sculpt layers: a stroke transaction begins, stamps, commits and cancels") {
    Fixture f;
    const uint64_t id = f.add("pass");
    int32_t err = -1;
    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, id, &err) == CLAY_OK);

    clay_multires_sculpt_layer_stroke* stroke = nullptr;
    REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
    REQUIRE(stroke != nullptr);

    uint64_t layer_before = 0;
    REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &layer_before) == CLAY_OK);
    uint64_t base_before = 0;
    REQUIRE(clay_multires_detail_checksum(f.surface, &base_before) == CLAY_OK);

    err = -1;
    REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, &err) == CLAY_OK);
    CHECK(err == CLAY_MULTIRES_OK);
    uint64_t target = 0;
    REQUIRE(clay_multires_sculpt_layer_stroke_target_layer(stroke, &target) == CLAY_OK);
    CHECK(target == id);

    // A slider is refused for the length of the gesture, with the sentence a
    // host shows.
    err = -1;
    CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) != CLAY_OK);
    CHECK(err == CLAY_MULTIRES_SCULPT_LAYER_STROKE_OPEN);

    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    for (int i = 0; i < 5; ++i) {
        const clay_mesh_brush_desc brush = draw_brush(-0.2f + 0.1f * static_cast<float>(i),
                                                      0.35f, 0.4f);
        REQUIRE(clay_multires_sculpt_layer_stroke_stamp(stroke, &brush, nullptr, &report) ==
                CLAY_OK);
    }
    size_t stamps = 0, entries = 0;
    REQUIRE(clay_multires_sculpt_layer_stroke_stamps(stroke, &stamps) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_stroke_record_size(stroke, &entries) == CLAY_OK);
    CHECK(stamps == 5);
    CHECK(entries > 0);
    uint64_t layer_mid = 0;
    REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &layer_mid) == CLAY_OK);
    CHECK(layer_mid != layer_before);
    // The FORM is untouched: with an active layer the gesture lands in the
    // channel and nowhere else.
    uint64_t base_mid = 0;
    REQUIRE(clay_multires_detail_checksum(f.surface, &base_mid) == CLAY_OK);
    CHECK(base_mid == base_before);

    SUBCASE("cancel puts the recorded values back and releases the hold") {
        REQUIRE(clay_multires_sculpt_layer_stroke_cancel(stroke) == CLAY_OK);
        uint64_t after = 0;
        REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &after) == CLAY_OK);
        CHECK(after == layer_before);
        err = -1;
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) == CLAY_OK);
    }
    SUBCASE("commit reports how many entries the record held") {
        size_t committed = 0;
        REQUIRE(clay_multires_sculpt_layer_stroke_commit(stroke, &committed) == CLAY_OK);
        CHECK(committed == entries);
        uint64_t after = 0;
        REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &after) == CLAY_OK);
        CHECK(after == layer_mid);
        err = -1;
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) == CLAY_OK);
        // A second commit has nothing to close.
        CHECK(clay_multires_sculpt_layer_stroke_commit(stroke, nullptr) != CLAY_OK);
    }
    clay_multires_sculpt_layer_stroke_destroy(stroke);
}

TEST_CASE("c sculpt layers: begin refuses what it cannot honour, with the reason") {
    Fixture f;
    const uint64_t id = f.add("pass");
    int32_t err = -1;

    SUBCASE("detail with no active layer") {
        REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, CLAY_NO_SCULPT_LAYER, &err) ==
                CLAY_OK);
        clay_multires_sculpt_layer_stroke* stroke = nullptr;
        REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_stroke_set_write_domain(
                    stroke, CLAY_MULTIRES_WRITE_DETAIL) == CLAY_OK);
        err = -1;
        CHECK(clay_multires_sculpt_layer_stroke_begin(stroke, &err) != CLAY_OK);
        // Nothing was held, so a host that shows the refusal can carry on.
        CHECK(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.5f, &err) == CLAY_OK);
        clay_multires_sculpt_layer_stroke_destroy(stroke);
    }
    SUBCASE("a locked target") {
        REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, id, &err) == CLAY_OK);
        REQUIRE(clay_multires_set_sculpt_layer_locked(f.surface, id, 1, &err) == CLAY_OK);
        clay_multires_sculpt_layer_stroke* stroke = nullptr;
        REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
        err = -1;
        CHECK(clay_multires_sculpt_layer_stroke_begin(stroke, &err) != CLAY_OK);
        CHECK(err == CLAY_MULTIRES_SCULPT_LAYER_LOCKED);
        clay_multires_sculpt_layer_stroke_destroy(stroke);
    }
    SUBCASE("geometry writes the form under the passes") {
        REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, id, &err) == CLAY_OK);
        uint64_t layer_before = 0;
        REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &layer_before) == CLAY_OK);
        uint64_t base_before = 0;
        REQUIRE(clay_multires_detail_checksum(f.surface, &base_before) == CLAY_OK);

        clay_multires_sculpt_layer_stroke* stroke = nullptr;
        REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_stroke_set_write_domain(
                    stroke, CLAY_MULTIRES_WRITE_GEOMETRY) == CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, &err) == CLAY_OK);
        uint64_t target = 1;
        REQUIRE(clay_multires_sculpt_layer_stroke_target_layer(stroke, &target) == CLAY_OK);
        CHECK(target == CLAY_NO_SCULPT_LAYER);
        const clay_mesh_brush_desc brush = draw_brush(0.0f, 0.4f, 0.5f);
        REQUIRE(clay_multires_sculpt_layer_stroke_stamp(stroke, &brush, nullptr, nullptr) ==
                CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_stroke_commit(stroke, nullptr) == CLAY_OK);
        uint64_t base_after = 0, layer_after = 0;
        REQUIRE(clay_multires_detail_checksum(f.surface, &base_after) == CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_checksum(f.surface, &layer_after) == CLAY_OK);
        CHECK(base_after != base_before);
        CHECK(layer_after == layer_before);
        clay_multires_sculpt_layer_stroke_destroy(stroke);
    }
}

TEST_CASE("c sculpt layers: a detail stamp is borrowed, planar, and reports its resolution") {
    Fixture f;
    const uint64_t id = f.add("map");
    int32_t err = -1;
    REQUIRE(clay_multires_set_active_sculpt_layer(f.surface, id, &err) == CLAY_OK);

    clay_multires_sculpt_layer_stroke* stroke = nullptr;
    REQUIRE(clay_multires_sculpt_layer_stroke_create(f.surface, &stroke) == CLAY_OK);
    REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, &err) == CLAY_OK);

    std::vector<float> image(64 * 64, 1.0f);
    clay_detail_stamp_desc stamp{};
    stamp.struct_size = sizeof(stamp);
    stamp.mode = CLAY_DETAIL_STAMP_HEIGHT;
    stamp.image = image.data();
    stamp.width = 64;
    stamp.height = 64;
    stamp.amplitude = 0.05f;
    stamp.bias = 0.0f;
    stamp.center[0] = 0.0f;
    stamp.center[1] = 0.0f;
    stamp.center[2] = 0.0f;
    stamp.extent = 0.8f;
    const clay_mesh_brush_desc brush = draw_brush(0.0f, 0.4f, 1.0f);

    clay_detail_stamp_report stamp_report{};
    stamp_report.struct_size = sizeof(stamp_report);
    clay_multires_stamp_report report{};
    report.struct_size = sizeof(report);
    REQUIRE(clay_multires_sculpt_layer_stroke_stamp_detail(stroke, &stamp, &brush, nullptr,
                                                           &stamp_report, &report) == CLAY_OK);
    CHECK(report.moved_vertices > 0);
    CHECK(stamp_report.sample_size == doctest::Approx(0.8f / 64.0f));
    CHECK(stamp_report.vertex_spacing > 0.0f);
    CHECK(stamp_report.oversampling ==
          doctest::Approx(stamp_report.vertex_spacing / stamp_report.sample_size));
    REQUIRE(clay_multires_sculpt_layer_stroke_commit(stroke, nullptr) == CLAY_OK);

    // A HEIGHT map moves the third coefficient and only the third: it is a
    // displacement along the vertex's own normal, never a world axis. Swept
    // over the level rather than sampled at a vertex index a subdivision
    // happens to hand out — which vertex sits under the square is the
    // subdivision's business, and "some did, none tangentially" is the claim.
    uint64_t level_vertices = 0, level_faces = 0;
    REQUIRE(clay_multires_level_counts(f.surface, 2, &level_vertices, &level_faces) == CLAY_OK);
    size_t lifted = 0;
    for (uint32_t v = 0; v < level_vertices; ++v) {
        float tbn[3] = {9, 9, 9};
        REQUIRE(clay_multires_sculpt_layer_detail(f.surface, id, 2, v, tbn) == CLAY_OK);
        CHECK(tbn[0] == 0.0f);
        CHECK(tbn[1] == 0.0f);
        if (tbn[2] > 0.0f) ++lifted;
    }
    CHECK(lifted > 0);

    SUBCASE("a scalar weight is refused rather than served through a second door") {
        REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, &err) == CLAY_OK);
        clay_detail_stamp_desc weight = stamp;
        weight.mode = CLAY_DETAIL_STAMP_WEIGHT;
        clay_multires_stamp_report out{};
        out.struct_size = sizeof(out);
        clay_multires_sculpt_layer_stroke_stamp_detail(stroke, &weight, &brush, nullptr, nullptr,
                                                       &out);
        CHECK(out.moved_vertices == 0);
        REQUIRE(clay_multires_sculpt_layer_stroke_cancel(stroke) == CLAY_OK);
    }
    SUBCASE("a null image is refused rather than read") {
        REQUIRE(clay_multires_sculpt_layer_stroke_begin(stroke, &err) == CLAY_OK);
        clay_detail_stamp_desc empty = stamp;
        empty.image = nullptr;
        CHECK(clay_multires_sculpt_layer_stroke_stamp_detail(stroke, &empty, &brush, nullptr,
                                                             nullptr, nullptr) != CLAY_OK);
        REQUIRE(clay_multires_sculpt_layer_stroke_cancel(stroke) == CLAY_OK);
    }
    clay_multires_sculpt_layer_stroke_destroy(stroke);
}

TEST_CASE("c sculpt layers: the stats are what makes both scale claims measurements") {
    // A correct implementation and a quadratic one produce the same surface, so
    // there is no other way to see either claim from outside.
    Fixture f(4, 4);
    const uint64_t id = f.add("local");
    for (uint32_t v = 10; v < 40; ++v) f.write(id, 4, v, 0.01f);
    f.positions(4);

    REQUIRE(clay_multires_reset_sculpt_layer_stats(f.surface) == CLAY_OK);
    int32_t err = -1;
    REQUIRE(clay_multires_set_sculpt_layer_strength(f.surface, id, 0.25f, &err) == CLAY_OK);
    f.positions(4);

    clay_sculpt_layer_stats stats{};
    stats.struct_size = sizeof(stats);
    REQUIRE(clay_multires_sculpt_layer_stats(f.surface, &stats) == CLAY_OK);
    // THE GATE: the blocks the LAYER has allocated, not the blocks the level
    // holds.
    CHECK(stats.blocks_recomposed == 1);
    CHECK(stats.compositions >= 1);

    SUBCASE("compacting releases what a cancelled gesture left behind") {
        for (uint32_t v = 10; v < 40; ++v) f.write(id, 4, v, 0.0f);
        clay_multires_memory before{};
        before.struct_size = sizeof(before);
        REQUIRE(clay_multires_memory_get(f.surface, &before) == CLAY_OK);
        REQUIRE(clay_multires_compact_sculpt_layers(f.surface) == CLAY_OK);
        clay_multires_memory after{};
        after.struct_size = sizeof(after);
        REQUIRE(clay_multires_memory_get(f.surface, &after) == CLAY_OK);
        CHECK(after.sculpt_layers < before.sculpt_layers);
    }
}
