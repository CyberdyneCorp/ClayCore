// A brick with no seed starts from the layer's prefix instead of the whole
// edit list (c-abi spec, issue #306).
//
// WHAT WAS LEFT OF #306. The resumable refill made a dab inside a stroke flat
// in document size -- a brick that has been filled before evaluates only what
// the document gained since. The FIRST touch of a window has no seed and walks
// everything: 0.004 ms warm against 33.7 ms cold at 50,000 items, and a SECOND
// cold window costs the same as the first, so it is the walk and not the index.
// A stroke crosses brick planes constantly, so it lands mid-gesture.
//
// THE THREE THINGS THIS FILE GATES, and the order matters because only the
// first one is about correctness:
//
//   1. THE SEEDED ANSWER IS THE WALK'S ANSWER. Asserted against a fresh
//      document's full walk, in-band, because that is the whole of what a
//      sampled field promises -- outside the band both paths clamp and a
//      comparison there measures the clamp.
//   2. THE PREFIX ACTUALLY SERVED. A cache that covers nothing is
//      indistinguishable from one that is off, and both are "correct".
//   3. A NULL CACHE CHANGES NOTHING, so a host that has not opted in cannot be
//      affected by any of it.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

struct CDoc {
    clay_document* doc = clay_document_create();
    CDoc() { REQUIRE(doc != nullptr); }
    ~CDoc() { clay_document_destroy(doc); }
    CDoc(const CDoc&) = delete;
    CDoc& operator=(const CDoc&) = delete;
};

constexpr int kDim = 8;
constexpr float kVoxel = 0.05f;

// A sphere worked over with small dabs, which is a history worth not walking.
clay_layer_id worked(clay_document* doc, int items) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    clay_item_desc base;
    std::memset(&base, 0, sizeof base);
    base.struct_size = static_cast<uint32_t>(sizeof base);
    base.prim = CLAY_PRIM_SPHERE;
    base.params[0] = 1.0f;
    base.op = CLAY_OP_ADD;
    clay_node_id n = 0;
    REQUIRE(clay_add_item(doc, layer, &base, &n) == CLAY_OK);
    for (int i = 0; i < items; ++i) {
        const float t = static_cast<float>(i) * 2.399963f;
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(items);
        const float r = std::sqrt(z * z >= 1.0f ? 0.0f : 1.0f - z * z);
        clay_item_desc d;
        std::memset(&d, 0, sizeof d);
        d.struct_size = static_cast<uint32_t>(sizeof d);
        d.prim = CLAY_PRIM_SPHERE;
        d.params[0] = 0.06f;
        d.op = CLAY_OP_ADD;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.02f;
        d.position[0] = r * std::cos(t);
        d.position[1] = r * std::sin(t);
        d.position[2] = z;
        clay_node_id id = 0;
        REQUIRE(clay_add_item(doc, layer, &d, &id) == CLAY_OK);
    }
    return layer;
}

// A window ON THE SURFACE. A brick spans dim * voxel = 0.4 units, so key 2
// starts at x = 0.8 and crosses the unit sphere at x = 1. A window off the
// model would have every item culled away and a cold refill there is free --
// which measured 0.126 ms and would have said the cliff was already gone.
std::vector<clay_brick_request> window(int wx, int wy, int wz) {
    std::vector<clay_brick_request> r;
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 3; ++z) {
                clay_brick_request q{};
                q.key[0] = wx + x;
                q.key[1] = wy + y;
                q.key[2] = wz + z;
                q.spacing = kVoxel;
                q.dims[0] = q.dims[1] = q.dims[2] = kDim;
                q.band = 3.0f * kVoxel;
                for (int a = 0; a < 3; ++a)
                    q.origin[a] = static_cast<float>(q.key[a] * kDim) * kVoxel;
                r.push_back(q);
            }
    return r;
}

clay_sculpt_policy prefix_policy() {
    clay_sculpt_policy p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<uint32_t>(sizeof p);
    p.cell_size = kVoxel;
    p.band = 8.0f * kVoxel;  // wide enough that a brick-sized window is stored
    p.padding = 8.0f * kVoxel;
    p.prefix_min_history_roots = 64;
    p.prefix_keep_live_suffix_roots = 32;
    p.prefix_max_bytes = 512ull * 1024 * 1024;
    return p;
}

std::vector<float> refill(const clay_document* doc, const std::vector<clay_brick_request>& w) {
    std::vector<float> v(w.size() * kDim * kDim * kDim, 0.0f);
    REQUIRE(clay_brick_cache_eval_requests(doc, "cpu", w.data(), w.size(), v.data(), v.size(),
                                           nullptr, 0) == CLAY_OK);
    return v;
}

}  // namespace

TEST_CASE("cold brick: a prefix-seeded refill is the walk's answer") {
    const int kItems = 400;
    CDoc d;
    const clay_layer_id layer = worked(d.doc, kItems);
    const clay_sculpt_policy pol = prefix_policy();

    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(512ull * 1024 * 1024);
    REQUIRE(cache != nullptr);
    // The REFILL's build, not Smooth's. The two read the prefix on different
    // lattices and a seed read off the wrong one is an interpolation of two
    // samples rather than one of them.
    REQUIRE(clay_sdf_prefix_cache_build_for_refill(cache, d.doc, layer, &pol, nullptr) == CLAY_OK);

    const std::vector<clay_brick_request> w = window(2, -1, -1);
    std::vector<float> got(w.size() * kDim * kDim * kDim, 0.0f);
    uint64_t seeded = 0;
    REQUIRE(clay_brick_cache_eval_requests_seeded(d.doc, cache, &pol, "cpu", w.data(), w.size(),
                                                  got.data(), got.size(), nullptr, 0,
                                                  &seeded) == CLAY_OK);

    // (2) The prefix served. Without this the case passes when the cache is
    // covering nothing, which is the failure this feature actually has.
    MESSAGE("bricks seeded from the prefix: " << seeded << " of " << w.size());
    CHECK(seeded > 0);

    // (1) Against a FRESH document's full walk -- the oracle, with no seeds of
    // any kind in it.
    CDoc fresh;
    worked(fresh.doc, kItems);
    const std::vector<float> want = refill(fresh.doc, w);
    REQUIRE(want.size() == got.size());

    const float band = 3.0f * kVoxel;
    double worst = 0.0;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (std::fabs(want[i]) > band) continue;  // both paths clamp outside it
        worst = std::max(worst, static_cast<double>(std::fabs(want[i] - got[i])));
        ++compared;
    }
    MESSAGE("in-band samples " << compared << ", worst " << worst);
    // A gate that compared nothing would pass.
    CHECK(compared > 500);
    // Float rounding, which is what a seed read ON the lattice it was built for
    // costs. A seed read half a cell away is an interpolation of two samples
    // and lands about a quarter of a cell out -- 0.011 at this cell size,
    // measured before the refill build was given its own alignment.
    CHECK(worst < 1e-5);

    clay_sdf_prefix_cache_destroy(cache);
}

TEST_CASE("cold brick: a null cache answers exactly what the plain refill answers") {
    // The opt-in property. A host that has not heard of any of this must not be
    // able to tell that it exists.
    const int kItems = 300;
    CDoc d;
    worked(d.doc, kItems);
    const clay_sculpt_policy pol = prefix_policy();
    const std::vector<clay_brick_request> w = window(2, -1, -1);

    const std::vector<float> plain = refill(d.doc, w);

    CDoc other;
    worked(other.doc, kItems);
    std::vector<float> seeded_out(w.size() * kDim * kDim * kDim, 0.0f);
    uint64_t seeded = 1;
    REQUIRE(clay_brick_cache_eval_requests_seeded(other.doc, nullptr, &pol, "cpu", w.data(),
                                                  w.size(), seeded_out.data(), seeded_out.size(),
                                                  nullptr, 0, &seeded) == CLAY_OK);
    CHECK(seeded == 0);
    REQUIRE(seeded_out.size() == plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i) CHECK(seeded_out[i] == plain[i]);
}

TEST_CASE("cold brick: an uncovered window takes the walk rather than a bad seed") {
    // THE FAR-BOUND RULE, which is the whole correctness argument. A sparse
    // volume answers interpolation where it stores samples and a conservative
    // FAR BOUND where it does not, and a suffix folded onto that bound is wrong
    // by about 14 cells. So a window the prefix does not fully store must be
    // refused rather than seeded.
    //
    // Provoked with a NARROW band, so the prefix stores only a thin shell and a
    // brick-sized window reaches past it.
    const int kItems = 400;
    CDoc d;
    const clay_layer_id layer = worked(d.doc, kItems);
    clay_sculpt_policy narrow = prefix_policy();
    narrow.band = kVoxel;  // one cell: far too thin to cover a brick
    narrow.padding = kVoxel;

    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(512ull * 1024 * 1024);
    REQUIRE(cache != nullptr);
    REQUIRE(clay_sdf_prefix_cache_build_for_refill(cache, d.doc, layer, &narrow, nullptr) ==
            CLAY_OK);

    const std::vector<clay_brick_request> w = window(2, -1, -1);
    std::vector<float> got(w.size() * kDim * kDim * kDim, 0.0f);
    uint64_t seeded = 0;
    REQUIRE(clay_brick_cache_eval_requests_seeded(d.doc, cache, &narrow, "cpu", w.data(), w.size(),
                                                  got.data(), got.size(), nullptr, 0,
                                                  &seeded) == CLAY_OK);
    MESSAGE("with a one-cell band, bricks seeded: " << seeded << " of " << w.size());

    // Whatever it decided, the answer is the walk's. That is the claim: refusing
    // is slower and never wrong.
    CDoc fresh;
    worked(fresh.doc, kItems);
    const std::vector<float> want = refill(fresh.doc, w);
    const float band = 3.0f * kVoxel;
    double worst = 0.0;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (std::fabs(want[i]) > band) continue;
        worst = std::max(worst, static_cast<double>(std::fabs(want[i] - got[i])));
        ++compared;
    }
    CHECK(compared > 500);
    CHECK(worst < 1e-5);
    clay_sdf_prefix_cache_destroy(cache);
}

TEST_CASE("cold brick: a stroke keeps hitting as its boundary moves") {
    // THE CASE THE POLICY'S OWN BOUNDARY WOULD MISS. `prefix_boundary_for` is
    // roots - keep_live_suffix_roots, so every stamp moves it by one: a lookup
    // by the CURRENT boundary would miss on every dab of a stroke, which is
    // exactly when a cold window is reached. An older prefix is still valid --
    // an append cannot change the roots before it -- so the lookup takes the
    // best boundary the cache holds and the suffix absorbs the difference.
    const int kItems = 400;
    CDoc d;
    const clay_layer_id layer = worked(d.doc, kItems);
    const clay_sculpt_policy pol = prefix_policy();

    clay_sdf_prefix_cache* cache = clay_sdf_prefix_cache_create(512ull * 1024 * 1024);
    REQUIRE(cache != nullptr);
    REQUIRE(clay_sdf_prefix_cache_build_for_refill(cache, d.doc, layer, &pol, nullptr) == CLAY_OK);

    // Now stamp more items on, as a stroke does. The boundary the policy names
    // has moved past what the cache holds.
    for (int i = 0; i < 12; ++i) {
        clay_item_desc s;
        std::memset(&s, 0, sizeof s);
        s.struct_size = static_cast<uint32_t>(sizeof s);
        s.prim = CLAY_PRIM_SPHERE;
        s.params[0] = 0.07f;
        s.op = CLAY_OP_ADD;
        s.blend = CLAY_BLEND_QUADRATIC;
        s.blend_k = 0.02f;
        s.position[0] = 0.9f;
        s.position[1] = -0.2f + 0.03f * static_cast<float>(i);
        s.position[2] = 0.0f;
        clay_node_id id = 0;
        REQUIRE(clay_add_item(d.doc, layer, &s, &id) == CLAY_OK);
    }

    const std::vector<clay_brick_request> w = window(-3, -1, -1);
    std::vector<float> got(w.size() * kDim * kDim * kDim, 0.0f);
    uint64_t seeded = 0;
    REQUIRE(clay_brick_cache_eval_requests_seeded(d.doc, cache, &pol, "cpu", w.data(), w.size(),
                                                  got.data(), got.size(), nullptr, 0,
                                                  &seeded) == CLAY_OK);
    MESSAGE("after 12 stamps, bricks still seeded: " << seeded << " of " << w.size());
    // Still served, from the prefix built before the stamps.
    CHECK(seeded > 0);

    // ... and still the walk's answer, with the stamps folded in by the suffix.
    CDoc fresh;
    const clay_layer_id fl = worked(fresh.doc, kItems);
    for (int i = 0; i < 12; ++i) {
        clay_item_desc s;
        std::memset(&s, 0, sizeof s);
        s.struct_size = static_cast<uint32_t>(sizeof s);
        s.prim = CLAY_PRIM_SPHERE;
        s.params[0] = 0.07f;
        s.op = CLAY_OP_ADD;
        s.blend = CLAY_BLEND_QUADRATIC;
        s.blend_k = 0.02f;
        s.position[0] = 0.9f;
        s.position[1] = -0.2f + 0.03f * static_cast<float>(i);
        s.position[2] = 0.0f;
        clay_node_id id = 0;
        REQUIRE(clay_add_item(fresh.doc, fl, &s, &id) == CLAY_OK);
    }
    const std::vector<float> want = refill(fresh.doc, w);
    const float band = 3.0f * kVoxel;
    double worst = 0.0;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (std::fabs(want[i]) > band) continue;
        worst = std::max(worst, static_cast<double>(std::fabs(want[i] - got[i])));
        ++compared;
    }
    MESSAGE("in-band " << compared << ", worst " << worst);
    CHECK(compared > 300);
    CHECK(worst < 1e-5);
    clay_sdf_prefix_cache_destroy(cache);
}
