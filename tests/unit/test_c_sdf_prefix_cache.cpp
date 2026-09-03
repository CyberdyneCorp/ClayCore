// The SDF prefix cache across the C ABI (c-abi spec, expose-the-prefix-cache,
// closing add-sdf-prefix-cache 17.3).
//
// WHAT WAS MISSING, and it was not the engine. `SdfPrefixCache` has made a cold
// window cost its suffix rather than the whole edit history since #371 — 2.32 ms
// against 242 ms on a 20,000-item layer — and `clay_sdf_smooth_begin` let the
// cache argument default to null, so no C or Python host could ask for any of
// it. These check that a host can now hold one, schedule its build, and have a
// gesture actually served by it.
//
// THE COUNTERS ARE THE POINT. A timing here would prove nothing on a shared
// box, and the OUTPUT of an accelerated gesture is identical to an
// unaccelerated one by design — that is the contract, not a coincidence. So
// what is asserted is `builds`, `entries` and `seeded_windows`: the difference
// between "fast because the cache worked" and "fast because the test touched
// the same window twice".

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

const float kCell = 0.05f;

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

// A policy that WILL cache: a boundary needs more roots than `min_history` and
// more than `keep_live_suffix`, and a budget of zero caches nothing.
clay_sculpt_policy caching_policy(uint64_t keep_live = 8, uint64_t min_history = 4) {
    clay_sculpt_policy p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.cell_size = kCell;
    p.prefix_min_history_roots = min_history;
    p.prefix_keep_live_suffix_roots = keep_live;
    p.prefix_max_bytes = 256u * 1024u * 1024u;
    return p;
}

clay_sdf_prefix_stats stats_out() {
    clay_sdf_prefix_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<uint32_t>(sizeof s);
    return s;
}

clay_sdf_prefix_stats stats_of(const clay_sdf_prefix_cache* cache) {
    clay_sdf_prefix_stats s = stats_out();
    REQUIRE(clay_sdf_prefix_cache_stats(cache, &s) == CLAY_OK);
    return s;
}

// A worked sphere: a base plus `dabs` stamps walked over its surface by the
// golden angle, which is the shape #306's long-history fixtures use and the
// shape the C++ prefix tests use, so the two sides are measuring one thing.
clay_layer_id worked(clay_document* doc, int dabs) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc base;
    std::memset(&base, 0, sizeof base);
    base.struct_size = static_cast<uint32_t>(sizeof base);
    base.prim = CLAY_PRIM_SPHERE;
    base.params[0] = 1.0f;
    base.op = CLAY_OP_ADD;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &base, &node) == CLAY_OK);
    for (int i = 1; i < dabs; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(dabs);
        const float r = std::sqrt(z * z >= 1.0f ? 0.0f : 1.0f - z * z);
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_SPHERE;
        d.params[0] = 0.09f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.04f;
        d.position[0] = r * std::cos(t);
        d.position[1] = r * std::sin(t);
        d.position[2] = z;
        REQUIRE(clay_add_item(doc, layer, &d, &node) == CLAY_OK);
    }
    return layer;
}

}  // namespace

TEST_CASE("c abi: a host can ask where a prefix boundary falls without building one") {
    CDoc d;
    const clay_layer_id layer = worked(d.doc, 40);
    const clay_sculpt_policy policy = caching_policy();

    uint64_t roots = 0;
    REQUIRE(clay_sdf_prefix_boundary_for(d.doc, layer, &policy, &roots) == CLAY_OK);
    // 40 roots keeping 8 live leaves 32 behind the boundary.
    CHECK(roots == 32);

    // A policy that declines: a budget of 0 is OFF, not unbounded, which is the
    // safe reading of a field nobody filled in.
    clay_sculpt_policy off = policy;
    off.prefix_max_bytes = 0;
    REQUIRE(clay_sdf_prefix_boundary_for(d.doc, layer, &off, &roots) == CLAY_OK);
    CHECK(roots == 0);

    // A layer too shallow to be worth caching declines too, rather than caching
    // a prefix that saves less than it costs.
    CDoc shallow;
    const clay_layer_id thin = worked(shallow.doc, 3);
    REQUIRE(clay_sdf_prefix_boundary_for(shallow.doc, thin, &policy, &roots) == CLAY_OK);
    CHECK(roots == 0);
}

TEST_CASE("c abi: a cold window is served from a prefix a host built") {
    CDoc d;
    const clay_layer_id layer = worked(d.doc, 40);
    const clay_sculpt_policy policy = caching_policy();

    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(256u * 1024u * 1024u);
    REQUIRE(cache != nullptr);

    // BEGINNING BUILDS NOTHING. This is the property the whole lazy design
    // rests on: the build is the whole layer's cost and a gesture begins at the
    // moment an artist has already put their finger down.
    clay_sdf_smooth_tx* cold =
        clay_sdf_smooth_begin_cached(d.doc, layer, &policy, cache, nullptr);
    REQUIRE(cold != nullptr);
    CHECK(stats_of(cache).builds == 0);
    CHECK(stats_of(cache).entries == 0);
    clay_sdf_smooth_destroy(cold);

    // The host schedules the build itself, which is the door 17.3 was about.
    REQUIRE(clay_sdf_prefix_cache_build(cache, d.doc, layer, &policy, nullptr) == CLAY_OK);
    const clay_sdf_prefix_stats after_build = stats_of(cache);
    CHECK(after_build.builds == 1);
    CHECK(after_build.entries == 1);
    CHECK(after_build.bytes > 0);

    // And now a gesture is actually served by it. `seeded_windows` is what says
    // so — the gesture's OUTPUT is identical either way by design, so nothing
    // in the result could have told us.
    //
    // THE DAB IS WHAT MEASURES IT, not the begin. Beginning samples nothing
    // (that is the lazy design), so the counters do not move until a window is
    // actually filled — which is `update`. Asserting on them straight after
    // `begin` reads zero and looks like the cache failing.
    clay_sdf_smooth_tx* warm =
        clay_sdf_smooth_begin_cached(d.doc, layer, &policy, cache, nullptr);
    REQUIRE(warm != nullptr);
    CHECK(stats_of(cache).seeded_windows == 0);  // still nothing sampled

    clay_relax_params relax;
    std::memset(&relax, 0, sizeof relax);
    relax.struct_size = static_cast<uint32_t>(sizeof relax);
    relax.strength = 0.8f;
    relax.radius_cells = 1;
    relax.iterations = 1;
    relax.centre[0] = 0.0f;
    relax.centre[1] = 0.0f;
    relax.centre[2] = 1.0f;
    relax.region_radius = 0.3f;
    REQUIRE(clay_sdf_smooth_update(warm, &relax, nullptr, nullptr) == CLAY_OK);

    const clay_sdf_prefix_stats served = stats_of(cache);
    CHECK(served.seeded_windows > 0);
    // Still one build: a gesture consumes a prefix and never makes one.
    CHECK(served.builds == 1);
    clay_sdf_smooth_destroy(warm);

    clay_sdf_prefix_cache_destroy(cache);
}

TEST_CASE("c abi: a cache belongs to whoever made it, not to a document") {
    // The lifetime rule, stated in the header and asserted here: destroying the
    // document leaves the cache usable, because no document owns one and none
    // points at one. Under ASan this is the test that would say otherwise.
    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(64u * 1024u * 1024u);
    REQUIRE(cache != nullptr);
    {
        CDoc d;
        const clay_layer_id layer = worked(d.doc, 40);
        const clay_sculpt_policy policy = caching_policy();
        REQUIRE(clay_sdf_prefix_cache_build(cache, d.doc, layer, &policy, nullptr) == CLAY_OK);
        CHECK(stats_of(cache).entries == 1);
    }
    // The document is gone. The cache is not.
    CHECK(stats_of(cache).entries == 1);
    REQUIRE(clay_sdf_prefix_cache_clear(cache) == CLAY_OK);
    CHECK(stats_of(cache).entries == 0);
    clay_sdf_prefix_cache_destroy(cache);
}

TEST_CASE("c abi: the prefix cache respects the budget it was given") {
    CDoc d;
    const clay_layer_id layer = worked(d.doc, 40);
    const clay_sculpt_policy policy = caching_policy();

    // A handle created with 0 is a WORKING handle that caches nothing, rather
    // than one that caches without limit.
    clay_sdf_prefix_cache* none = clay_sdf_prefix_cache_create(0);
    REQUIRE(none != nullptr);
    clay_sdf_prefix_cache_build(none, d.doc, layer, &policy, nullptr);
    CHECK(stats_of(none).entries == 0);
    CHECK(stats_of(none).bytes == 0);
    clay_sdf_prefix_cache_destroy(none);

    // Lowering the ceiling under what is held evicts down to it at once.
    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(256u * 1024u * 1024u);
    REQUIRE(cache != nullptr);
    REQUIRE(clay_sdf_prefix_cache_build(cache, d.doc, layer, &policy, nullptr) == CLAY_OK);
    REQUIRE(stats_of(cache).entries == 1);
    REQUIRE(clay_sdf_prefix_cache_set_max_bytes(cache, 1) == CLAY_OK);
    CHECK(stats_of(cache).entries == 0);
    CHECK(stats_of(cache).bytes == 0);

    // Invalidating a layer drops what was cached for it and nothing else.
    REQUIRE(clay_sdf_prefix_cache_set_max_bytes(cache, 256u * 1024u * 1024u) == CLAY_OK);
    REQUIRE(clay_sdf_prefix_cache_build(cache, d.doc, layer, &policy, nullptr) == CLAY_OK);
    REQUIRE(stats_of(cache).entries == 1);
    REQUIRE(clay_sdf_prefix_cache_invalidate_layer(cache, layer) == CLAY_OK);
    CHECK(stats_of(cache).entries == 0);

    clay_sdf_prefix_cache_destroy(cache);
}

TEST_CASE("c abi: a null cache is exactly the uncached begin") {
    CDoc d;
    const clay_layer_id layer = worked(d.doc, 40);
    const clay_sculpt_policy policy = caching_policy();

    clay_sdf_smooth_tx* a = clay_sdf_smooth_begin(d.doc, layer, &policy, nullptr);
    REQUIRE(a != nullptr);
    clay_sdf_smooth_destroy(a);

    clay_sdf_smooth_tx* b = clay_sdf_smooth_begin_cached(d.doc, layer, &policy, nullptr, nullptr);
    REQUIRE(b != nullptr);
    clay_sdf_smooth_destroy(b);
}

TEST_CASE("c abi: a policy that sets none of the prefix knobs is today's behaviour") {
    // The compatibility claim of the grown descriptor, and the one a host that
    // has never heard of this cares about: three zeroed fields cache nothing.
    CDoc d;
    const clay_layer_id layer = worked(d.doc, 40);
    clay_sculpt_policy plain;
    std::memset(&plain, 0, sizeof plain);
    plain.struct_size = static_cast<uint32_t>(sizeof plain);
    plain.cell_size = kCell;

    uint64_t roots = 0;
    REQUIRE(clay_sdf_prefix_boundary_for(d.doc, layer, &plain, &roots) == CLAY_OK);
    CHECK(roots == 0);

    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(64u * 1024u * 1024u);
    REQUIRE(cache != nullptr);
    // Refused, and it says why rather than caching nothing quietly.
    CHECK(clay_sdf_prefix_cache_build(cache, d.doc, layer, &plain, nullptr) != CLAY_OK);
    CHECK(stats_of(cache).entries == 0);
    clay_sdf_prefix_cache_destroy(cache);
}
