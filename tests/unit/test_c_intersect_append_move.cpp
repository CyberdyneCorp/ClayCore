#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "clay.h"

// ISSUE #665: an append followed by a region edit must not launder the seeds
// the append left behind.
//
// Appending an INTERSECT item and then moving that same item with a uniform
// transform, with no refill between, left the brick cache holding the
// un-intersected layer -- and a later mark over the whole bound plus a refill
// did not repair it. The document was right; the bricks were not.
//
// The cause was in the seed store, not in the bound. An append re-stamps no
// seed (it is the one edit a seed can be carried across, by the append log).
// The move that followed took the region invalidation, which forgets the log
// and then advances every seed its box does not reach to the NEW revision --
// including the ones still sitting at the revision BEFORE the append. For a
// local item that is harmless only because its move's box covers where it was
// appended; an intersect changes the whole layer, and the #471 swept box is
// deliberately much smaller than that. Those seeds were then "current", so the
// refill's `rev == now` shortcut handed their pre-append values straight back,
// however many times the host dirtied the brick.
//
// The table is the issue's, each variant against the same reference: the item
// built at its final place in one add, into a SEPARATE document (a second cache
// on the same document would read the same seed store and inherit the fault).

namespace {

constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr float kVoxel = 0.02f;
constexpr float kLift = 0.9f;

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    Doc() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "Base", &layer) == CLAY_OK);
    }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

struct Cache {
    clay_brick_cache* c = nullptr;
    Cache() {
        clay_brick_config cfg;
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        cfg.dim = kDim;
        cfg.voxel_size = kVoxel;
        cfg.band_voxels = 3;
        cfg.memory_budget = 0;
        cfg.colors = 0;
        c = clay_brick_cache_create(&cfg);
        REQUIRE(c != nullptr);
    }
    ~Cache() { clay_brick_cache_destroy(c); }
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
};

clay_node_id add_item(Doc& doc, int32_t prim, const float* params, std::size_t n, int32_t op,
                      float y) {
    clay_item* it = clay_item_create(prim, params, n);
    REQUIRE(it != nullptr);
    const float pos[3] = {0.0f, y, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_op(it, op) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

clay_node_id add_sphere(Doc& doc) {
    const float r = 1.0f;
    return add_item(doc, CLAY_PRIM_SPHERE, &r, 1, CLAY_OP_ADD, 0.0f);
}

// The issue's cutter: radius 0.25, half-height 1.6, intersecting.
clay_node_id add_cutter(Doc& doc, float y) {
    const float params[2] = {0.25f, 1.6f};
    return add_item(doc, CLAY_PRIM_CAPPED_CYLINDER, params, 2, CLAY_OP_INTERSECT, y);
}

void move_uniform(Doc& doc, clay_node_id node, float y) {
    const float pos[3] = {0.0f, y, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_layer_set_transform(doc.d, doc.layer, node, pos, axis, 0.0f, 1.0f) == CLAY_OK);
}

void move_nonuniform(Doc& doc, clay_node_id node, float y) {
    const float pos[3] = {0.0f, y, 0.0f};
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const float scale[3] = {1.0f, 1.001f, 1.0f};
    REQUIRE(clay_layer_set_transform_nonuniform(doc.d, doc.layer, node, pos, axis, 0.0f, scale) ==
            CLAY_OK);
}

// take_dirty -> eval_requests -> submit until nothing is left.
void drain(clay_brick_cache* cache, const clay_document* doc) {
    constexpr std::size_t kChunk = 256;
    std::vector<clay_brick_request> reqs(kChunk);
    std::vector<float> values(kChunk * kSamples);
    std::vector<std::int32_t> results(kChunk);
    for (;;) {
        std::size_t count = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(), count * kSamples,
                                        nullptr, 0, results.data(), &accepted) == CLAY_OK);
        REQUIRE(accepted == count);
        if (remaining == 0) break;
    }
}

// What the issue's host did after the edits: mark the intersect's whole
// influence bound (the layer's extent, so it covers every stale brick) dirty.
void mark_node_bound(clay_brick_cache* cache, const Doc& doc, clay_node_id node) {
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    int32_t has = 0, infinite = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, node, lo, hi, &has, &infinite) ==
            CLAY_OK);
    REQUIRE(has == 1);
    REQUIRE(infinite == 0);
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
}

using Snapshot = std::map<std::tuple<int, int, int>, std::vector<std::uint16_t>>;

// Every surface brick's fp16 payload, keyed by brick coordinate.
Snapshot snapshot(const clay_brick_cache* cache) {
    std::size_t count = 0;
    REQUIRE(clay_brick_cache_surface_bricks(cache, nullptr, &count) == CLAY_OK);
    Snapshot out;
    if (count == 0) return out;
    std::vector<std::int32_t> keys(count * 3);
    std::size_t capacity = count;
    REQUIRE(clay_brick_cache_surface_bricks(cache, keys.data(), &capacity) == CLAY_OK);
    std::vector<std::uint16_t> halves(count * kSamples);
    std::vector<std::int32_t> states(count);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys.data(), count, 0, states.data(),
                                         halves.data(), count * kSamples, nullptr, 0) == CLAY_OK);
    for (std::size_t i = 0; i < count; ++i) {
        const auto first = halves.begin() + static_cast<std::ptrdiff_t>(i * kSamples);
        out[{keys[i * 3], keys[i * 3 + 1], keys[i * 3 + 2]}].assign(
            first, first + static_cast<std::ptrdiff_t>(kSamples));
    }
    return out;
}

std::uint64_t surface_bricks(const clay_brick_cache* cache) {
    clay_brick_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<std::uint32_t>(sizeof s);
    REQUIRE(clay_brick_cache_stats(cache, &s) == CLAY_OK);
    return s.surface_bricks;
}

// The issue's probe: a ray straight down outside the cylinder, through where
// the sphere was. The intersected form has nothing there.
int32_t cache_hits_outside_cutter(const clay_brick_cache* cache) {
    const float o[3] = {-0.6f, 4.0f, -0.3f};
    const float dir[3] = {0.0f, -1.0f, 0.0f};
    int32_t hit = -1;
    REQUIRE(clay_brick_cache_raycast(cache, o, dir, &hit, nullptr, nullptr, nullptr) == CLAY_OK);
    return hit;
}

int32_t doc_hits_outside_cutter(const Doc& doc) {
    const float o[3] = {-0.6f, 4.0f, -0.3f};
    const float dir[3] = {0.0f, -1.0f, 0.0f};
    int32_t hit = -1;
    REQUIRE(clay_raycast(doc.d, o, dir, &hit, nullptr, nullptr, nullptr) == CLAY_OK);
    return hit;
}

// A document holding the sphere, with its cache fully filled.
struct Scene {
    Doc doc;
    Cache cache;
    Scene() {
        add_sphere(doc);
        REQUIRE(clay_brick_cache_mark_dirty_layer(cache.c, doc.d, doc.layer) == CLAY_OK);
        drain(cache.c, doc.d);
    }
    void finish(clay_node_id cutter) {
        mark_node_bound(cache.c, doc, cutter);
        drain(cache.c, doc.d);
    }
};

// The reference: the cutter built at its place in one add, in its own document.
Snapshot reference(std::uint64_t* out_surface) {
    Scene s;
    s.finish(add_cutter(s.doc, kLift));
    *out_surface = surface_bricks(s.cache.c);
    return snapshot(s.cache.c);
}

void check_matches_reference(const Scene& s) {
    std::uint64_t want_surface = 0;
    const Snapshot want = reference(&want_surface);
    // The reference itself is the intersected form, not the sphere.
    REQUIRE(want_surface > 0);
    REQUIRE(want_surface < 400);
    CHECK(doc_hits_outside_cutter(s.doc) == 0);
    CHECK(cache_hits_outside_cutter(s.cache.c) == 0);
    CHECK(surface_bricks(s.cache.c) == want_surface);
    const bool same_bits = snapshot(s.cache.c) == want;
    CHECK(same_bits);
}

}  // namespace

TEST_CASE("#665: append an intersect, move it uniformly, then refill -- no stale layer") {
    Scene s;
    const clay_node_id cutter = add_cutter(s.doc, 0.0f);
    move_uniform(s.doc, cutter, kLift);  // the surface-delta fast path, at the appended ordinal
    s.finish(cutter);
    check_matches_reference(s);
}

TEST_CASE("#665: re-dirtying a brick after the append+move sequence still refills it") {
    // The second half of the report: even after the first refill had happened,
    // marking the whole bound again did not repair the cache.
    Scene s;
    const clay_node_id cutter = add_cutter(s.doc, 0.0f);
    move_uniform(s.doc, cutter, kLift);
    s.finish(cutter);
    s.finish(cutter);
    check_matches_reference(s);
}

TEST_CASE("#665: append a local item, then move ANOTHER item elsewhere, then refill") {
    // The same laundering without an intersect: the move's box does not cover
    // where the appended item landed, so nothing but the append log could have
    // carried those seeds -- and the move forgets it.
    Scene s;
    const float r = 0.2f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float far_pos[3] = {3.0f, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, far_pos) == CLAY_OK);
    clay_node_id far_node = 0;
    REQUIRE(clay_layer_add_item(s.doc.d, s.doc.layer, it, &far_node) == CLAY_OK);
    clay_item_destroy(it);
    REQUIRE(clay_brick_cache_mark_dirty_layer(s.cache.c, s.doc.d, s.doc.layer) == CLAY_OK);
    drain(s.cache.c, s.doc.d);

    // Append a bump on the big sphere, then move the far one: its box is far
    // from the bump.
    const float bump_r = 0.3f;
    clay_item* bump = clay_item_create(CLAY_PRIM_SPHERE, &bump_r, 1);
    REQUIRE(bump != nullptr);
    const float bump_pos[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(clay_item_set_position(bump, bump_pos) == CLAY_OK);
    clay_node_id bump_node = 0;
    REQUIRE(clay_layer_add_item(s.doc.d, s.doc.layer, bump, &bump_node) == CLAY_OK);
    clay_item_destroy(bump);
    const float far_axis[3] = {0.0f, 1.0f, 0.0f};
    const float far_to[3] = {3.0f, 0.1f, 0.0f};
    REQUIRE(clay_layer_set_transform(s.doc.d, s.doc.layer, far_node, far_to, far_axis, 0.0f,
                                     1.0f) == CLAY_OK);
    REQUIRE(clay_brick_cache_mark_dirty_layer(s.cache.c, s.doc.d, s.doc.layer) == CLAY_OK);
    drain(s.cache.c, s.doc.d);

    // Reference: the same final document built without the edit history.
    Doc ref;
    add_sphere(ref);
    clay_node_id ignored = 0;
    clay_item* a = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(clay_item_set_position(a, far_to) == CLAY_OK);
    REQUIRE(clay_layer_add_item(ref.d, ref.layer, a, &ignored) == CLAY_OK);
    clay_item_destroy(a);
    clay_item* b = clay_item_create(CLAY_PRIM_SPHERE, &bump_r, 1);
    REQUIRE(clay_item_set_position(b, bump_pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(ref.d, ref.layer, b, &ignored) == CLAY_OK);
    clay_item_destroy(b);
    Cache ref_cache;
    REQUIRE(clay_brick_cache_mark_dirty_layer(ref_cache.c, ref.d, ref.layer) == CLAY_OK);
    drain(ref_cache.c, ref.d);
    CHECK(surface_bricks(s.cache.c) == surface_bricks(ref_cache.c));
    const bool same_bits = snapshot(s.cache.c) == snapshot(ref_cache.c);
    CHECK(same_bits);
}

// -- the controls: every row of the issue's table that was already right ------

TEST_CASE("#665 control: the cutter built at its place in one add") {
    Scene s;
    s.finish(add_cutter(s.doc, kLift));
    check_matches_reference(s);
}

TEST_CASE("#665 control: append, then a NON-uniform move (no surface-delta fast path)") {
    Scene s;
    const clay_node_id cutter = add_cutter(s.doc, 0.0f);
    move_nonuniform(s.doc, cutter, kLift);
    s.finish(cutter);
    CHECK(doc_hits_outside_cutter(s.doc) == 0);
    CHECK(cache_hits_outside_cutter(s.cache.c) == 0);
    std::uint64_t want = 0;
    reference(&want);
    // A 0.1% taller cylinder can touch a brick or two more; the sphere is 1171.
    CHECK(surface_bricks(s.cache.c) < want + 16);
}

TEST_CASE("#665 control: append, refill, then the uniform move") {
    Scene s;
    const clay_node_id cutter = add_cutter(s.doc, 0.0f);
    s.finish(cutter);
    move_uniform(s.doc, cutter, kLift);
    s.finish(cutter);
    check_matches_reference(s);
}

TEST_CASE("#665 control: cutter at its place, refill, two uniform moves with no refill between") {
    Scene s;
    const clay_node_id cutter = add_cutter(s.doc, kLift);
    s.finish(cutter);
    move_uniform(s.doc, cutter, 0.3f);
    move_uniform(s.doc, cutter, kLift);
    s.finish(cutter);
    check_matches_reference(s);
}

TEST_CASE("#665 control: cutter at its place, refill, append another item, move the cutter") {
    Scene s;
    const clay_node_id cutter = add_cutter(s.doc, kLift);
    s.finish(cutter);
    // An item that the cylinder leaves nothing of, so the final form is the
    // reference's: a small sphere far outside the cutter.
    const float r = 0.1f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {0.7f, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    clay_node_id small = 0;
    REQUIRE(clay_layer_add_item(s.doc.d, s.doc.layer, it, &small) == CLAY_OK);
    clay_item_destroy(it);
    move_uniform(s.doc, cutter, 0.3f);
    move_uniform(s.doc, cutter, kLift);
    s.finish(cutter);
    CHECK(doc_hits_outside_cutter(s.doc) == 0);
    CHECK(cache_hits_outside_cutter(s.cache.c) == 0);
}

// -- and the fix must not cost the carry it narrows ---------------------------
//
// A seed an append left behind is still carried across a later region edit
// when NO append in between reached it -- which after a stroke is almost every
// seed on the model. Dropping them all would be correct and would send the
// next edit there down the full walk, brick by brick; this pins that they are
// still answered from their seeds, and still right.

namespace {

clay_resume_stats resume_stats(const clay_document* doc) {
    clay_resume_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<std::uint32_t>(sizeof s);
    REQUIRE(clay_document_resume_stats(doc, &s) == CLAY_OK);
    return s;
}

clay_node_id add_sphere_at(Doc& doc, float r, const float pos[3]) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

}  // namespace

TEST_CASE("#665: a seed no append reached is still carried across the next region edit") {
    Doc doc;
    add_sphere(doc);
    const float far_at[3] = {3.0f, 0.0f, 0.0f};
    const clay_node_id far_node = add_sphere_at(doc, 0.2f, far_at);
    Cache cache;
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache.c, doc.d, doc.layer) == CLAY_OK);
    drain(cache.c, doc.d);

    // A dab on the TOP of the sphere, then a move of the far item: neither
    // reaches the sphere's underside.
    const float bump_at[3] = {0.0f, 1.0f, 0.0f};
    add_sphere_at(doc, 0.2f, bump_at);
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const float far_to[3] = {3.0f, 0.05f, 0.0f};
    REQUIRE(clay_layer_set_transform(doc.d, doc.layer, far_node, far_to, axis, 0.0f, 1.0f) ==
            CLAY_OK);

    // Re-dirty only the underside and refill it.
    const clay_resume_stats before = resume_stats(doc.d);
    const float lo[3] = {-0.3f, -1.1f, -0.3f};
    const float hi[3] = {0.3f, -0.8f, 0.3f};
    REQUIRE(clay_brick_cache_mark_dirty(cache.c, lo, hi) == CLAY_OK);
    drain(cache.c, doc.d);
    const clay_resume_stats after = resume_stats(doc.d);
    // Not every brick of the window resumes -- one the refill proves uniform
    // counts as refilled whatever its seed (the clay_resume_stats contract),
    // and the same share did before this fix. Dropping the lagging seeds turns
    // the resumed count to zero; that is the regression this pins.
    const std::uint64_t resumed = after.resumed_bricks - before.resumed_bricks;
    const std::uint64_t refilled = after.refilled_bricks - before.refilled_bricks;
    CHECK(resumed > 0);
    CHECK(resumed > refilled);

    // And what they were answered with is the document, bit for bit.
    Doc ref;
    add_sphere(ref);
    add_sphere_at(ref, 0.2f, far_to);
    add_sphere_at(ref, 0.2f, bump_at);
    Cache ref_cache;
    REQUIRE(clay_brick_cache_mark_dirty_layer(ref_cache.c, ref.d, ref.layer) == CLAY_OK);
    drain(ref_cache.c, ref.d);
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache.c, doc.d, doc.layer) == CLAY_OK);
    drain(cache.c, doc.d);
    const bool same_bits = snapshot(cache.c) == snapshot(ref_cache.c);
    CHECK(same_bits);
}
