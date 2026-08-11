#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// The brick cache as a GPU UPLOAD PATH (brick-cache spec: optional per-brick
// colour, padded readback; c-abi spec: a host uploads an atlas without
// meshing). Issue #43's finding was that read_bricks is almost what a WebGPU
// host wants and stops two steps short: it carries no colour, so a host that
// wants palette-indexed voxel colour has to mesh after all; and it writes
// exactly dim^3, so hardware trilinear filtering across a brick boundary needs
// a halo the host does not have.
//
// So this suite holds the two additions to the properties that make the call
// worth using: the stride stays FIXED whatever a brick's state, a MISSING key
// still leaves its whole slice untouched, and the halo is defined for every
// neighbour — including the implicit and never-evaluated ones at the edge of
// the sculpted region, which is where an undefined sample would actually land.

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr int kBandVoxels = 3;
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

clay_brick_cache* make_cache(bool colors, std::uint64_t budget = 0) {
    clay_brick_config c;
    REQUIRE(clay_brick_config_defaults(&c) == CLAY_OK);
    c.dim = kDim;
    c.voxel_size = kVoxel;
    c.band_voxels = kBandVoxels;
    c.memory_budget = budget;
    c.colors = colors ? 1 : 0;
    return clay_brick_cache_create(&c);
}

// A coloured sphere, so the colour lattice has something to carry that is not
// the tape's neutral seed.
void add_sphere(Doc& doc, float r, float x, float y, float z, const float rgb[3]) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, y, z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_color(it, rgb) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
}

// The documented refill loop, carrying colour when the cache wants it.
void refill(clay_brick_cache* cache, const clay_document* doc, bool colors) {
    constexpr std::size_t kChunk = 64;
    std::vector<clay_brick_request> reqs(kChunk);
    std::vector<float> values(kChunk * kSamples);
    std::vector<float> rgb(kChunk * kSamples * 3);
    std::vector<std::int32_t> results(kChunk);
    for (;;) {
        std::size_t count = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples,
                                               colors ? rgb.data() : nullptr,
                                               colors ? count * kSamples * 3 : 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(), count * kSamples,
                                        colors ? rgb.data() : nullptr,
                                        colors ? count * kSamples * 3 : 0, results.data(),
                                        &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }
}

void mark_and_fill(clay_brick_cache* cache, Doc& doc, bool colors) {
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    refill(cache, doc.d, colors);
}

std::vector<std::int32_t> surface_keys(const clay_brick_cache* cache) {
    std::size_t count = 0;
    REQUIRE(clay_brick_cache_surface_bricks(cache, nullptr, &count) == CLAY_OK);
    std::vector<std::int32_t> keys(count * 3);
    if (count == 0) return keys;
    std::size_t capacity = count;
    REQUIRE(clay_brick_cache_surface_bricks(cache, keys.data(), &capacity) == CLAY_OK);
    return keys;
}

std::size_t padded_stride(int apron) {
    const std::size_t w = static_cast<std::size_t>(kDim + 2 * apron);
    return w * w * w;
}

std::uint64_t memory_usage(const clay_brick_cache* cache) {
    clay_brick_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = static_cast<std::uint32_t>(sizeof s);
    REQUIRE(clay_brick_cache_stats(cache, &s) == CLAY_OK);
    return s.memory_usage;
}

}  // namespace

// -- colour -------------------------------------------------------------------

TEST_CASE("brick colour: the atlas carries what the field carries") {
    Doc doc;
    const float orange[3] = {0.9f, 0.4f, 0.1f};
    add_sphere(doc, 0.3f, 0, 0, 0, orange);
    Cache cache(make_cache(true));
    mark_and_fill(cache, doc, true);

    std::vector<std::int32_t> keys = surface_keys(cache);
    const std::size_t count = keys.size() / 3;
    REQUIRE(count > 0);
    std::vector<std::int32_t> states(count);
    std::vector<std::uint8_t> rgba(count * kSamples * 4);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys.data(), count, 0, states.data(), nullptr, 0,
                                         rgba.data(), rgba.size()) == CLAY_OK);

    // Against the field itself, at the lattice points, within the quantization
    // step the RGBA8 storage commits to. Compared through clay_eval_points
    // rather than through the value that was submitted, so this would catch the
    // lattice being transposed as well as it catches it being wrong.
    std::size_t checked = 0;
    for (std::size_t b = 0; b < count; ++b) {
        if (states[b] != CLAY_BRICK_SURFACE) continue;
        float origin[3] = {0, 0, 0};
        REQUIRE(clay_brick_cache_brick_bounds(cache, keys.data() + b * 3, origin, nullptr) ==
                CLAY_OK);
        for (int k = 0; k < kDim; k += 3)
            for (int j = 0; j < kDim; j += 3)
                for (int i = 0; i < kDim; i += 3) {
                    const float p[3] = {origin[0] + static_cast<float>(i) * kVoxel,
                                        origin[1] + static_cast<float>(j) * kVoxel,
                                        origin[2] + static_cast<float>(k) * kVoxel};
                    float d = 0.0f, expect[3] = {0, 0, 0};
                    REQUIRE(clay_eval_points(doc.d, nullptr, p, 1, &d, expect) == CLAY_OK);
                    const std::size_t idx =
                        b * kSamples + (static_cast<std::size_t>(k) * kDim + j) * kDim + i;
                    // Absolute, at the quantization step the RGBA8 storage
                    // commits to: one 8-bit level, plus room for the rounding.
                    for (int ch = 0; ch < 3; ++ch) {
                        const float got = static_cast<float>(rgba[idx * 4 + ch]) / 255.0f;
                        CHECK(std::fabs(got - expect[ch]) <= 1.0f / 255.0f + 1e-5f);
                    }
                    // Alpha is 255 and reserved; a host must be able to rely on
                    // that to upload rgba8unorm without a fixup pass.
                    CHECK(rgba[idx * 4 + 3] == 255);
                    ++checked;
                }
    }
    CHECK(checked > 0);
}

TEST_CASE("brick colour: a uniform brick answers without allocating a lattice") {
    Doc doc;
    const float blue[3] = {0.1f, 0.2f, 0.8f};
    // Big enough that bricks near the centre are wholly inside the band's
    // negative side and so classify uniform.
    add_sphere(doc, 0.9f, 0, 0, 0, blue);
    Cache cache(make_cache(true));
    const float lo[3] = {-1.2f, -1.2f, -1.2f}, hi[3] = {1.2f, 1.2f, 1.2f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    refill(cache, doc.d, true);

    const std::int32_t centre[3] = {0, 0, 0};
    std::int32_t state = -1;
    std::vector<std::uint8_t> rgba(kSamples * 4, 0);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, centre, 1, 0, &state, nullptr, 0, rgba.data(),
                                         rgba.size()) == CLAY_OK);
    REQUIRE(state == CLAY_BRICK_INSIDE);
    // FILLED, so an uploader never branches on the state: every texel is the
    // one value, and it is not left as the zero the buffer started at.
    for (std::size_t i = 1; i < kSamples; ++i) {
        CHECK(rgba[i * 4 + 0] == rgba[0]);
        CHECK(rgba[i * 4 + 1] == rgba[1]);
        CHECK(rgba[i * 4 + 2] == rgba[2]);
        CHECK(rgba[i * 4 + 3] == 255);
    }
    CHECK(rgba[2] > rgba[0]);  // the sphere's blue, not a neutral grey
}

TEST_CASE("brick colour: a distance-only cache is exactly the cache it was") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.3f, 0, 0, 0, grey);

    Cache plain(make_cache(false));
    mark_and_fill(plain, doc, false);
    Cache coloured(make_cache(true));
    mark_and_fill(coloured, doc, true);

    // Distance payload is identical; the colour cache holds three times the
    // bytes for it, which is the cost the header says it is.
    const std::size_t bricks = surface_keys(plain).size() / 3;
    REQUIRE(bricks > 0);
    CHECK(memory_usage(plain) == bricks * kSamples * 2);
    CHECK(memory_usage(coloured) == bricks * kSamples * 6);
    CHECK(surface_keys(coloured).size() == surface_keys(plain).size());

    SUBCASE("and it refuses to deal in colour at all") {
        std::vector<std::int32_t> keys = surface_keys(plain);
        std::int32_t state = 0;
        std::vector<std::uint8_t> rgba(kSamples * 4);
        CHECK(clay_brick_cache_read_bricks(plain, 0, keys.data(), 1, 0, &state, nullptr, 0,
                                           rgba.data(), rgba.size()) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // and refuses colours on the way IN, so the mistake is caught at the
        // call that made it rather than at the read three calls later
        clay_brick_request req{};
        std::size_t count = 1, remaining = 0;
        REQUIRE(clay_brick_cache_mark_dirty_layer(plain, doc.d, doc.layer) == CLAY_OK);
        REQUIRE(clay_brick_cache_take_dirty(plain, &req, &count, &remaining) == CLAY_OK);
        REQUIRE(count == 1);
        std::vector<float> values(kSamples, 0.0f);
        std::vector<float> rgb(kSamples * 3, 0.5f);
        std::size_t accepted = 0;
        CHECK(clay_brick_cache_submit(plain, &req, 1, values.data(), kSamples, rgb.data(),
                                      kSamples * 3, nullptr, &accepted) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }
}

TEST_CASE("brick colour: a colour cache requires colours, and the budget covers them") {
    Doc doc;
    const float red[3] = {1.0f, 0.0f, 0.0f};
    add_sphere(doc, 0.3f, 0, 0, 0, red);

    SUBCASE("submit without colours is refused, not silently greyed") {
        Cache cache(make_cache(true));
        REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
        clay_brick_request req{};
        std::size_t count = 1, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, &req, &count, &remaining) == CLAY_OK);
        REQUIRE(count == 1);
        std::vector<float> values(kSamples, 0.0f);
        std::size_t accepted = 0;
        CHECK(clay_brick_cache_submit(cache, &req, 1, values.data(), kSamples, nullptr, 0, nullptr,
                                      &accepted) == CLAY_ERROR_INVALID_ARGUMENT);
        // and the colour capacity is exact, like every other capacity here
        std::vector<float> rgb(kSamples * 3, 0.5f);
        CHECK(clay_brick_cache_submit(cache, &req, 1, values.data(), kSamples, rgb.data(),
                                      kSamples * 3 - 1, nullptr, &accepted) ==
              CLAY_ERROR_INVALID_ARGUMENT);
    }

    SUBCASE("the ceiling bounds a whole brick, not half of one") {
        // A budget that admits exactly four distance-only bricks admits ONE
        // with colour, and is never breached either way.
        const std::uint64_t budget = 4 * kSamples * 2;
        Cache plain(make_cache(false, budget));
        mark_and_fill(plain, doc, false);
        Cache coloured(make_cache(true, budget));
        mark_and_fill(coloured, doc, true);

        const std::size_t plain_bricks = surface_keys(plain).size() / 3;
        const std::size_t coloured_bricks = surface_keys(coloured).size() / 3;
        CHECK(plain_bricks == 4);
        CHECK(coloured_bricks == 1);
        CHECK(memory_usage(plain) <= budget);
        CHECK(memory_usage(coloured) <= budget);
    }
}

TEST_CASE("brick colour: a mip has none, and says so") {
    Doc doc;
    const float green[3] = {0.2f, 0.9f, 0.3f};
    add_sphere(doc, 0.6f, 0, 0, 0, green);
    Cache cache(make_cache(true));
    mark_and_fill(cache, doc, true);

    // Find a coarse key whose eight children are all evaluated and clean.
    std::vector<std::int32_t> keys = surface_keys(cache);
    REQUIRE(keys.size() >= 3);
    bool built = false;
    std::int32_t coarse[3] = {0, 0, 0};
    for (std::size_t i = 0; i < keys.size() / 3 && !built; ++i) {
        for (int a = 0; a < 3; ++a) {
            const std::int32_t k = keys[i * 3 + a];
            coarse[a] = k >= 0 ? k / 2 : (k - 1) / 2;
        }
        std::int32_t ok = 0;
        REQUIRE(clay_brick_cache_build_mip(cache, coarse, &ok) == CLAY_OK);
        built = ok != 0;
    }
    REQUIRE(built);

    std::int32_t state = -1;
    std::vector<std::uint16_t> halves(kSamples);
    REQUIRE(clay_brick_cache_read_bricks(cache, 1, coarse, 1, 0, &state, halves.data(), kSamples,
                                         nullptr, 0) == CLAY_OK);
    CHECK(state == CLAY_BRICK_SURFACE);
    // Refused rather than averaged: picking a filter across the 2x2x2 block
    // would be a policy chosen on the host's behalf.
    std::vector<std::uint8_t> rgba(kSamples * 4);
    CHECK(clay_brick_cache_read_bricks(cache, 1, coarse, 1, 0, &state, nullptr, 0, rgba.data(),
                                       rgba.size()) == CLAY_ERROR_INVALID_ARGUMENT);
}

// -- apron --------------------------------------------------------------------

TEST_CASE("brick apron: a halo equals the neighbour's own boundary plane") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.5f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);

    // Two keys adjacent on x, both surface. read them padded and unpadded, and
    // require the padded tile's halo to BE the neighbour's samples — which is
    // what makes trilinear filtering across the shared face agree with the
    // cache, and is the whole reason the apron exists.
    std::vector<std::int32_t> keys = surface_keys(cache);
    const std::size_t count = keys.size() / 3;
    std::int32_t a[3] = {0, 0, 0};
    bool found = false;
    for (std::size_t i = 0; i < count && !found; ++i) {
        const std::int32_t k[3] = {keys[i * 3], keys[i * 3 + 1], keys[i * 3 + 2]};
        const std::int32_t right[3] = {k[0] + 1, k[1], k[2]};
        std::int32_t states[2] = {-1, -1};
        const std::int32_t pair[6] = {k[0], k[1], k[2], right[0], right[1], right[2]};
        REQUIRE(clay_brick_cache_read_bricks(cache, 0, pair, 2, 0, states, nullptr, 0, nullptr,
                                             0) == CLAY_OK);
        if (states[0] == CLAY_BRICK_SURFACE && states[1] == CLAY_BRICK_SURFACE) {
            a[0] = k[0];
            a[1] = k[1];
            a[2] = k[2];
            found = true;
        }
    }
    REQUIRE(found);
    const std::int32_t b[3] = {a[0] + 1, a[1], a[2]};

    constexpr int kApron = 1;
    const std::size_t w = kDim + 2 * kApron;
    const std::size_t padded = padded_stride(kApron);
    std::vector<std::uint16_t> tile(padded, 0xBEEF);
    std::int32_t state = -1;
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, a, 1, kApron, &state, tile.data(), padded,
                                         nullptr, 0) == CLAY_OK);
    CHECK(state == CLAY_BRICK_SURFACE);  // the KEY's state, never the halo's

    std::vector<std::uint16_t> plain_b(kSamples);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, b, 1, 0, nullptr, plain_b.data(), kSamples,
                                         nullptr, 0) == CLAY_OK);
    // a's +x halo plane is b's i = 0 plane
    for (int k = 0; k < kDim; ++k)
        for (int j = 0; j < kDim; ++j) {
            const std::size_t halo =
                ((static_cast<std::size_t>(k + kApron) * w) + (j + kApron)) * w + (kDim + kApron);
            const std::size_t own = (static_cast<std::size_t>(k) * kDim + j) * kDim + 0;
            CHECK(tile[halo] == plain_b[own]);
        }

    // and the interior of the padded tile is the unpadded brick, unchanged
    std::vector<std::uint16_t> plain_a(kSamples);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, a, 1, 0, nullptr, plain_a.data(), kSamples,
                                         nullptr, 0) == CLAY_OK);
    for (int k = 0; k < kDim; ++k)
        for (int j = 0; j < kDim; ++j)
            for (int i = 0; i < kDim; ++i) {
                const std::size_t p =
                    ((static_cast<std::size_t>(k + kApron) * w) + (j + kApron)) * w + (i + kApron);
                const std::size_t q = (static_cast<std::size_t>(k) * kDim + j) * kDim + i;
                CHECK(tile[p] == plain_a[q]);
            }
}

TEST_CASE("brick apron: the edge of the sculpted region is defined, not garbage") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.3f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);

    // A surface brick with at least one neighbour the cache never tracked. Its
    // halo there must be the +band value a single sample of that brick
    // reports, so a tile at the edge filters against the band.
    std::vector<std::int32_t> keys = surface_keys(cache);
    REQUIRE(keys.size() >= 3);
    std::int32_t far_x = keys[0];
    std::size_t pick = 0;
    for (std::size_t i = 1; i < keys.size() / 3; ++i)
        if (keys[i * 3] > far_x) {
            far_x = keys[i * 3];
            pick = i;
        }
    const std::int32_t key[3] = {keys[pick * 3], keys[pick * 3 + 1], keys[pick * 3 + 2]};

    constexpr int kApron = 2;
    const std::size_t w = kDim + 2 * kApron;
    const std::size_t padded = padded_stride(kApron);
    std::vector<std::uint16_t> tile(padded, 0xBEEF);
    std::int32_t state = -1;
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, key, 1, kApron, &state, tile.data(), padded,
                                         nullptr, 0) == CLAY_OK);
    REQUIRE(state == CLAY_BRICK_SURFACE);

    // Nothing was left at the fill pattern: every element is written, which is
    // the property "no output sample is undefined" reduces to.
    for (std::uint16_t v : tile) CHECK(v != 0xBEEF);

    // and the halo values are inside the band, decoded through the cache's own
    // sample() so this compares against the engine rather than against a
    // constant this test computed
    for (std::size_t idx = 0; idx < padded; ++idx) {
        const int di = static_cast<int>(idx % w);
        const int dj = static_cast<int>((idx / w) % w);
        const int dk = static_cast<int>(idx / (w * w));
        const bool own = di >= kApron && di < kApron + kDim && dj >= kApron &&
                         dj < kApron + kDim && dk >= kApron && dk < kApron + kDim;
        if (own) continue;
        // decode: half bits -> float, via a lattice sample of the brick the
        // coordinate actually falls in
        const int g[3] = {key[0] * kDim + di - kApron, key[1] * kDim + dj - kApron,
                          key[2] * kDim + dk - kApron};
        auto fdiv = [](int x, int y) { return x >= 0 ? x / y : -(((-x) + y - 1) / y); };
        const std::int32_t nk[3] = {fdiv(g[0], kDim), fdiv(g[1], kDim), fdiv(g[2], kDim)};
        const int li = g[0] - nk[0] * kDim, lj = g[1] - nk[1] * kDim, lk = g[2] - nk[2] * kDim;
        float expect = 0.0f;
        REQUIRE(clay_brick_cache_sample(cache, nk, li, lj, lk, &expect) == CLAY_OK);
        std::int32_t neighbour_state = 0;
        std::vector<std::uint16_t> nb(kSamples, 0);
        REQUIRE(clay_brick_cache_read_bricks(cache, 0, nk, 1, 0, &neighbour_state, nb.data(),
                                             kSamples, nullptr, 0) == CLAY_OK);
        if (neighbour_state == CLAY_BRICK_MISSING) {
            // A never-evaluated neighbour still answers: the halo is the +band
            // value sample() reports for it, so the tile filters against the
            // band rather than against whatever the buffer held.
            CHECK(expect == doctest::Approx(kBand));
            continue;
        }
        // Otherwise the halo is the neighbour's own stored bits, unconverted —
        // which is what makes filtering across the face agree with the cache.
        CHECK(tile[idx] ==
              nb[(static_cast<std::size_t>(lk) * kDim + lj) * kDim + li]);
    }
}

TEST_CASE("brick apron: the stride stays fixed, and MISSING stays untouched") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.9f, 0, 0, 0, grey);
    Cache cache(make_cache(true));
    const float lo[3] = {-1.2f, -1.2f, -1.2f}, hi[3] = {1.2f, 1.2f, 1.2f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    refill(cache, doc.d, true);

    std::vector<std::int32_t> surface = surface_keys(cache);
    REQUIRE(surface.size() >= 3);
    // one surface, one uniform (the centre), one the cache never tracked
    const std::int32_t keys[9] = {surface[0], surface[1], surface[2], 0,  0,
                                  0,          500,        500,        500};

    constexpr int kApron = 1;
    const std::size_t padded = padded_stride(kApron);
    std::vector<std::int32_t> states(3, -1);
    std::vector<std::uint16_t> halves(3 * padded, 0xBEEF);
    std::vector<std::uint8_t> rgba(3 * padded * 4, 0xAB);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys, 3, kApron, states.data(), halves.data(),
                                         halves.size(), rgba.data(), rgba.size()) == CLAY_OK);
    CHECK(states[0] == CLAY_BRICK_SURFACE);
    CHECK(states[1] == CLAY_BRICK_INSIDE);
    CHECK(states[2] == CLAY_BRICK_MISSING);

    // the two the cache holds are written at the padded stride...
    for (std::size_t i = 0; i < 2 * padded; ++i) CHECK(halves[i] != 0xBEEF);
    // ...and the missing one's WHOLE padded slice is left exactly as it was:
    // the rule is about the key, not about its neighbourhood
    for (std::size_t i = 2 * padded; i < 3 * padded; ++i) CHECK(halves[i] == 0xBEEF);
    for (std::size_t i = 2 * padded * 4; i < 3 * padded * 4; ++i) CHECK(rgba[i] == 0xAB);
}

TEST_CASE("brick apron: every refusal") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.3f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);
    std::vector<std::int32_t> keys = surface_keys(cache);
    REQUIRE(keys.size() >= 3);
    std::int32_t state = 0;

    // wider than the brick is refused, not clamped: past that the tile is
    // mostly neighbour and what the caller wants is a coarser lod
    std::vector<std::uint16_t> big(padded_stride(kDim + 1));
    CHECK(clay_brick_cache_read_bricks(cache, 0, keys.data(), 1, kDim + 1, &state, big.data(),
                                       big.size(), nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_read_bricks(cache, 0, keys.data(), 1, -1, &state, nullptr, 0, nullptr,
                                       0) == CLAY_ERROR_INVALID_ARGUMENT);
    // exactly dim is the widest accepted
    std::vector<std::uint16_t> widest(padded_stride(kDim));
    CHECK(clay_brick_cache_read_bricks(cache, 0, keys.data(), 1, kDim, &state, widest.data(),
                                       widest.size(), nullptr, 0) == CLAY_OK);
    // the capacity is against the PADDED stride, and still exact
    std::vector<std::uint16_t> one(padded_stride(1));
    CHECK(clay_brick_cache_read_bricks(cache, 0, keys.data(), 1, 1, &state, one.data(), kSamples,
                                       nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_read_bricks(cache, 0, keys.data(), 1, 1, &state, one.data(),
                                       one.size() - 1, nullptr, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_read_bricks(cache, 0, keys.data(), 1, 1, &state, one.data(), one.size(),
                                       nullptr, 0) == CLAY_OK);
}
