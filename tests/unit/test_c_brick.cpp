#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "clay.h"

// The brick cache across the C boundary (brick-cache spec + c-abi spec).
//
// The cache is what makes sculpting incremental, and until now it was
// unreachable from any consumer: an iPad app talks to claycore through this
// header alone. So this suite drives the ONE path the header documents —
// mark -> take_dirty -> eval_requests -> submit — and holds it to the two
// claims the design rests on: a small edit re-evaluates FEW bricks (proved
// with a count, not a clock), and every brick outside that edit's influence
// bound stays BIT-IDENTICAL.
//
// It also walks every documented refusal, because the two most dangerous
// arguments here are a region (three floats that can name 1e19 bricks) and a
// count (which the boundary cannot check against the caller's memory).

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr int kBandVoxels = 3;
// The band the default cache is built with, in world units. Culling a
// request must dilate by it, or an item just outside a brick that still
// decides samples inside it is dropped.
constexpr float kBand = kVoxel * static_cast<float>(kBandVoxels);

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

struct Cache {
    clay_brick_cache* c = nullptr;
    explicit Cache(clay_brick_cache* handle) : c(handle) { REQUIRE(c != nullptr); }
    ~Cache() { clay_brick_cache_destroy(c); }
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    operator clay_brick_cache*() const { return c; }
};

clay_brick_config config_of(int dim, float voxel, int band, std::uint64_t budget) {
    clay_brick_config c;
    REQUIRE(clay_brick_config_defaults(&c) == CLAY_OK);
    c.dim = dim;
    c.voxel_size = voxel;
    c.band_voxels = band;
    c.memory_budget = budget;
    return c;
}

clay_brick_cache* make_cache(float voxel = kVoxel, std::uint64_t budget = 0) {
    clay_brick_config c = config_of(kDim, voxel, 3, budget);
    return clay_brick_cache_create(&c);
}

clay_node_id add_sphere(Doc& doc, float r, float x, float y, float z,
                        int32_t op = CLAY_OP_ADD) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, y, z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_op(it, op) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

// What a host's frame loop does, in `chunk`-sized bites so the drain's
// capacity-in/count-out contract is exercised rather than assumed.
struct Refill {
    std::size_t requested = 0;
    std::size_t accepted = 0;
    std::size_t stale = 0;
    std::size_t over_budget = 0;
};

Refill refill(clay_brick_cache* cache, const clay_document* doc, std::size_t chunk = 32) {
    Refill out;
    std::vector<clay_brick_request> reqs(chunk);
    std::vector<float> values(chunk * kSamples);
    std::vector<std::int32_t> results(chunk);
    for (;;) {
        std::size_t count = chunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(), count * kSamples,
                                        nullptr, 0, results.data(), &accepted) == CLAY_OK);
        out.requested += count;
        out.accepted += accepted;
        for (std::size_t i = 0; i < count; ++i) {
            if (results[i] == CLAY_BRICK_SUBMIT_STALE) ++out.stale;
            if (results[i] == CLAY_BRICK_SUBMIT_BUDGET_EXCEEDED) ++out.over_budget;
        }
        if (remaining == 0) break;
    }
    return out;
}

clay_brick_stats stats_of(const clay_brick_cache* cache) {
    clay_brick_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<std::uint32_t>(sizeof s);
    REQUIRE(clay_brick_cache_stats(cache, &s) == CLAY_OK);
    return s;
}

std::vector<std::int32_t> surface_keys(const clay_brick_cache* cache) {
    std::size_t count = 0;
    REQUIRE(clay_brick_cache_surface_bricks(cache, nullptr, &count) == CLAY_OK);
    std::vector<std::int32_t> keys(count * 3);
    if (count == 0) return keys;
    std::size_t capacity = count;
    REQUIRE(clay_brick_cache_surface_bricks(cache, keys.data(), &capacity) == CLAY_OK);
    REQUIRE(capacity == count);
    return keys;
}

// Every surface brick's stored fp16 payload, keyed by brick coordinate: the
// bits the locality guarantee is about.
std::map<std::tuple<int, int, int>, std::vector<std::uint16_t>> snapshot(
    const clay_brick_cache* cache) {
    std::vector<std::int32_t> keys = surface_keys(cache);
    std::size_t count = keys.size() / 3;
    std::map<std::tuple<int, int, int>, std::vector<std::uint16_t>> out;
    if (count == 0) return out;
    std::vector<std::uint16_t> halves(count * kSamples);
    std::vector<std::int32_t> states(count);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys.data(), count, 0, states.data(),
                                         halves.data(), count * kSamples, nullptr, 0) == CLAY_OK);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(states[i] == CLAY_BRICK_SURFACE);
        out[{keys[i * 3], keys[i * 3 + 1], keys[i * 3 + 2]}].assign(
            halves.begin() + static_cast<std::ptrdiff_t>(i * kSamples),
            halves.begin() + static_cast<std::ptrdiff_t>((i + 1) * kSamples));
    }
    return out;
}

bool boxes_overlap(const float amin[3], const float amax[3], const float bmin[3],
                   const float bmax[3]) {
    for (int a = 0; a < 3; ++a)
        if (amin[a] > bmax[a] || amax[a] < bmin[a]) return false;
    return true;
}

}  // namespace

TEST_CASE("brick config: defaults, round trip and every refusal") {
    clay_brick_config d;
    REQUIRE(clay_brick_config_defaults(&d) == CLAY_OK);
    CHECK(d.struct_size == sizeof(clay_brick_config));
    CHECK(d.dim == 8);
    CHECK(d.voxel_size == doctest::Approx(0.05f));
    CHECK(d.band_voxels == 3);
    CHECK(d.memory_budget == 0);  // 0 is unlimited, exactly as the engine has it
    CHECK(clay_brick_config_defaults(nullptr) == CLAY_ERROR_INVALID_ARGUMENT);

    Cache cache(clay_brick_cache_create(&d));
    clay_brick_config back;
    std::memset(&back, 0xAB, sizeof back);
    back.struct_size = static_cast<std::uint32_t>(sizeof back);
    REQUIRE(clay_brick_cache_config(cache, &back) == CLAY_OK);
    CHECK(back.dim == d.dim);
    CHECK(back.voxel_size == d.voxel_size);
    CHECK(back.band_voxels == d.band_voxels);
    CHECK(back.memory_budget == d.memory_budget);

    SUBCASE("a zeroed descriptor is a mistake, not a request") {
        clay_brick_config zero;
        std::memset(&zero, 0, sizeof zero);
        CHECK(clay_brick_cache_create(&zero) == nullptr);
        CHECK(std::strlen(clay_last_error()) > 0);
    }
    SUBCASE("struct_size below the original layout") {
        clay_brick_config c = config_of(8, 0.05f, 3, 0);
        c.struct_size = 4;
        CHECK(clay_brick_cache_create(&c) == nullptr);
    }
    SUBCASE("struct_size no descriptor could have") {
        clay_brick_config c = config_of(8, 0.05f, 3, 0);
        c.struct_size = 0x3D23D70Au;
        CHECK(clay_brick_cache_create(&c) == nullptr);
    }
    SUBCASE("dim is 8 or 16 and nothing else") {
        for (int dim : {0, 1, 7, 9, 12, 32, -8}) {
            clay_brick_config c = config_of(dim, 0.05f, 3, 0);
            CHECK(clay_brick_cache_create(&c) == nullptr);
        }
        for (int dim : {8, 16}) {
            clay_brick_config c = config_of(dim, 0.05f, 3, 0);
            clay_brick_cache* h = clay_brick_cache_create(&c);
            CHECK(h != nullptr);
            clay_brick_cache_destroy(h);
        }
    }
    SUBCASE("voxel size must be finite and > 0") {
        for (float v : {0.0f, -0.1f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
            clay_brick_config c = config_of(8, v, 3, 0);
            CHECK(clay_brick_cache_create(&c) == nullptr);
        }
    }
    SUBCASE("band must be at least one voxel") {
        for (int b : {0, -1}) {
            clay_brick_config c = config_of(8, 0.05f, b, 0);
            CHECK(clay_brick_cache_create(&c) == nullptr);
        }
    }
    SUBCASE("null handles are refused, and destroying null is a no-op") {
        CHECK(clay_brick_cache_create(nullptr) == nullptr);
        clay_brick_cache_destroy(nullptr);
        clay_brick_config c;
        c.struct_size = static_cast<std::uint32_t>(sizeof c);
        CHECK(clay_brick_cache_config(nullptr, &c) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_config(cache, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("influence bounds: three states through two flags") {
    Doc doc;
    float lo[3], hi[3];
    std::int32_t has = -1, infinite = -1;

    SUBCASE("an empty layer shows nothing") {
        REQUIRE(clay_layer_influence_bound(doc.d, doc.layer, lo, hi, &has, &infinite) == CLAY_OK);
        CHECK(has == 0);
        CHECK(infinite == 0);
    }

    SUBCASE("a local item's bound covers it, dilated") {
        clay_node_id id = add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
        REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, id, lo, hi, &has, &infinite) ==
                CLAY_OK);
        REQUIRE(has == 1);
        CHECK(infinite == 0);
        CHECK(lo[0] <= -0.4f);
        CHECK(hi[0] >= 0.4f);
        // ...and the layer's union agrees for a one-item layer
        float llo[3], lhi[3];
        std::int32_t lhas = 0, linf = 0;
        REQUIRE(clay_layer_influence_bound(doc.d, doc.layer, llo, lhi, &lhas, &linf) == CLAY_OK);
        CHECK(lhas == 1);
        CHECK(linf == 0);
        for (int a = 0; a < 3; ++a) {
            CHECK(llo[a] == doctest::Approx(lo[a]));
            CHECK(lhi[a] == doctest::Approx(hi[a]));
        }
    }

    SUBCASE("a non-local op has no finite influence and says so") {
        add_sphere(doc, 0.5f, 0.0f, 0.0f, 0.0f);
        clay_node_id cut = add_sphere(doc, 0.3f, 0.2f, 0.0f, 0.0f, CLAY_OP_INTERSECT);
        lo[0] = hi[0] = 1234.5f;
        REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, cut, lo, hi, &has, &infinite) ==
                CLAY_OK);
        CHECK(infinite == 1);
        CHECK(has == 1);
        CHECK(lo[0] == 1234.5f);  // an unbounded answer writes no box
        CHECK(hi[0] == 1234.5f);
        // the whole layer inherits it
        REQUIRE(clay_layer_influence_bound(doc.d, doc.layer, lo, hi, &has, &infinite) == CLAY_OK);
        CHECK(infinite == 1);
    }

    SUBCASE("an unbounded primitive is unbounded too") {
        const float plane[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        clay_item* it = clay_item_create(CLAY_PRIM_PLANE, plane, 4);
        REQUIRE(it != nullptr);
        clay_node_id id = 0;
        REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
        clay_item_destroy(it);
        REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, id, lo, hi, &has, &infinite) ==
                CLAY_OK);
        CHECK(infinite == 1);
    }

    SUBCASE("a node the layer does not hold contributes nothing, and is not an error") {
        add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
        REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, 99999, lo, hi, &has,
                                                &infinite) == CLAY_OK);
        CHECK(has == 0);
        CHECK(infinite == 0);
    }

    SUBCASE("a layer the document does not hold is NOT_FOUND") {
        CHECK(clay_layer_node_influence_bound(doc.d, 4242, 1, lo, hi, &has, &infinite) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(clay_layer_influence_bound(doc.d, 4242, lo, hi, &has, &infinite) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(clay_layer_influence_bound(nullptr, 1, lo, hi, &has, &infinite) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("the influence bound is wider than the camera bound where a blend needs it") {
        // A group blended over its children dilates by its own blend support;
        // clay_layer_bounds is deliberately tight and does not.
        add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
        float tight_lo[3], tight_hi[3];
        std::int32_t tight_has = 0;
        REQUIRE(clay_layer_bounds(doc.d, doc.layer, tight_lo, tight_hi, &tight_has) == CLAY_OK);
        REQUIRE(clay_layer_influence_bound(doc.d, doc.layer, lo, hi, &has, &infinite) == CLAY_OK);
        REQUIRE(tight_has == 1);
        REQUIRE(has == 1);
        for (int a = 0; a < 3; ++a) {  // never tighter than the box to frame on
            CHECK(lo[a] <= tight_lo[a] + 1e-5f);
            CHECK(hi[a] >= tight_hi[a] - 1e-5f);
        }
    }
}

TEST_CASE("clay_eval_grid: a culled grid answers what an unculled one does") {
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    add_sphere(doc, 0.2f, 3.0f, 0.0f, 0.0f);  // far away: what culling drops

    clay_grid_query q;
    std::memset(&q, 0, sizeof q);
    q.struct_size = static_cast<std::uint32_t>(sizeof q);
    q.origin[0] = q.origin[1] = q.origin[2] = -0.2f;
    q.spacing = 0.1f;
    q.dims[0] = q.dims[1] = q.dims[2] = 5;
    const std::size_t n = 125;

    std::vector<float> whole(n), culled(n), colors(n * 3);
    REQUIRE(clay_eval_grid(doc.d, nullptr, &q, nullptr, nullptr, whole.data(), colors.data(),
                           n) == CLAY_OK);
    const float rmin[3] = {-0.25f, -0.25f, -0.25f};
    const float rmax[3] = {0.25f, 0.25f, 0.25f};
    REQUIRE(clay_eval_grid(doc.d, nullptr, &q, rmin, rmax, culled.data(), nullptr, n) == CLAY_OK);
    for (std::size_t i = 0; i < n; ++i) CHECK(culled[i] == whole[i]);

    // ...and the grid is the same field clay_eval_points reports, in x-fastest
    // order, which is the claim a host's mesher rests on
    std::vector<float> points(n * 3), expected(n);
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 5; ++j)
            for (int i = 0; i < 5; ++i) {
                std::size_t idx = static_cast<std::size_t>((k * 5 + j) * 5 + i);
                points[idx * 3 + 0] = q.origin[0] + q.spacing * static_cast<float>(i);
                points[idx * 3 + 1] = q.origin[1] + q.spacing * static_cast<float>(j);
                points[idx * 3 + 2] = q.origin[2] + q.spacing * static_cast<float>(k);
            }
    REQUIRE(clay_eval_points(doc.d, nullptr, points.data(), n, expected.data(), nullptr) ==
            CLAY_OK);
    for (std::size_t i = 0; i < n; ++i) CHECK(whole[i] == doctest::Approx(expected[i]).epsilon(1e-5));

    SUBCASE("every refusal") {
        std::vector<float> out(n);
        CHECK(clay_eval_grid(nullptr, nullptr, &q, nullptr, nullptr, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid(doc.d, nullptr, nullptr, nullptr, nullptr, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid(doc.d, nullptr, &q, nullptr, nullptr, nullptr, nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // the count is required to be exact: it is the one argument the
        // boundary cannot check against the caller's memory
        CHECK(clay_eval_grid(doc.d, nullptr, &q, nullptr, nullptr, out.data(), nullptr, n - 1) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid(doc.d, nullptr, &q, nullptr, nullptr, out.data(), nullptr, n + 1) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // one region bound without the other
        CHECK(clay_eval_grid(doc.d, nullptr, &q, rmin, nullptr, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_eval_grid(doc.d, nullptr, &q, nullptr, rmax, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // an empty region, and one carrying an infinity
        CHECK(clay_eval_grid(doc.d, nullptr, &q, rmax, rmin, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        const float inf_min[3] = {-std::numeric_limits<float>::infinity(), -1.0f, -1.0f};
        CHECK(clay_eval_grid(doc.d, nullptr, &q, inf_min, rmax, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // an unknown backend
        CHECK(clay_eval_grid(doc.d, "nope", &q, nullptr, nullptr, out.data(), nullptr, n) ==
              CLAY_ERROR_NOT_FOUND);
        // a descriptor that declares nothing
        clay_grid_query bad = q;
        bad.struct_size = 0;
        CHECK(clay_eval_grid(doc.d, nullptr, &bad, nullptr, nullptr, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // a lattice that is not one
        for (int axis = 0; axis < 3; ++axis) {
            clay_grid_query z = q;
            z.dims[axis] = 0;
            CHECK(clay_eval_grid(doc.d, nullptr, &z, nullptr, nullptr, out.data(), nullptr, 0) ==
                  CLAY_ERROR_INVALID_ARGUMENT);
        }
        for (float s : {0.0f, -0.1f, std::numeric_limits<float>::infinity()}) {
            clay_grid_query z = q;
            z.spacing = s;
            CHECK(clay_eval_grid(doc.d, nullptr, &z, nullptr, nullptr, out.data(), nullptr, n) ==
                  CLAY_ERROR_INVALID_ARGUMENT);
        }
        // a dims product above the batch limit, refused before it sizes anything
        clay_grid_query huge = q;
        huge.dims[0] = huge.dims[1] = huge.dims[2] = 2048;  // 8.6e9 samples
        CHECK(clay_eval_grid(doc.d, nullptr, &huge, nullptr, nullptr, out.data(), nullptr, n) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("mark_dirty: the span guard runs before anything is inserted") {
    // BrickCache::mark_dirty casts floor(region / brick_size) to int with no
    // range check and inserts a tracked entry per brick in the span. Three
    // caller floats name 1e19 of them, which is undefined behaviour on the
    // cast and an unbounded allocation on the insert — and under
    // -fno-exceptions the bad_alloc terminates the host rather than returning.
    Cache cache(make_cache());
    const float ok_min[3] = {-0.4f, -0.4f, -0.4f};
    const float ok_max[3] = {0.4f, 0.4f, 0.4f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, ok_min, ok_max) == CLAY_OK);
    const std::uint64_t tracked = stats_of(cache).tracked_bricks;
    REQUIRE(tracked > 0);

    auto refused = [&](const float lo[3], const float hi[3]) {
        CHECK(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_ERROR_INVALID_ARGUMENT);
        // the cache is left exactly as it was
        CHECK(stats_of(cache).tracked_bricks == tracked);
    };

    SUBCASE("a region spanning more bricks than the batch limit") {
        const float lo[3] = {-1e6f, -1e6f, -1e6f};
        const float hi[3] = {1e6f, 1e6f, 1e6f};
        refused(lo, hi);
    }
    SUBCASE("a brick coordinate that would not fit in an int32") {
        const float lo[3] = {-1e30f, -0.1f, -0.1f};
        const float hi[3] = {-1e29f, 0.1f, 0.1f};
        refused(lo, hi);
    }
    SUBCASE("a region carrying an infinity is refused, not read as 'everything'") {
        const float lo[3] = {-std::numeric_limits<float>::infinity(), -1.0f, -1.0f};
        const float hi[3] = {1.0f, 1.0f, 1.0f};
        refused(lo, hi);
        const float nan_hi[3] = {std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f};
        refused(ok_min, nan_hi);
    }
    SUBCASE("an empty region") { refused(ok_max, ok_min); }
    SUBCASE("one bound without the other") {
        CHECK(clay_brick_cache_mark_dirty(cache, ok_min, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_mark_dirty(cache, nullptr, ok_max) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(stats_of(cache).tracked_bricks == tracked);
    }
    SUBCASE("both NULL is the only way to say 'dirty everything tracked'") {
        Doc doc;
        add_sphere(doc, 0.3f, 0.0f, 0.0f, 0.0f);
        refill(cache, doc.d);
        REQUIRE(stats_of(cache).dirty_bricks == 0);
        REQUIRE(clay_brick_cache_mark_dirty(cache, nullptr, nullptr) == CLAY_OK);
        clay_brick_stats s = stats_of(cache);
        CHECK(s.dirty_bricks == s.tracked_bricks);
        CHECK(s.tracked_bricks == tracked);  // it dirties, it does not track more
    }
    SUBCASE("a null cache") {
        CHECK(clay_brick_cache_mark_dirty(nullptr, ok_min, ok_max) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_mark_dirty(nullptr, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("the documented host loop fills a cache and the bricks describe the field") {
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    Cache cache(make_cache());

    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    clay_brick_stats before = stats_of(cache);
    CHECK(before.dirty_bricks > 0);
    CHECK(before.surface_bricks == 0);
    CHECK(before.memory_usage == 0);

    Refill r = refill(cache, doc.d, 7);  // a chunk that does not divide the count
    CHECK(r.requested == before.dirty_bricks);
    CHECK(r.accepted == r.requested);
    CHECK(r.stale == 0);
    CHECK(r.over_budget == 0);

    clay_brick_stats after = stats_of(cache);
    CHECK(after.dirty_bricks == 0);
    CHECK(after.tracked_bricks == before.tracked_bricks);
    CHECK(after.surface_bricks > 0);
    // empty space costs nothing: most bricks of a sphere in its own bounding
    // box are implicit
    CHECK(after.surface_bricks < after.tracked_bricks);
    CHECK(after.memory_usage == after.surface_bricks * kSamples * sizeof(std::uint16_t));
    CHECK(after.memory_budget == 0);

    SUBCASE("a sampled brick agrees with the field it was filled from") {
        std::vector<std::int32_t> keys = surface_keys(cache);
        REQUIRE(keys.size() >= 3);
        const std::int32_t key[3] = {keys[0], keys[1], keys[2]};
        float lo[3], hi[3];
        REQUIRE(clay_brick_cache_brick_bounds(cache, key, lo, hi) == CLAY_OK);
        const float band = 3 * kVoxel;
        for (int k = 0; k < kDim; k += 3)
            for (int j = 0; j < kDim; j += 3)
                for (int i = 0; i < kDim; ++i) {
                    float cached = 0.0f;
                    REQUIRE(clay_brick_cache_sample(cache, key, i, j, k, &cached) == CLAY_OK);
                    const float p[3] = {lo[0] + kVoxel * static_cast<float>(i),
                                        lo[1] + kVoxel * static_cast<float>(j),
                                        lo[2] + kVoxel * static_cast<float>(k)};
                    float truth = 0.0f;
                    REQUIRE(clay_eval_points(doc.d, nullptr, p, 1, &truth, nullptr) == CLAY_OK);
                    truth = truth < -band ? -band : (truth > band ? band : truth);
                    CHECK(cached == doctest::Approx(truth).epsilon(0.01));
                }
    }

    SUBCASE("the cull region is the brick dilated by the band") {
        const std::int32_t key[3] = {0, 1, -2};
        float bmin[3], bmax[3], cmin[3], cmax[3];
        REQUIRE(clay_brick_cache_brick_bounds(cache, key, bmin, bmax) == CLAY_OK);
        REQUIRE(clay_brick_cache_cull_region(cache, key, cmin, cmax) == CLAY_OK);
        const float bs = kDim * kVoxel;
        const float band = 3 * kVoxel;
        CHECK(bmin[0] == doctest::Approx(0.0f));
        CHECK(bmax[2] == doctest::Approx(-2 * bs + bs));
        for (int a = 0; a < 3; ++a) {
            CHECK(cmin[a] == doctest::Approx(bmin[a] - band));
            CHECK(cmax[a] == doctest::Approx(bmax[a] + band));
        }
    }

    SUBCASE("meshing and raycasting the bricks") {
        clay_brick_mesh_params p;
        std::memset(&p, 0, sizeof p);
        p.struct_size = static_cast<std::uint32_t>(sizeof p);
        p.normals = CLAY_NORMAL_GRADIENT;
        p.colors = 1;
        clay_mesh* m = nullptr;
        REQUIRE(clay_brick_cache_mesh(cache, doc.d, &p, nullptr, 0, nullptr, &m) == CLAY_OK);
        REQUIRE(m != nullptr);
        CHECK(clay_mesh_vertex_count(m) > 0);
        CHECK(clay_mesh_index_count(m) > 0);
        CHECK(clay_mesh_colors(m) != nullptr);
        clay_mesh_destroy(m);

        // ...and without a document, which is the no-attributes path
        clay_brick_mesh_params bare = p;
        bare.normals = CLAY_NORMAL_FACE;
        bare.colors = 0;
        clay_mesh* plain = nullptr;
        REQUIRE(clay_brick_cache_mesh(cache, nullptr, &bare, nullptr, 0, nullptr, &plain) ==
                CLAY_OK);
        CHECK(clay_mesh_vertex_count(plain) > 0);
        clay_mesh_destroy(plain);

        // gradient normals and colours are attributes of the FIELD: asking for
        // either with no document is refused, not quietly downgraded
        CHECK(clay_brick_cache_mesh(cache, nullptr, &p, nullptr, 0, nullptr, &plain) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        clay_brick_mesh_params unknown = p;
        unknown.normals = 77;
        CHECK(clay_brick_cache_mesh(cache, doc.d, &unknown, nullptr, 0, nullptr, &plain) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        unknown.normals = CLAY_NORMAL_FACE;
        unknown.struct_size = 4;
        CHECK(clay_brick_cache_mesh(cache, doc.d, &unknown, nullptr, 0, nullptr, &plain) ==
              CLAY_ERROR_INVALID_ARGUMENT);

        const float origin[3] = {0.0f, 0.0f, 2.0f};
        const float dir[3] = {0.0f, 0.0f, -3.0f};  // normalized inside
        std::int32_t hit = 0;
        float t = 0.0f, pos[3], nrm[3];
        REQUIRE(clay_brick_cache_raycast(cache, origin, dir, &hit, &t, pos, nrm) == CLAY_OK);
        CHECK(hit == 1);
        CHECK(pos[2] == doctest::Approx(0.4f).epsilon(0.05));
        CHECK(nrm[2] > 0.8f);

        const float away[3] = {0.0f, 1.0f, 0.0f};
        std::int32_t miss = 1;
        REQUIRE(clay_brick_cache_raycast(cache, origin, away, &miss, nullptr, nullptr,
                                         nullptr) == CLAY_OK);
        CHECK(miss == 0);  // no hit is not an error
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        CHECK(clay_brick_cache_raycast(cache, origin, zero, &hit, nullptr, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("read_bricks: a fixed stride, uniform bricks filled, missing ones untouched") {
    Doc doc;
    // Wider than one brick (0.4 world units here), so the ball has an interior
    // brick the cache can represent implicitly.
    add_sphere(doc, 1.0f, 0.0f, 0.0f, 0.0f);
    Cache cache(make_cache());
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    refill(cache, doc.d);

    // one surface brick, one implicit interior brick, one the cache never saw
    std::vector<std::int32_t> surface = surface_keys(cache);
    REQUIRE(surface.size() >= 3);
    const std::int32_t keys[9] = {surface[0], surface[1], surface[2],
                                  -1,         -1,         -1,
                                  500,        500,        500};
    std::vector<std::int32_t> states(3, -1);
    std::vector<std::uint16_t> halves(3 * kSamples, 0xBEEF);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys, 3, 0, states.data(), halves.data(),
                                         3 * kSamples, nullptr, 0) == CLAY_OK);
    CHECK(states[0] == CLAY_BRICK_SURFACE);
    CHECK(states[1] == CLAY_BRICK_INSIDE);
    CHECK(states[2] == CLAY_BRICK_MISSING);

    // the uniform brick is FILLED with the band value of its sign, so an
    // uploader never branches on the state...
    const std::uint16_t inside = halves[kSamples];
    for (std::size_t i = 0; i < kSamples; ++i) CHECK(halves[kSamples + i] == inside);
    float decoded = 0.0f;
    const std::int32_t interior[3] = {-1, -1, -1};
    REQUIRE(clay_brick_cache_sample(cache, interior, 0, 0, 0, &decoded) == CLAY_OK);
    CHECK(decoded == doctest::Approx(-3 * kVoxel));
    // ...and the missing one is the single state that leaves the buffer alone
    for (std::size_t i = 0; i < kSamples; ++i) CHECK(halves[2 * kSamples + i] == 0xBEEF);

    // The decoded and the stored form describe the same brick: every sample is
    // inside the band, equal halves decode equal and a re-read is bit-stable.
    const float band = 3 * kVoxel;
    std::vector<std::uint16_t> again(kSamples, 0);
    std::int32_t again_state = -1;
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys, 1, 0, &again_state, again.data(), kSamples,
                                         nullptr, 0) == CLAY_OK);
    for (std::size_t i = 0; i < kSamples; ++i) CHECK(again[i] == halves[i]);
    std::map<std::uint16_t, float> decoded_by_bits;
    for (int k = 0; k < kDim; ++k)
        for (int j = 0; j < kDim; ++j)
            for (int i = 0; i < kDim; ++i) {
                float v = 0.0f;
                REQUIRE(clay_brick_cache_sample(cache, keys, i, j, k, &v) == CLAY_OK);
                CHECK(v >= -band - 1e-4f);
                CHECK(v <= band + 1e-4f);
                const std::size_t idx =
                    (static_cast<std::size_t>(k) * kDim + static_cast<std::size_t>(j)) * kDim +
                    static_cast<std::size_t>(i);
                auto [it, inserted] = decoded_by_bits.emplace(halves[idx], v);
                if (!inserted) CHECK(it->second == v);  // same bits, same value
            }

    SUBCASE("every refusal") {
        std::vector<std::uint16_t> buf(kSamples);
        std::int32_t one_state = 0;
        CHECK(clay_brick_cache_read_bricks(nullptr, 0, keys, 1, 0, &one_state, nullptr, 0, nullptr,
                                           0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_read_bricks(cache, 0, nullptr, 1, 0, &one_state, nullptr, 0, nullptr,
                                           0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_read_bricks(cache, 0, keys, 1, 0, nullptr, nullptr, 0, nullptr, 0) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // an lod above the one level that exists is rejected, not clamped
        for (std::int32_t lod : {2, 3, -1}) {
            CHECK(clay_brick_cache_read_bricks(cache, lod, keys, 1, 0, &one_state, nullptr, 0,
                                               nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
        }
        // the stride is fixed, so the capacity is exact
        CHECK(clay_brick_cache_read_bricks(cache, 0, keys, 1, 0, &one_state, buf.data(),
                                           kSamples - 1, nullptr,
                                           0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_read_bricks(cache, 0, keys, 1, 0, &one_state, buf.data(),
                                           kSamples + 1, nullptr,
                                           0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_read_bricks(cache, 0, keys, 1, 0, &one_state, nullptr, kSamples,
                                           nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("sample refuses a lattice index outside the brick") {
        float v = 0.0f;
        CHECK(clay_brick_cache_sample(cache, keys, kDim, 0, 0, &v) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_sample(cache, keys, 0, -1, 0, &v) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_sample(cache, keys, 0, 0, kDim, &v) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_sample(cache, keys, 0, 0, 0, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("surface_bricks is the section's one size query") {
        std::size_t count = 0;
        REQUIRE(clay_brick_cache_surface_bricks(cache, nullptr, &count) == CLAY_OK);
        REQUIRE(count > 1);
        std::vector<std::int32_t> small(count * 3);
        std::size_t capacity = count - 1;
        CHECK(clay_brick_cache_surface_bricks(cache, small.data(), &capacity) ==
              CLAY_ERROR_BUFFER_TOO_SMALL);
        CHECK(capacity == count);  // and it reports what was needed
        CHECK(clay_brick_cache_surface_bricks(cache, small.data(), &capacity) == CLAY_OK);
        CHECK(capacity == count);
        CHECK(clay_brick_cache_surface_bricks(cache, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("take_dirty is capacity-in/count-out, and never a size query") {
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    Cache cache(make_cache());
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    const std::uint64_t pending = stats_of(cache).dirty_bricks;
    REQUIRE(pending > 4);

    clay_brick_request one;
    std::size_t count = 1, remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, &one, &count, &remaining) == CLAY_OK);
    CHECK(count == 1);
    CHECK(remaining == pending - 1);
    // the request carries the lattice, and it is the configuration's
    CHECK(one.spacing == kVoxel);
    CHECK(one.dims[0] == kDim);
    CHECK(one.dims[1] == kDim);
    CHECK(one.dims[2] == kDim);
    float lo[3], hi[3];
    REQUIRE(clay_brick_cache_brick_bounds(cache, one.key, lo, hi) == CLAY_OK);
    for (int a = 0; a < 3; ++a) CHECK(one.origin[a] == lo[a]);
    // ...and the pending count the stats report includes what is staged
    CHECK(stats_of(cache).dirty_bricks == pending - 1);

    SUBCASE("draining the rest reaches zero, and asking again yields nothing") {
        std::vector<clay_brick_request> rest(pending);
        std::size_t capacity = rest.size();
        REQUIRE(clay_brick_cache_take_dirty(cache, rest.data(), &capacity, &remaining) ==
                CLAY_OK);
        CHECK(capacity == pending - 1);
        CHECK(remaining == 0);
        capacity = rest.size();
        REQUIRE(clay_brick_cache_take_dirty(cache, rest.data(), &capacity, &remaining) ==
                CLAY_OK);
        CHECK(capacity == 0);
        CHECK(remaining == 0);
    }

    SUBCASE("a NULL buffer is a mistake, not a size query") {
        std::size_t capacity = 4;
        CHECK(clay_brick_cache_take_dirty(cache, nullptr, &capacity, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(stats_of(cache).dirty_bricks == pending - 1);  // and it drained nothing more
        CHECK(clay_brick_cache_take_dirty(cache, &one, nullptr, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_take_dirty(nullptr, &one, &capacity, nullptr) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("out_remaining is optional") {
        std::size_t capacity = 1;
        REQUIRE(clay_brick_cache_take_dirty(cache, &one, &capacity, nullptr) == CLAY_OK);
        CHECK(capacity == 1);
    }
}

TEST_CASE("generations: a result computed against the older scene is rejected") {
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    Cache cache(make_cache());
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);

    std::vector<clay_brick_request> inflight(8);
    std::size_t count = inflight.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, inflight.data(), &count, &remaining) == CLAY_OK);
    REQUIRE(count > 0);
    inflight.resize(count);

    std::vector<float> values(count * kSamples);
    REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, inflight.data(), count, values.data(),
                                           count * kSamples, nullptr, 0) == CLAY_OK);

    // the brick is re-dirtied while the request is in flight
    float lo[3], hi[3];
    REQUIRE(clay_brick_cache_brick_bounds(cache, inflight[0].key, lo, hi) == CLAY_OK);
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    std::vector<std::int32_t> results(count, -1);
    std::size_t accepted = 0;
    REQUIRE(clay_brick_cache_submit(cache, inflight.data(), count, values.data(), count * kSamples,
                                    nullptr, 0, results.data(), &accepted) == CLAY_OK);
    CHECK(results[0] == CLAY_BRICK_SUBMIT_STALE);  // an ordinary outcome, and CLAY_OK
    // Re-dirtying reaches the brick's band-dilated neighbours too, so the
    // assertion is the accounting rather than a fixed number: every request is
    // either accepted or stale, and at least the one re-dirtied is stale.
    std::size_t stale = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (results[i] == CLAY_BRICK_SUBMIT_STALE) ++stale;
    CHECK(stale >= 1);
    CHECK(accepted + stale == count);
    CHECK(accepted < count);

    // the NEW request for that brick is accepted
    Refill r = refill(cache, doc.d);
    CHECK(r.accepted > 0);
    CHECK(stats_of(cache).dirty_bricks == 0);

    SUBCASE("submit's own refusals") {
        std::size_t got = 0;
        std::vector<std::int32_t> res(1);
        CHECK(clay_brick_cache_submit(nullptr, inflight.data(), 1, values.data(), kSamples, nullptr,
                                      0, res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_submit(cache, nullptr, 1, values.data(), kSamples, nullptr, 0,
                                      res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_submit(cache, inflight.data(), 1, nullptr, kSamples, nullptr, 0,
                                      res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        // a caller that skips both outputs cannot tell what happened
        CHECK(clay_brick_cache_submit(cache, inflight.data(), 1, values.data(), kSamples, nullptr,
                                      0, nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        // the capacity must be exactly count * dim^3
        CHECK(clay_brick_cache_submit(cache, inflight.data(), 1, values.data(), kSamples - 1,
                                      nullptr, 0, res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_submit(cache, inflight.data(), 1, values.data(), kSamples * 2,
                                      nullptr, 0, res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        // a request whose lattice is not this cache's was modified, or came
        // from another cache
        clay_brick_request tampered = inflight[0];
        tampered.spacing *= 2.0f;
        CHECK(clay_brick_cache_submit(cache, &tampered, 1, values.data(), kSamples, nullptr, 0,
                                      res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        tampered = inflight[0];
        tampered.dims[1] = 16;
        CHECK(clay_brick_cache_submit(cache, &tampered, 1, values.data(), kSamples, nullptr, 0,
                                      res.data(), &got) == CLAY_ERROR_INVALID_ARGUMENT);
        // ...but a submission to a cache that never issued it is merely STALE
        Cache sibling(make_cache());
        std::int32_t sibling_result = -1;
        REQUIRE(clay_brick_cache_submit(sibling, inflight.data(), 1, values.data(), kSamples,
                                        nullptr, 0, &sibling_result, nullptr) == CLAY_OK);
        CHECK(sibling_result == CLAY_BRICK_SUBMIT_STALE);
    }

    SUBCASE("eval_requests' own refusals") {
        std::vector<float> out(kSamples);
        CHECK(clay_brick_cache_eval_requests(nullptr, nullptr, inflight.data(), 1, out.data(),
                                             kSamples, nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_eval_requests(doc.d, nullptr, nullptr, 1, out.data(), kSamples,
                                             nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_eval_requests(doc.d, nullptr, inflight.data(), 1, nullptr, kSamples,
                                             nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_eval_requests(doc.d, nullptr, inflight.data(), 1, out.data(),
                                             kSamples - 1, nullptr,
                                             0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_eval_requests(doc.d, "nope", inflight.data(), 1, out.data(),
                                             kSamples, nullptr, 0) == CLAY_ERROR_NOT_FOUND);
        CHECK(clay_brick_cache_eval_requests(doc.d, nullptr, inflight.data(), 0, out.data(), 4,
                                             nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_eval_requests(doc.d, nullptr, inflight.data(), 0, nullptr, 0,
                                             nullptr, 0) == CLAY_OK);
        if (inflight.size() > 1) {
            std::vector<clay_brick_request> mixed = {inflight[0], inflight[1]};
            mixed[1].dims[0] = 16;  // two lattices in one batch have no stride
            CHECK(clay_brick_cache_eval_requests(doc.d, nullptr, mixed.data(), 2, out.data(),
                                                 2 * kSamples, nullptr,
                                                 0) == CLAY_ERROR_INVALID_ARGUMENT);
        }
    }
}

TEST_CASE("eval_requests: every registered backend answers what the CPU answers, batched") {
    // Regression for issue #64's fix: the request batch reaches the backend as
    // batched evaluations rather than one backend call per brick, which is
    // what lets a GPU backend amortize its dispatch. The values must not
    // notice: whatever backend name crosses the boundary, a batched refill
    // produces the CPU path's field within GPU parity tolerance (1e-4, the
    // evaluation-backends gate). The marked region spans surface, inside,
    // outside AND far-empty bricks, so the batch carries culled tapes of very
    // different sizes — including empty ones.
    Doc doc;
    add_sphere(doc, 0.5f, 0.0f, 0.0f, 0.0f);
    Cache cache(make_cache());
    const float near_lo[3] = {-0.6f, -0.6f, -0.6f}, near_hi[3] = {0.6f, 0.6f, 0.6f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, near_lo, near_hi) == CLAY_OK);
    const float far_lo[3] = {5.0f, 5.0f, 5.0f}, far_hi[3] = {5.3f, 5.3f, 5.3f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, far_lo, far_hi) == CLAY_OK);

    std::vector<clay_brick_request> reqs(4096);
    std::size_t count = reqs.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
    REQUIRE(remaining == 0);
    REQUIRE(count > 64);  // enough bricks that per-brick overhead would show
    reqs.resize(count);

    std::vector<float> reference(count * kSamples);
    REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), count, reference.data(),
                                           reference.size(), nullptr, 0) == CLAY_OK);

    char names[256];
    std::size_t size = sizeof names;
    REQUIRE(clay_list_backends(names, &size) == CLAY_OK);
    std::string list(names);
    for (std::size_t at = 0; at < list.size();) {
        std::size_t comma = list.find(',', at);
        std::string name = list.substr(at, comma == std::string::npos ? comma : comma - at);
        at = comma == std::string::npos ? list.size() : comma + 1;
        if (name.empty() || name == "cpu") continue;
        CAPTURE(name);
        const float kPoison = -12345.678f;
        std::vector<float> got(count * kSamples, kPoison);
        REQUIRE(clay_brick_cache_eval_requests(doc.d, name.c_str(), reqs.data(), count,
                                               got.data(), got.size(), nullptr, 0) == CLAY_OK);
        float worst = 0.0f;
        for (std::size_t i = 0; i < got.size(); ++i) {
            REQUIRE(got[i] != kPoison);  // a brick nobody evaluated
            const float scale =
                std::max(std::max(std::fabs(got[i]), std::fabs(reference[i])), 1.0f);
            worst = std::max(worst, std::fabs(got[i] - reference[i]) / scale);
        }
        CHECK(worst <= 1e-4f);
    }

    SUBCASE("a batch that mixes spacings keeps each request's own lattice") {
        // dims are checked uniform across a batch; spacing is not, and each
        // request keeps its own — the batched submission must split where the
        // spacing changes rather than stretch one spacing over the batch.
        Cache coarse(make_cache(2.0f * kVoxel));
        REQUIRE(clay_brick_cache_mark_dirty(coarse, near_lo, near_hi) == CLAY_OK);
        std::vector<clay_brick_request> coarse_reqs(4096);
        std::size_t coarse_count = coarse_reqs.size();
        REQUIRE(clay_brick_cache_take_dirty(coarse, coarse_reqs.data(), &coarse_count,
                                            nullptr) == CLAY_OK);
        REQUIRE(coarse_count > 0);
        coarse_reqs.resize(coarse_count);

        std::vector<float> coarse_ref(coarse_count * kSamples);
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, coarse_reqs.data(), coarse_count,
                                               coarse_ref.data(), coarse_ref.size(), nullptr,
                                               0) == CLAY_OK);

        std::vector<clay_brick_request> mixed(reqs);
        mixed.insert(mixed.end(), coarse_reqs.begin(), coarse_reqs.end());
        std::vector<float> got(mixed.size() * kSamples);
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, mixed.data(), mixed.size(),
                                               got.data(), got.size(), nullptr, 0) == CLAY_OK);
        // Bitwise: the same backend ran the same per-request lattices.
        CHECK(std::memcmp(got.data(), reference.data(), reference.size() * sizeof(float)) == 0);
        CHECK(std::memcmp(got.data() + reference.size(), coarse_ref.data(),
                          coarse_ref.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("the incremental claim, as a COUNT: a small edit re-evaluates few bricks") {
    // The whole reason the cache exists. A full fill, then an edit far from
    // everything: the number of bricks the second refill touches is what is
    // asserted, and every brick outside the edit's influence bound must come
    // back BIT-IDENTICAL.
    Doc doc;
    add_sphere(doc, 1.0f, 0.0f, 0.0f, 0.0f);
    Cache cache(make_cache());

    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    const std::uint64_t first_pass = stats_of(cache).dirty_bricks;
    Refill full = refill(cache, doc.d, 64);
    REQUIRE(full.accepted == first_pass);
    REQUIRE(first_pass > 100);

    auto before = snapshot(cache);
    REQUIRE(before.size() > 20);

    // a small item, far from the ball
    clay_node_id id = add_sphere(doc, 0.05f, 3.0f, 3.0f, 3.0f);
    float bmin[3], bmax[3];
    std::int32_t has = 0, infinite = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, id, bmin, bmax, &has, &infinite) ==
            CLAY_OK);
    REQUIRE(has == 1);
    REQUIRE(infinite == 0);

    std::size_t marked = 0;
    REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, &id, 1, &marked) ==
            CLAY_OK);
    CHECK(marked == 1);

    const std::uint64_t second_pass = stats_of(cache).dirty_bricks;
    // THE CLAIM: the work is proportional to the edit, not to the model.
    CHECK(second_pass > 0);
    CHECK(second_pass < first_pass / 8);
    CHECK(second_pass <= 27);  // a 0.1-wide ball at a 0.4 brick size

    Refill incremental = refill(cache, doc.d);
    CHECK(incremental.requested == second_pass);
    CHECK(incremental.accepted == second_pass);

    // ...and everything the edit could not reach is bit-identical
    auto after = snapshot(cache);
    int compared = 0;
    for (const auto& [key, values] : before) {
        const std::int32_t k[3] = {std::get<0>(key), std::get<1>(key), std::get<2>(key)};
        float cmin[3], cmax[3];
        REQUIRE(clay_brick_cache_cull_region(cache, k, cmin, cmax) == CLAY_OK);
        if (boxes_overlap(cmin, cmax, bmin, bmax)) continue;
        auto it = after.find(key);
        REQUIRE(it != after.end());
        CHECK(it->second == values);  // fp16 payload, bit for bit
        ++compared;
    }
    CHECK(compared > 20);

    SUBCASE("mark_dirty_nodes ignores ids the layer does not hold") {
        const clay_node_id absent[2] = {99999, 88888};
        std::size_t count = 7;
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, absent, 2, &count) ==
                CLAY_OK);
        CHECK(count == 0);
        CHECK(stats_of(cache).dirty_bricks == 0);
    }
    SUBCASE("mark_dirty_nodes and _layer refuse what they cannot address") {
        std::size_t count = 0;
        CHECK(clay_brick_cache_mark_dirty_nodes(cache, doc.d, 4242, &id, 1, &count) ==
              CLAY_ERROR_NOT_FOUND);
        CHECK(clay_brick_cache_mark_dirty_nodes(nullptr, doc.d, doc.layer, &id, 1, &count) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_mark_dirty_nodes(cache, nullptr, doc.layer, &id, 1, &count) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, nullptr, 1, &count) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_mark_dirty_layer(cache, doc.d, 4242) == CLAY_ERROR_NOT_FOUND);
        CHECK(clay_brick_cache_mark_dirty_layer(nullptr, doc.d, doc.layer) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
    SUBCASE("an infinite influence dirties everything tracked, and nothing more") {
        clay_brick_stats s = stats_of(cache);
        clay_node_id cut = add_sphere(doc, 0.5f, 0.0f, 0.0f, 0.0f, CLAY_OP_INTERSECT);
        std::int32_t node_has = 0, node_infinite = 0;
        float nlo[3], nhi[3];
        REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, cut, nlo, nhi, &node_has,
                                                &node_infinite) == CLAY_OK);
        REQUIRE(node_infinite == 1);
        std::size_t count = 0;
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, &cut, 1, &count) ==
                CLAY_OK);
        CHECK(count == 1);
        clay_brick_stats now = stats_of(cache);
        CHECK(now.dirty_bricks == s.tracked_bricks);
        CHECK(now.tracked_bricks == s.tracked_bricks);
    }
}

TEST_CASE("memory budget: a predictable refusal, a ceiling never breached") {
    Doc doc;
    add_sphere(doc, 0.5f, 0.0f, 0.0f, 0.0f);
    const std::uint64_t brick_bytes = kSamples * sizeof(std::uint16_t);
    const std::uint64_t budget = brick_bytes * 6;  // room for six surface bricks
    Cache cache(make_cache(kVoxel, budget));

    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    Refill r = refill(cache, doc.d, 4);
    CHECK(r.accepted > 0);
    CHECK(r.over_budget > 0);  // this scene needs more than six surface bricks

    clay_brick_stats s = stats_of(cache);
    CHECK(s.memory_budget == budget);
    CHECK(s.memory_usage <= budget);
    CHECK(s.surface_bricks <= 6);
    // every brick that IS stored stays valid and readable
    std::vector<std::int32_t> keys = surface_keys(cache);
    REQUIRE(keys.size() / 3 == s.surface_bricks);
    std::vector<std::int32_t> states(keys.size() / 3, -1);
    std::vector<std::uint16_t> halves(states.size() * kSamples);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys.data(), states.size(), 0, states.data(),
                                         halves.data(), states.size() * kSamples, nullptr,
                                         0) == CLAY_OK);
    for (std::int32_t state : states) CHECK(state == CLAY_BRICK_SURFACE);
    // the tracked bookkeeping is NOT what the budget bounds, and says so
    CHECK(s.tracked_bricks > s.surface_bricks);
}

TEST_CASE("LOD mips: built from clean children, dropped when one is dirtied") {
    Doc doc;
    add_sphere(doc, 0.6f, 0.4f, 0.4f, 0.4f);
    Cache cache(make_cache());
    const float lo[3] = {-1.0f, -1.0f, -1.0f};
    const float hi[3] = {1.6f, 1.6f, 1.6f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    refill(cache, doc.d, 64);

    const std::int32_t coarse[3] = {0, 0, 0};
    std::int32_t built = 0, lod = -1;
    REQUIRE(clay_brick_cache_build_mip(cache, coarse, &built) == CLAY_OK);
    CHECK(built == 1);
    REQUIRE(clay_brick_cache_current_lod(cache, coarse, &lod) == CLAY_OK);
    CHECK(lod == 1);

    // the mip reads back through the same call the full-resolution bricks use
    std::int32_t state = -1;
    std::vector<std::uint16_t> halves(kSamples, 0xBEEF);
    REQUIRE(clay_brick_cache_read_bricks(cache, 1, coarse, 1, 0, &state, halves.data(), kSamples,
                                         nullptr, 0) == CLAY_OK);
    CHECK(state == CLAY_BRICK_SURFACE);
    bool wrote = false;
    for (std::uint16_t h : halves) wrote = wrote || h != 0xBEEF;
    CHECK(wrote);

    // dirtying a child drops it, so a mip is never downsampled from stale data
    float bmin[3], bmax[3];
    const std::int32_t child[3] = {0, 0, 0};
    REQUIRE(clay_brick_cache_brick_bounds(cache, child, bmin, bmax) == CLAY_OK);
    REQUIRE(clay_brick_cache_mark_dirty(cache, bmin, bmax) == CLAY_OK);
    REQUIRE(clay_brick_cache_current_lod(cache, coarse, &lod) == CLAY_OK);
    CHECK(lod == 0);
    REQUIRE(clay_brick_cache_build_mip(cache, coarse, &built) == CLAY_OK);
    CHECK(built == 0);  // "not yet", not a failure
    std::int32_t missing = -1;
    REQUIRE(clay_brick_cache_read_bricks(cache, 1, coarse, 1, 0, &missing, nullptr, 0, nullptr,
                                         0) == CLAY_OK);
    CHECK(missing == CLAY_BRICK_MISSING);

    CHECK(clay_brick_cache_build_mip(nullptr, coarse, &built) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_build_mip(cache, coarse, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_current_lod(cache, coarse, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("two caches over one document are independent") {
    // The contract the header states positively: two handles share nothing, so
    // a host may keep one per viewport or per resolution.
    Doc doc;
    add_sphere(doc, 0.4f, 0.0f, 0.0f, 0.0f);
    Cache coarse(make_cache(0.1f));
    Cache fine(make_cache(0.05f));
    REQUIRE(clay_brick_cache_mark_dirty_layer(coarse, doc.d, doc.layer) == CLAY_OK);
    refill(coarse, doc.d);
    CHECK(stats_of(fine).tracked_bricks == 0);
    REQUIRE(clay_brick_cache_mark_dirty_layer(fine, doc.d, doc.layer) == CLAY_OK);
    refill(fine, doc.d);
    clay_brick_stats c = stats_of(coarse), f = stats_of(fine);
    CHECK(c.surface_bricks > 0);
    CHECK(f.surface_bricks > c.surface_bricks);  // half the voxel, more bricks
    CHECK(stats_of(coarse).dirty_bricks == 0);
}

TEST_CASE("brick cache: an item a band away from a brick still decides its samples") {
    // Regression. The evaluation path first culled each request against the
    // brick's bare world box, on the reasoning that an item whose influence
    // bound misses the box cannot change a sample inside it. That is wrong for
    // a BANDED cache: a sample stores its true distance whenever that distance
    // is within the band, so an item outside the box but within a band of it
    // decides samples inside. Culling on the bare box drops the item and the
    // brick is classified empty instead of carrying the surface's approach.
    //
    // The geometry below is chosen so the difference is unambiguous: a small
    // sphere sits 0.13 from brick (0,0,0)'s nearest sample, inside the 0.15
    // band, and entirely outside that brick's box.
    Doc doc;
    const float r = 0.02f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {0.50f, 0.20f, 0.20f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);

    // The truth, straight from the document.
    const float sample[3] = {0.35f, 0.20f, 0.20f};
    float truth = 0.0f;
    REQUIRE(clay_eval_points(doc.d, nullptr, sample, 1, &truth, nullptr) == CLAY_OK);
    REQUIRE(truth == doctest::Approx(0.13f).epsilon(1e-3));
    REQUIRE(truth < kBand);  // the premise: inside the band, so it must be stored

    Cache cache(make_cache());
    const float lo[3] = {0.40f, 0.10f, 0.10f}, hi[3] = {0.60f, 0.30f, 0.30f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    std::vector<clay_brick_request> reqs(64);
    std::size_t n = reqs.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &n, &remaining) == CLAY_OK);
    REQUIRE(n > 0);
    reqs.resize(n);

    std::vector<float> values(n * kSamples);
    REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), n, values.data(),
                                           values.size(), nullptr, 0) == CLAY_OK);

    // Find brick (0,0,0) — the one whose box the sphere is OUTSIDE — and read
    // the corner sample nearest the sphere.
    bool checked = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (reqs[i].key[0] != 0 || reqs[i].key[1] != 0 || reqs[i].key[2] != 0) continue;
        // lattice index of (0.35, 0.20, 0.20) within a brick at the origin,
        // spacing 0.05, dim 8: (7, 4, 4)
        const std::size_t idx = (static_cast<std::size_t>(4) * kDim + 4) * kDim + 7;
        CHECK(values[i * kSamples + idx] == doctest::Approx(truth).epsilon(1e-3));
        checked = true;
    }
    CHECK(checked);

    // ...and the brick is stored as a surface brick, not classified away.
    std::size_t accepted = 0;
    REQUIRE(clay_brick_cache_submit(cache, reqs.data(), n, values.data(), values.size(), nullptr, 0,
                                    nullptr, &accepted) == CLAY_OK);
    CHECK(accepted == n);
    const std::int32_t key[3] = {0, 0, 0};
    std::int32_t state = -1;
    std::vector<std::uint16_t> halves(kSamples);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, key, 1, 0, &state, halves.data(), halves.size(),
                                         nullptr, 0) == CLAY_OK);
    CHECK(state == CLAY_BRICK_SURFACE);
}
