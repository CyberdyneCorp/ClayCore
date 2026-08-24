#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// The layer's RADIAL symmetry (#256): count copies of every participating item
// about the layer-local axis, as a MODE rather than the per-item modifier
// clay_item_set_repeat_radial already provided.
//
// The probe is rotational invariance measured rather than asserted
// structurally: an off-axis lump on a sphere, sampled at a ring of points, must
// read the same at every multiple of the sector angle and must NOT read the
// same at a half-sector — otherwise a test would pass on a field that was
// simply smooth.

namespace {

constexpr float kRadius = 1.0f;
constexpr float kLumpR = 0.35f;
constexpr float kLumpX = 0.9f;  // off-axis, so rotation about Y moves it

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "body", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_item_desc sphere_desc(float radius) {
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_SPHERE;
    desc.params[0] = radius;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    return desc;
}

void add_base(Doc& doc) {
    clay_item_desc base = sphere_desc(kRadius);
    REQUIRE(clay_add_item(doc.d, doc.layer, &base, nullptr) == CLAY_OK);
}

// A lump off the Y axis. `mirror` is the participation flag the radial mode
// shares with the layer mirror: 0 follows the layer, -1 opts out.
void add_lump(Doc& doc, int32_t mirror) {
    clay_item_desc lump = sphere_desc(kLumpR);
    lump.position[0] = kLumpX;
    lump.mirror = mirror;
    REQUIRE(clay_add_item(doc.d, doc.layer, &lump, nullptr) == CLAY_OK);
}

float eval_at(const clay_document* doc, float x, float y, float z) {
    const float p[3] = {x, y, z};
    float d = 0.0f;
    REQUIRE(clay_eval_points(doc, "cpu", p, 1, &d, nullptr) == CLAY_OK);
    return d;
}

// The lump's own position, rotated about Y by `radians`.
float eval_rotated(const clay_document* doc, float radians) {
    const float x = kLumpX * std::cos(radians);
    const float z = -kLumpX * std::sin(radians);
    return eval_at(doc, x, 0.0f, z);
}

}  // namespace

TEST_CASE("radial symmetry arrays an item about the layer axis") {
    Doc doc;
    add_base(doc);
    add_lump(doc, 0);

    const float at_lump = eval_rotated(doc.d, 0.0f);
    // Before the mode, the far sectors are bare sphere and the lump is not.
    CHECK(eval_rotated(doc.d, 6.2831853f / 6.0f) > at_lump + 0.1f);

    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 6, 0.0f) == CLAY_OK);

    // Every sector reads what the lump's own sector reads.
    for (int k = 1; k < 6; ++k) {
        const float angle = 6.2831853f * static_cast<float>(k) / 6.0f;
        CHECK(eval_rotated(doc.d, angle) == doctest::Approx(at_lump).epsilon(0.001));
    }
    // ...and a HALF sector does not, or the check above would pass on any
    // smooth field.
    CHECK(eval_rotated(doc.d, 6.2831853f / 12.0f) > at_lump + 0.05f);
}

TEST_CASE("clearing the radial count restores the field") {
    Doc doc;
    add_base(doc);
    add_lump(doc, 0);
    const float before = eval_rotated(doc.d, 6.2831853f / 4.0f);

    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 4, 0.0f) == CLAY_OK);
    CHECK(eval_rotated(doc.d, 6.2831853f / 4.0f) != doctest::Approx(before).epsilon(0.001));

    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 0, 0.0f) == CLAY_OK);
    CHECK(eval_rotated(doc.d, 6.2831853f / 4.0f) == doctest::Approx(before).epsilon(0.0001));
}

TEST_CASE("an item excluded from the layer symmetry does not array") {
    Doc doc;
    add_base(doc);
    add_lump(doc, -1);  // the asymmetric detail
    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 6, 0.0f) == CLAY_OK);

    const float at_lump = eval_rotated(doc.d, 0.0f);
    // The far sectors stay bare: one flag excludes an item from the layer's
    // symmetry, and it is the same flag the mirror uses.
    CHECK(eval_rotated(doc.d, 6.2831853f / 6.0f) > at_lump + 0.1f);
}

TEST_CASE("radial symmetry is undoable, and restores count, axis and blend") {
    Doc doc;
    add_base(doc);
    add_lump(doc, 0);
    REQUIRE(clay_document_enable_undo(doc.d) == CLAY_OK);

    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 6, 0.0f) == CLAY_OK);
    const float arrayed = eval_rotated(doc.d, 6.2831853f / 6.0f);
    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 3, 0.25f) == CLAY_OK);

    int32_t undone = 0;
    REQUIRE(clay_document_undo(doc.d, &undone) == CLAY_OK);
    CHECK(undone == 1);
    // Back to the six-fold array, not to no array at all.
    CHECK(eval_rotated(doc.d, 6.2831853f / 6.0f) == doctest::Approx(arrayed).epsilon(0.001));
}

TEST_CASE("an out-of-range axis or a negative blend is refused, not clamped") {
    Doc doc;
    add_base(doc);
    CHECK(clay_set_layer_radial(doc.d, doc.layer, 3, 6, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_set_layer_radial(doc.d, doc.layer, -1, 6, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_set_layer_radial(doc.d, doc.layer, 1, 6, -0.1f) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("radial symmetry survives a document round-trip") {
    Doc doc;
    add_base(doc);
    add_lump(doc, 0);
    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 5, 0.1f) == CLAY_OK);
    const float arrayed = eval_rotated(doc.d, 6.2831853f * 2.0f / 5.0f);

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc.d, &blob) == CLAY_OK);
    REQUIRE(blob != nullptr);
    const uint8_t* data = clay_blob_data(blob);
    const size_t size = clay_blob_size(blob);
    REQUIRE(data != nullptr);
    REQUIRE(size > 0);

    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(data, size, &back) == CLAY_OK);
    CHECK(eval_rotated(back, 6.2831853f * 2.0f / 5.0f) ==
          doctest::Approx(arrayed).epsilon(0.0001));
    clay_document_destroy(back);
    clay_blob_destroy(blob);
}

TEST_CASE("radial and mirror compose additively, not as their product") {
    Doc doc;
    add_base(doc);
    add_lump(doc, 0);
    // Three sectors about Y, plus a reflection through x = 0. The additive set
    // is 3 rotations + 1 reflection = 4 lumps; the product would be 6.
    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 3, 0.0f) == CLAY_OK);
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);

    const float at_lump = eval_rotated(doc.d, 0.0f);
    // The reflection of the lump is at -x, which is 180 degrees — NOT one of
    // the three sector angles (0, 120, 240), so it exists only because the
    // mirror put it there.
    CHECK(eval_rotated(doc.d, 3.14159265f) == doctest::Approx(at_lump).epsilon(0.001));
    // The product copies — a rotated reflection at 60 and 300 degrees — are
    // deliberately NOT emitted.
    CHECK(eval_rotated(doc.d, 6.2831853f / 6.0f) > at_lump + 0.05f);
}

TEST_CASE("a stroke on a radial layer arrays without touching its resolved nodes") {
    // This is the property that makes the layer mode a SCULPTING mode rather
    // than the per-item modifier clay_item_set_repeat_radial already was: the
    // caller sets the mode on the layer and applies one stroke, and never
    // reaches into the nodes the stroke resolved into.
    Doc doc;
    add_base(doc);
    REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 4, 0.0f) == CLAY_OK);

    float radius = 0.22f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_op(item, CLAY_OP_RELIEF) == CLAY_OK);
    REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, 0.08f) == CLAY_OK);
    clay_stroke_preset preset;
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_stroke_preset_defaults(&preset) == CLAY_OK);
    preset.radius = 0.2f;
    // One dab on the sphere's +x pole.
    const float sample[5] = {kRadius, 0.0f, 0.0f, 1.0f, 0.0f};
    size_t count = 0;
    REQUIRE(clay_layer_apply_stroke(doc.d, doc.layer, sample, 1, &preset, item, nullptr, nullptr,
                                    &count) == CLAY_OK);
    clay_item_destroy(item);
    REQUIRE(count == 1);

    // The dab is at angle 0 on the surface; a 4-fold array puts the same
    // deposit at 90, 180 and 270 degrees.
    const float at_dab = eval_at(doc.d, kRadius, 0.0f, 0.0f);
    for (int k = 1; k < 4; ++k) {
        const float angle = 6.2831853f * static_cast<float>(k) / 4.0f;
        const float x = kRadius * std::cos(angle);
        const float z = -kRadius * std::sin(angle);
        CHECK(eval_at(doc.d, x, 0.0f, z) == doctest::Approx(at_dab).epsilon(0.002));
    }
    // A half-sector away is not the same, so the check above is not passing on
    // a uniformly inflated sphere.
    const float half = 6.2831853f / 8.0f;
    CHECK(eval_at(doc.d, kRadius * std::cos(half), 0.0f, -kRadius * std::sin(half)) >
          at_dab + 0.01f);
}

TEST_CASE("a document written before the field loads with radial off") {
    // The writer gates the three fields on minor 12 and the reader consumes
    // them only at 12 — the pairing the armature minor-7 test already guards
    // for its own fields, and which a one-sided gate here segfaulted.
    Doc doc;
    add_base(doc);
    add_lump(doc, 0);
    const float bare = eval_rotated(doc.d, 6.2831853f / 3.0f);

    clay_blob* blob = nullptr;
    REQUIRE(clay_document_save_memory(doc.d, &blob) == CLAY_OK);
    const uint8_t* data = clay_blob_data(blob);
    const size_t size = clay_blob_size(blob);
    clay_document* back = nullptr;
    REQUIRE(clay_document_load_memory(data, size, &back) == CLAY_OK);
    // A document saved with the mode off reads back with it off, and evaluates
    // to the un-arrayed field rather than to anything the defaults invented.
    CHECK(eval_rotated(back, 6.2831853f / 3.0f) == doctest::Approx(bare).epsilon(0.0001));
    clay_document_destroy(back);
    clay_blob_destroy(blob);
}
