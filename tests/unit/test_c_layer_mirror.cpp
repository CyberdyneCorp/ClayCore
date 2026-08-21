#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// The layer mirror, held to what a sculpting host expects of it (issue #60):
// clay_set_layer_mirror alone mirrors the layer — placed items and strokes,
// through the document raycast AND the brick cache raycast, whether the
// mirror was set before or after the items were added. Until this suite
// existed the mirror was an opt-in whose default excluded every item, so the
// call returned CLAY_OK and mirrored nothing.
//
// The probe is the issue's own: a unit sphere at the origin, a lump at
// +(0.5, 0.3, 0.8124) — a point ON the sphere, so both sides read 1.0 until
// something is deposited — mirrored about x = 0, and the distance from the
// origin to the surface measured along the probe direction on each side.

namespace {

constexpr float kP[3] = {0.5f, 0.3f, 0.8124f};

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

// The lump of the issue: radius 0.25, CLAY_OP_ADD, a ZEROED mirror field —
// the host that only called clay_set_layer_mirror.
void add_lump(Doc& doc, int32_t mirror) {
    clay_item_desc lump = sphere_desc(0.25f);
    lump.position[0] = kP[0];
    lump.position[1] = kP[1];
    lump.position[2] = kP[2];
    lump.mirror = mirror;
    REQUIRE(clay_add_item(doc.d, doc.layer, &lump, nullptr) == CLAY_OK);
}

// Surface distance from the origin along the probe direction, x sign chosen
// by `side` (+1 near the lump, -1 on the mirrored side). -1 means no hit.
float probe(const clay_document* doc, float side) {
    const float len = std::sqrt(kP[0] * kP[0] + kP[1] * kP[1] + kP[2] * kP[2]);
    const float dir[3] = {side * kP[0] / len, kP[1] / len, kP[2] / len};
    const float origin[3] = {dir[0] * 3.0f, dir[1] * 3.0f, dir[2] * 3.0f};
    const float inward[3] = {-dir[0], -dir[1], -dir[2]};
    int32_t hit = 0;
    float t = 0.0f;
    REQUIRE(clay_raycast(doc, origin, inward, &hit, &t, nullptr, nullptr) == CLAY_OK);
    if (!hit) return -1.0f;
    return 3.0f - t;
}

// The same probe against a brick cache filled from scratch over the model.
float probe_cache(const clay_document* doc, float side) {
    clay_brick_config cfg;
    cfg.struct_size = sizeof(cfg);
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);
    const float lo[3] = {-2.0f, -2.0f, -2.0f}, hi[3] = {2.0f, 2.0f, 2.0f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    const std::size_t samples = static_cast<std::size_t>(cfg.dim) * cfg.dim * cfg.dim;
    const std::size_t chunk = 64;
    std::vector<clay_brick_request> reqs(chunk);
    std::vector<float> values(chunk * samples);
    std::vector<int32_t> results(chunk);
    for (;;) {
        std::size_t count = chunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                               count * samples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                        count * samples, nullptr, 0, results.data(),
                                        &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }

    const float len = std::sqrt(kP[0] * kP[0] + kP[1] * kP[1] + kP[2] * kP[2]);
    const float dir[3] = {side * kP[0] / len, kP[1] / len, kP[2] / len};
    const float origin[3] = {dir[0] * 3.0f, dir[1] * 3.0f, dir[2] * 3.0f};
    const float inward[3] = {-dir[0], -dir[1], -dir[2]};
    int32_t hit = 0;
    float t = 0.0f;
    float out = -1.0f;
    REQUIRE(clay_brick_cache_raycast(cache, origin, inward, &hit, &t, nullptr, nullptr) ==
            CLAY_OK);
    if (hit) out = 3.0f - t;
    clay_brick_cache_destroy(cache);
    return out;
}

}  // namespace

TEST_CASE("the layer mirror mirrors a placed item by default, in both raycast paths") {
    Doc doc;
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);
    clay_item_desc sphere = sphere_desc(1.0f);
    REQUIRE(clay_add_item(doc.d, doc.layer, &sphere, nullptr) == CLAY_OK);
    add_lump(doc, 0);

    const float near_side = probe(doc.d, 1.0f);
    const float far_side = probe(doc.d, -1.0f);
    // The issue measured 1.2500 near and exactly 1.0000 (untouched sphere) far.
    CHECK(near_side == doctest::Approx(1.25f).epsilon(0.01));
    CHECK(far_side == doctest::Approx(near_side).epsilon(0.005));

    // The brick cache agrees with the document, on both sides. Its surface is
    // a trilinear reconstruction over 0.05-unit voxels, so the tolerance is
    // the lattice's, not the raycaster's.
    const float cache_near = probe_cache(doc.d, 1.0f);
    const float cache_far = probe_cache(doc.d, -1.0f);
    CHECK(cache_near == doctest::Approx(near_side).epsilon(0.01));
    CHECK(cache_far == doctest::Approx(cache_near).epsilon(0.005));
}

TEST_CASE("setting the mirror after the items were added mirrors them all the same") {
    Doc doc;
    clay_item_desc sphere = sphere_desc(1.0f);
    REQUIRE(clay_add_item(doc.d, doc.layer, &sphere, nullptr) == CLAY_OK);
    add_lump(doc, 0);
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);

    const float near_side = probe(doc.d, 1.0f);
    const float far_side = probe(doc.d, -1.0f);
    CHECK(near_side == doctest::Approx(1.25f).epsilon(0.01));
    CHECK(far_side == doctest::Approx(near_side).epsilon(0.005));

    // and turning it back off restores the asymmetric field
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 0, 0, 0, 0.0f) == CLAY_OK);
    CHECK(probe(doc.d, -1.0f) == doctest::Approx(1.0f).epsilon(0.005));
    CHECK(probe(doc.d, 1.0f) == doctest::Approx(1.25f).epsilon(0.01));
}

TEST_CASE("a stroke on a mirrored layer lands on both sides, in both raycast paths") {
    Doc doc;
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);
    clay_item_desc sphere = sphere_desc(1.0f);
    REQUIRE(clay_add_item(doc.d, doc.layer, &sphere, nullptr) == CLAY_OK);

    // The issue's relief stamp: radius 0.18 at the lump's point, which lies on
    // the unit sphere. blend.k is the amplitude and rounding the falloff
    // width — a relief that drops rounding declares its amplitude over ~1e-6
    // and nothing can march it (docs/07, §5).
    float radius = 0.18f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(item != nullptr);
    REQUIRE(clay_item_set_op(item, CLAY_OP_RELIEF) == CLAY_OK);
    REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, 0.08f) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(item, 0.09f) == CLAY_OK);
    clay_stroke_preset preset;
    preset.struct_size = sizeof(preset);
    REQUIRE(clay_stroke_preset_defaults(&preset) == CLAY_OK);
    preset.radius = 0.18f;
    const float sample[5] = {kP[0], kP[1], kP[2], 1.0f, 0.0f};
    size_t count = 0;
    REQUIRE(clay_layer_apply_stroke(doc.d, doc.layer, sample, 1, &preset, item, nullptr,
                                    nullptr, &count) == CLAY_OK);
    clay_item_destroy(item);
    REQUIRE(count == 1);

    const float near_side = probe(doc.d, 1.0f);
    const float far_side = probe(doc.d, -1.0f);
    // The stamp deposits a measurable bulge, and the mirrored side carries
    // the SAME bulge — not the 30x-smaller spillover the issue measured.
    CHECK(near_side > 1.02f);
    CHECK(far_side == doctest::Approx(near_side).epsilon(0.005));

    const float cache_near = probe_cache(doc.d, 1.0f);
    const float cache_far = probe_cache(doc.d, -1.0f);
    CHECK(cache_near > 1.02f);
    CHECK(cache_far == doctest::Approx(cache_near).epsilon(0.005));
}

TEST_CASE("mirror = -1 keeps an item out of the layer's mirror") {
    Doc doc;
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);
    clay_item_desc sphere = sphere_desc(1.0f);
    REQUIRE(clay_add_item(doc.d, doc.layer, &sphere, nullptr) == CLAY_OK);
    add_lump(doc, -1);

    CHECK(probe(doc.d, 1.0f) == doctest::Approx(1.25f).epsilon(0.01));
    // the excluded lump leaves the far side the untouched unit sphere
    CHECK(probe(doc.d, -1.0f) == doctest::Approx(1.0f).epsilon(0.005));
}

TEST_CASE("a layer with no mirror axes is untouched by the participation default") {
    Doc doc;
    clay_item_desc sphere = sphere_desc(1.0f);
    REQUIRE(clay_add_item(doc.d, doc.layer, &sphere, nullptr) == CLAY_OK);
    add_lump(doc, 0);

    CHECK(probe(doc.d, 1.0f) == doctest::Approx(1.25f).epsilon(0.01));
    CHECK(probe(doc.d, -1.0f) == doctest::Approx(1.0f).epsilon(0.005));

    // Not just the same geometry: the same TAPE, byte for byte, whatever the
    // items' participation flags. Participation must cost nothing until a
    // mirror axis exists — stroke latency rides on the tape's length.
    Doc excluded;
    REQUIRE(clay_add_item(excluded.d, excluded.layer, &sphere, nullptr) == CLAY_OK);
    add_lump(excluded, -1);

    clay_tape* a = nullptr;
    clay_tape* b = nullptr;
    REQUIRE(clay_tape_export(doc.d, nullptr, nullptr, &a) == CLAY_OK);
    REQUIRE(clay_tape_export(excluded.d, nullptr, nullptr, &b) == CLAY_OK);
    std::size_t na = 0, nb = 0;
    const clay_tape_instr* ia = clay_tape_instrs(a, &na);
    const clay_tape_instr* ib = clay_tape_instrs(b, &nb);
    REQUIRE(na == nb);
    CHECK(std::memcmp(ia, ib, na * sizeof(clay_tape_instr)) == 0);
    std::size_t pa = 0, pb = 0;
    const float* fa = clay_tape_params(a, &pa);
    const float* fb = clay_tape_params(b, &pb);
    REQUIRE(pa == pb);
    CHECK(std::memcmp(fa, fb, pa * sizeof(float)) == 0);
    clay_tape_release(a);
    clay_tape_release(b);
}
