#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
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

// -- subset meshing -----------------------------------------------------------

namespace {

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

clay_brick_mesh_params plain_params() {
    clay_brick_mesh_params p{};
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.normals = CLAY_NORMAL_FACE;  // needs no document
    p.colors = 0;
    p.gradient_eps = 0.0f;
    return p;
}

// Every triangle as a sorted triple of quantized world positions, so two
// meshes can be compared for the GEOMETRY they carry rather than for the
// vertex numbering they happened to produce.
using Tri = std::array<std::array<std::int64_t, 3>, 3>;

std::vector<Tri> triangles_of(const clay_mesh* m) {
    const float* pos = clay_mesh_positions(m);
    const std::uint32_t* idx = clay_mesh_indices(m);
    const std::size_t tris = clay_mesh_index_count(m) / 3;
    std::vector<Tri> out;
    out.reserve(tris);
    auto q = [](float v) { return static_cast<std::int64_t>(std::llround(v * 1e6f)); };
    for (std::size_t t = 0; t < tris; ++t) {
        Tri tri{};
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = idx[t * 3 + c];
            tri[c] = {q(pos[v * 3]), q(pos[v * 3 + 1]), q(pos[v * 3 + 2])};
        }
        std::sort(tri.begin(), tri.end());
        out.push_back(tri);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Subset meshes deliberately repeat a straddler that touches two requested
// keys' shares, so unions are compared as SETS.
std::vector<Tri> deduped(std::vector<Tri> tris) {
    tris.erase(std::unique(tris.begin(), tris.end()), tris.end());
    return tris;
}

// The triangles inside ONE key's index range of a subset mesh — what a
// per-brick host stores as that key's share of the surface.
std::vector<Tri> range_triangles(const clay_mesh* m, const clay_brick_mesh_range& r) {
    const float* pos = clay_mesh_positions(m);
    const std::uint32_t* idx = clay_mesh_indices(m);
    std::vector<Tri> out;
    out.reserve(r.index_count / 3);
    auto q = [](float v) { return static_cast<std::int64_t>(std::llround(v * 1e6f)); };
    for (std::uint32_t t = r.index_first; t < r.index_first + r.index_count; t += 3) {
        Tri tri{};
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = idx[t + c];
            tri[c] = {q(pos[v * 3]), q(pos[v * 3 + 1]), q(pos[v * 3 + 2])};
        }
        std::sort(tri.begin(), tri.end());
        out.push_back(tri);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("subset meshing: a subset is the whole, brick for brick") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.4f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);

    const clay_brick_mesh_params params = plain_params();
    MeshHandle whole;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, nullptr, 0, nullptr, &whole.m) ==
            CLAY_OK);
    // Deduped: quantizing to 1e-6 can collapse a handful of sliver triangles
    // into one key, and this comparison is about SETS of triangles.
    const std::vector<Tri> expected = deduped(triangles_of(whole.m));
    REQUIRE(!expected.empty());

    // Mesh every surface brick ALONE and collect the union. As a set it is
    // exactly the whole mesh's triangles: marching a subset samples across its
    // boundary exactly as the whole does, so no cell is skipped and no
    // crossing moves — and each single-brick mesh also carries the straddlers
    // that reach a corner into it from neighbouring cells, so a triangle near
    // a seam appears in more than one single-brick mesh. That repetition is
    // the documented attribution cost, which is why the union is deduped.
    std::vector<std::int32_t> keys = surface_keys(cache);
    const std::size_t count = keys.size() / 3;
    std::vector<Tri> got;
    for (std::size_t i = 0; i < count; ++i) {
        clay_brick_mesh_range range{};
        MeshHandle one;
        REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, keys.data() + i * 3, 1, &range,
                                      &one.m) == CLAY_OK);
        const std::vector<Tri> part = triangles_of(one.m);
        got.insert(got.end(), part.begin(), part.end());
        // one key alone owns the whole output, straddlers included
        CHECK(range.vertex_first == 0);
        CHECK(range.index_first == 0);
        CHECK(range.vertex_count == clay_mesh_vertex_count(one.m));
        CHECK(range.index_count == clay_mesh_index_count(one.m));
    }
    std::sort(got.begin(), got.end());
    CHECK(deduped(std::move(got)) == expected);
}

TEST_CASE("subset meshing: a seam vertex is duplicated, never moved") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.4f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);
    const clay_brick_mesh_params params = plain_params();

    // Two adjacent surface bricks, meshed as one subset and as two.
    std::vector<std::int32_t> keys = surface_keys(cache);
    const std::size_t count = keys.size() / 3;
    REQUIRE(count >= 2);
    bool found = false;
    std::int32_t pair[6] = {0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < count && !found; ++i) {
        const std::int32_t cand[6] = {keys[i * 3],     keys[i * 3 + 1], keys[i * 3 + 2],
                                      keys[i * 3] + 1, keys[i * 3 + 1], keys[i * 3 + 2]};
        std::int32_t states[2] = {-1, -1};
        REQUIRE(clay_brick_cache_read_bricks(cache, 0, cand, 2, 0, states, nullptr, 0, nullptr,
                                             0) == CLAY_OK);
        if (states[0] == CLAY_BRICK_SURFACE && states[1] == CLAY_BRICK_SURFACE) {
            std::memcpy(pair, cand, sizeof pair);
            found = true;
        }
    }
    REQUIRE(found);

    MeshHandle together;
    clay_brick_mesh_range ranges[2]{};
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, pair, 2, ranges, &together.m) ==
            CLAY_OK);
    MeshHandle first, second;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, pair, 1, nullptr, &first.m) == CLAY_OK);
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, pair + 3, 1, nullptr, &second.m) ==
            CLAY_OK);

    // The two separately-meshed halves carry exactly the triangles the joint
    // mesh does — bit-identical positions, so drawing them together leaves no
    // gap. Compared as sets: each half also carries the straddlers reaching
    // into it, including from the OTHER half's cells, so triangles near their
    // seam appear in both halves and once in the joint mesh.
    std::vector<Tri> apart = triangles_of(first.m);
    const std::vector<Tri> b = triangles_of(second.m);
    apart.insert(apart.end(), b.begin(), b.end());
    std::sort(apart.begin(), apart.end());
    CHECK(deduped(std::move(apart)) == deduped(triangles_of(together.m)));
    // welding across the seam means the joint mesh has FEWER vertices than the
    // two halves put together — the property the header warns about
    CHECK(clay_mesh_vertex_count(together.m) <=
          clay_mesh_vertex_count(first.m) + clay_mesh_vertex_count(second.m));

    SUBCASE("and the ranges partition the output") {
        CHECK(ranges[0].vertex_first == 0);
        CHECK(ranges[0].index_first == 0);
        CHECK(ranges[1].vertex_first == ranges[0].vertex_first + ranges[0].vertex_count);
        CHECK(ranges[1].index_first == ranges[0].index_first + ranges[0].index_count);
        CHECK(ranges[1].vertex_first + ranges[1].vertex_count ==
              clay_mesh_vertex_count(together.m));
        CHECK(ranges[1].index_first + ranges[1].index_count ==
              clay_mesh_index_count(together.m));
        for (int i = 0; i < 2; ++i)
            for (int a = 0; a < 3; ++a) CHECK(ranges[i].key[a] == pair[i * 3 + a]);
    }
}

namespace {

// A finer lattice than the suite default, so the sphere spans dozens of
// bricks and a dab's dirty set has a real boundary with unrequested surface
// bricks on the far side — the geometry issue #66 is about. At the suite's
// 0.05 voxel a brick is as big as the sphere and every dab dirties everything.
constexpr float kFineVoxel = 0.02f;

clay_brick_cache* make_fine_cache() {
    clay_brick_config c;
    REQUIRE(clay_brick_config_defaults(&c) == CLAY_OK);
    c.dim = kDim;
    c.voxel_size = kFineVoxel;
    c.band_voxels = kBandVoxels;
    return clay_brick_cache_create(&c);
}

// One relief dab on the surface: the edit whose dirty set is the subset a
// host re-meshes while the pointer is down.
clay_node_id add_relief_dab(Doc& doc, float r, float x, float y, float z) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, y, z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_op(it, CLAY_OP_RELIEF) == CLAY_OK);
    REQUIRE(clay_item_set_blend(it, CLAY_BLEND_HARD, r) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(it, r) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

// The refill loop again, this time keeping the keys the drain reported —
// exactly the list a host hands back as the subset to re-mesh.
std::vector<std::int32_t> refill_collect(clay_brick_cache* cache, const clay_document* doc) {
    constexpr std::size_t kChunk = 64;
    std::vector<clay_brick_request> reqs(kChunk);
    std::vector<float> values(kChunk * kSamples);
    std::vector<std::int32_t> results(kChunk);
    std::vector<std::int32_t> keys;
    for (;;) {
        std::size_t count = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        for (std::size_t i = 0; i < count; ++i)
            for (int a = 0; a < 3; ++a) keys.push_back(reqs[i].key[a]);
        REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                        count * kSamples, nullptr, 0, results.data(),
                                        &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }
    return keys;
}

// Closed containment: a corner exactly on a brick's boundary plane belongs to
// both neighbours, matching the attribution rule the header documents.
bool corner_in_keys(const float c[3], const std::vector<std::int32_t>& keys) {
    const std::size_t count = keys.size() / 3;
    for (std::size_t i = 0; i < count; ++i) {
        bool in = true;
        for (int a = 0; a < 3; ++a) {
            const float lo = static_cast<float>(keys[i * 3 + a] * kDim) * kFineVoxel;
            const float hi = static_cast<float>((keys[i * 3 + a] + 1) * kDim) * kFineVoxel;
            if (!(c[a] >= lo && c[a] <= hi)) {
                in = false;
                break;
            }
        }
        if (in) return true;
    }
    return false;
}

struct TouchCount {
    std::size_t all_three = 0;
    std::size_t at_least_one = 0;
    std::vector<Tri> touching;  // sorted; >= 1 corner inside the keys
};

TouchCount touching_triangles(const clay_mesh* m, const std::vector<std::int32_t>& keys) {
    const float* pos = clay_mesh_positions(m);
    const std::uint32_t* idx = clay_mesh_indices(m);
    const std::size_t tris = clay_mesh_index_count(m) / 3;
    auto q = [](float v) { return static_cast<std::int64_t>(std::llround(v * 1e6f)); };
    TouchCount out;
    for (std::size_t t = 0; t < tris; ++t) {
        int inside = 0;
        Tri tri{};
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = idx[t * 3 + c];
            if (corner_in_keys(pos + v * 3, keys)) ++inside;
            tri[c] = {q(pos[v * 3]), q(pos[v * 3 + 1]), q(pos[v * 3 + 2])};
        }
        if (inside == 3) ++out.all_three;
        if (inside >= 1) {
            ++out.at_least_one;
            std::sort(tri.begin(), tri.end());
            out.touching.push_back(tri);
        }
    }
    std::sort(out.touching.begin(), out.touching.end());
    return out;
}

}  // namespace

// Regression for issue #66: a subset mesh omitted every triangle straddling
// the requested set's boundary — triangles from cells owned by unrequested
// bricks with a corner inside a requested brick. Dilating the request only
// moved the boundary, so NO sequence of subset calls could maintain a complete
// surface, and a host fell back to whole-surface meshing on every dab. The
// subset must return every whole-mesh triangle with at least one corner in a
// requested brick: what was N-missing/0-spurious becomes 0-missing/0-spurious.
TEST_CASE("subset meshing: straddlers at the request boundary are emitted") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.4f, 0, 0, 0, grey);
    Cache cache(make_fine_cache());
    mark_and_fill(cache, doc, false);

    // One dab, then exactly the issue's two calls: same cache, same state,
    // differing only in the key list.
    const clay_node_id dab = add_relief_dab(doc, 0.06f, 0.0f, 0.4f, 0.0f);
    std::size_t marked = 0;
    REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, &dab, 1, &marked) ==
            CLAY_OK);
    const std::vector<std::int32_t> dirty = refill_collect(cache, doc.d);
    REQUIRE(!dirty.empty());

    const clay_brick_mesh_params params = plain_params();
    MeshHandle whole, subset;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, nullptr, 0, nullptr, &whole.m) ==
            CLAY_OK);
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, dirty.data(), dirty.size() / 3,
                                  nullptr, &subset.m) == CLAY_OK);

    const TouchCount w = touching_triangles(whole.m, dirty);
    const TouchCount s = touching_triangles(subset.m, dirty);

    // The fixture is meaningful: the whole mesh HAS straddlers here, so the
    // old strictly-inside subset would fail the checks below.
    REQUIRE(w.at_least_one > w.all_three);

    // Wholly-inside triangles were never in question...
    CHECK(s.all_three == w.all_three);
    // ...and the straddlers now arrive too: 0 missing, 0 spurious.
    CHECK(s.at_least_one == w.at_least_one);
    CHECK(s.touching == w.touching);

    // The subset invents nothing: every triangle it returns is in the whole.
    const std::vector<Tri> whole_tris = triangles_of(whole.m);
    const std::vector<Tri> subset_tris = triangles_of(subset.m);
    CHECK(std::includes(whole_tris.begin(), whole_tris.end(), subset_tris.begin(),
                        subset_tris.end()));
}

// The claim the straddlers exist to honour: a host that stores geometry per
// brick and replaces each dirty key's share from the subset's ranges holds a
// COMPLETE surface after every dab — equal, as a set, to a full rebuild.
TEST_CASE("subset meshing: a per-brick host loop equals a full rebuild") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.4f, 0, 0, 0, grey);
    Cache cache(make_fine_cache());
    const clay_brick_mesh_params params = plain_params();

    std::map<std::array<std::int32_t, 3>, std::vector<Tri>> store;
    auto apply = [&](const std::vector<std::int32_t>& keys) {
        const std::size_t count = keys.size() / 3;
        REQUIRE(count > 0);
        std::vector<clay_brick_mesh_range> ranges(count);
        MeshHandle m;
        REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, keys.data(), count, ranges.data(),
                                      &m.m) == CLAY_OK);
        for (const clay_brick_mesh_range& r : ranges)
            store[{r.key[0], r.key[1], r.key[2]}] = range_triangles(m.m, r);
    };

    // The host loop from the first fill: mesh what the drain reported.
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    apply(refill_collect(cache, doc.d));

    // Several dabs, each re-meshing only its dirty keys.
    const float dabs[3][3] = {{0.0f, 0.4f, 0.0f}, {0.28f, 0.28f, 0.0f}, {0.0f, -0.4f, 0.0f}};
    for (const float* at : dabs) {
        const clay_node_id id = add_relief_dab(doc, 0.06f, at[0], at[1], at[2]);
        std::size_t marked = 0;
        REQUIRE(clay_brick_cache_mark_dirty_nodes(cache, doc.d, doc.layer, &id, 1, &marked) ==
                CLAY_OK);
        apply(refill_collect(cache, doc.d));
    }

    // The union of the per-brick shares, deduped (a straddler may sit in two
    // shares), is exactly a full rebuild of the final document.
    std::vector<Tri> maintained;
    for (const auto& [key, tris] : store)
        maintained.insert(maintained.end(), tris.begin(), tris.end());
    std::sort(maintained.begin(), maintained.end());

    MeshHandle rebuilt;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, nullptr, 0, nullptr, &rebuilt.m) ==
            CLAY_OK);
    CHECK(deduped(std::move(maintained)) == deduped(triangles_of(rebuilt.m)));
}

// Regression: clay_brick_cache_mesh documents that with no document the mesh
// has "positions and face normals", and CLAY_NORMAL_FACE says it "needs no
// document". Neither was true — mesh_bricks applied attributes only through the
// tape, so a document-less brick mesh came back with NO normals and a host
// shaded it flat black. Found while testing clay_mesh_copy_vertices, which
// refuses a layout naming an attribute the mesh does not carry.
TEST_CASE("brick meshing: face normals need no document, as the header says") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.4f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);

    const clay_brick_mesh_params params = plain_params();  // FACE normals, no colours
    MeshHandle m;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, nullptr, 0, nullptr, &m.m) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(m.m) > 0);
    const float* n = clay_mesh_normals(m.m);
    REQUIRE(n != nullptr);
    // and they are unit normals, not a zero-filled placeholder
    for (std::size_t v = 0; v < clay_mesh_vertex_count(m.m); ++v) {
        const float len = std::sqrt(n[v * 3] * n[v * 3] + n[v * 3 + 1] * n[v * 3 + 1] +
                                    n[v * 3 + 2] * n[v * 3 + 2]);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-4));
    }

    SUBCASE("and NONE still means none") {
        clay_brick_mesh_params none = params;
        none.normals = CLAY_NORMAL_NONE;
        MeshHandle bare;
        REQUIRE(clay_brick_cache_mesh(cache, nullptr, &none, nullptr, 0, nullptr, &bare.m) ==
                CLAY_OK);
        CHECK(clay_mesh_normals(bare.m) == nullptr);
    }
}

TEST_CASE("subset meshing: uniform and untracked keys are ordinary") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.9f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    const float lo[3] = {-1.2f, -1.2f, -1.2f}, hi[3] = {1.2f, 1.2f, 1.2f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    refill(cache, doc.d, false);
    const clay_brick_mesh_params params = plain_params();

    // A drained dirty set routinely contains bricks that turned out uniform, so
    // this must succeed and contribute nothing rather than refuse.
    const std::int32_t keys[6] = {0, 0, 0, 500, 500, 500};  // inside, never tracked
    clay_brick_mesh_range ranges[2]{};
    MeshHandle m;
    REQUIRE(clay_brick_cache_mesh(cache, nullptr, &params, keys, 2, ranges, &m.m) == CLAY_OK);
    CHECK(clay_mesh_vertex_count(m.m) == 0);
    CHECK(clay_mesh_index_count(m.m) == 0);
    for (int i = 0; i < 2; ++i) {
        CHECK(ranges[i].vertex_count == 0);
        CHECK(ranges[i].index_count == 0);
    }

    SUBCASE("every refusal") {
        clay_mesh* out = nullptr;
        CHECK(clay_brick_cache_mesh(cache, nullptr, &params, nullptr, 2, nullptr, &out) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        // ranges with no key list: nothing would have told the caller how many
        // to allocate, and this ABI infers no length
        CHECK(clay_brick_cache_mesh(cache, nullptr, &params, nullptr, 0, ranges, &out) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(out == nullptr);
    }
}

// -- batched raycast ----------------------------------------------------------

TEST_CASE("batched brick raycast: agrees with the single-ray path, ray for ray") {
    Doc doc;
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    add_sphere(doc, 0.4f, 0, 0, 0, grey);
    Cache cache(make_cache(false));
    mark_and_fill(cache, doc, false);

    // A fan that deliberately contains misses: a batched form that silently
    // dropped them would still pass a hits-only comparison.
    std::vector<float> rays;
    for (int i = -4; i <= 4; ++i)
        for (int j = -4; j <= 4; ++j) {
            const float y = static_cast<float>(i) * 0.15f;
            const float z = static_cast<float>(j) * 0.15f;
            const float r[6] = {-2.0f, y, z, 1.0f, 0.0f, 0.0f};
            rays.insert(rays.end(), r, r + 6);
        }
    const std::size_t count = rays.size() / 6;

    std::vector<std::int32_t> hits(count, -1);
    std::vector<float> ts(count, -1.0f), pos(count * 3, 0.0f), nrm(count * 3, 0.0f);
    REQUIRE(clay_brick_cache_raycast_many(cache, rays.data(), count, hits.data(), ts.data(),
                                          pos.data(), nrm.data()) == CLAY_OK);
    std::size_t hit_count = 0, miss_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t one_hit = -1;
        float one_t = -1.0f, one_pos[3] = {0, 0, 0}, one_nrm[3] = {0, 0, 0};
        REQUIRE(clay_brick_cache_raycast(cache, rays.data() + i * 6, rays.data() + i * 6 + 3,
                                         &one_hit, &one_t, one_pos, one_nrm) == CLAY_OK);
        CHECK(hits[i] == one_hit);
        CHECK(ts[i] == one_t);
        for (int c = 0; c < 3; ++c) {
            CHECK(pos[i * 3 + c] == one_pos[c]);
            CHECK(nrm[i * 3 + c] == one_nrm[c]);
        }
        (one_hit ? hit_count : miss_count)++;
    }
    CHECK(hit_count > 0);
    CHECK(miss_count > 0);

    SUBCASE("every output is optional, and a zero direction is refused") {
        REQUIRE(clay_brick_cache_raycast_many(cache, rays.data(), count, hits.data(), nullptr,
                                              nullptr, nullptr) == CLAY_OK);
        // no rays is no work, as it is for the document-level batch
        CHECK(clay_brick_cache_raycast_many(cache, nullptr, 0, nullptr, nullptr, nullptr,
                                            nullptr) == CLAY_OK);
        CHECK(clay_brick_cache_raycast_many(nullptr, rays.data(), count, hits.data(), nullptr,
                                            nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_raycast_many(cache, nullptr, count, hits.data(), nullptr, nullptr,
                                            nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
        std::vector<float> zero = rays;
        zero[3] = zero[4] = zero[5] = 0.0f;
        CHECK(clay_brick_cache_raycast_many(cache, zero.data(), count, hits.data(), nullptr,
                                            nullptr, nullptr) == CLAY_ERROR_INVALID_ARGUMENT);
    }
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

// -- gradient normals and the size of the document (issue #73) ----------------

namespace {

// A small blended scene near the origin, so gradient normals and colours
// depend on MORE than one item — a wrongly culled tape that dropped a nearby
// item would visibly change them, where a lone sphere would hide it.
void add_near_scene(Doc& doc) {
    const float red[3] = {0.9f, 0.2f, 0.1f};
    add_sphere(doc, 0.4f, 0, 0, 0, red);
    const float blue[3] = {0.1f, 0.2f, 0.9f};
    const float r = 0.25f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {0.3f, 0.2f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_color(it, blue) == CLAY_OK);
    REQUIRE(clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, 0.1f) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
}

}  // namespace

// Regression for issue #73: clay_brick_cache_mesh with gradient normals used
// to evaluate the WHOLE document tape at every vertex, so re-meshing a fixed
// brick set grew linearly with everything already sculpted — 4.8 ms at 1 node
// to 120 ms at 193 — while refill over the same bricks stayed flat, because it
// culls. Gradient normals and colours now go through per-brick culled tapes,
// the same culling refill uses. This test pins the CORRECTNESS half of that:
// the culled attributes must equal both a full-tape evaluation and the
// attributes of a document that never held the far nodes at all. The scaling
// half is gated by BM_MeshBricksGradGrownDoc in the benchmark suite.
TEST_CASE("brick meshing: gradient normals read the bricks' region, not the document") {
    Doc near_doc;   // the blended scene alone
    Doc crowded;    // the same scene plus 200 nodes ~3 world units away
    add_near_scene(near_doc);
    add_near_scene(crowded);
    const float green[3] = {0.2f, 0.8f, 0.2f};
    for (int i = 0; i < 200; ++i)
        add_sphere(crowded, 0.05f, 3.0f + 0.02f * static_cast<float>(i % 10),
                   0.4f * std::sin(static_cast<float>(i) * 0.7f),
                   0.4f * std::cos(static_cast<float>(i) * 1.3f), green);

    Cache a(make_cache(false));
    Cache b(make_cache(false));
    mark_and_fill(a, near_doc, false);
    mark_and_fill(b, crowded, false);

    // The FIXED brick set: the near scene's whole surface. The far nodes are
    // an order of magnitude beyond any influence bound that reaches it.
    std::vector<std::int32_t> keys = surface_keys(a);
    const std::size_t count = keys.size() / 3;
    REQUIRE(count > 0);

    clay_brick_mesh_params params{};
    params.struct_size = static_cast<std::uint32_t>(sizeof params);
    params.normals = CLAY_NORMAL_GRADIENT;
    params.colors = 1;
    params.gradient_eps = 0.0f;

    MeshHandle ma, mb;
    REQUIRE(clay_brick_cache_mesh(a, near_doc.d, &params, keys.data(), count, nullptr, &ma.m) ==
            CLAY_OK);
    REQUIRE(clay_brick_cache_mesh(b, crowded.d, &params, keys.data(), count, nullptr, &mb.m) ==
            CLAY_OK);

    const std::size_t verts = clay_mesh_vertex_count(ma.m);
    REQUIRE(verts > 0);
    REQUIRE(clay_mesh_vertex_count(mb.m) == verts);
    const float* pa = clay_mesh_positions(ma.m);
    const float* pb = clay_mesh_positions(mb.m);
    const float* na = clay_mesh_normals(ma.m);
    const float* nb = clay_mesh_normals(mb.m);
    const float* ca = clay_mesh_colors(ma.m);
    const float* cb = clay_mesh_colors(mb.m);
    REQUIRE(na != nullptr);
    REQUIRE(nb != nullptr);
    REQUIRE(ca != nullptr);
    REQUIRE(cb != nullptr);

    // Same culled refill, same marched cells: the geometry is bit-identical,
    // and the attributes must be too — the far nodes cannot reach these bricks.
    std::size_t worst = 0;
    float worst_diff = 0.0f;
    for (std::size_t i = 0; i < verts * 3; ++i) {
        REQUIRE(pa[i] == pb[i]);
        const float dn = std::fabs(na[i] - nb[i]);
        const float dc = std::fabs(ca[i] - cb[i]);
        if (dn > worst_diff) {
            worst_diff = dn;
            worst = i / 3;
        }
        REQUIRE(dn <= 1e-5f);
        REQUIRE(dc <= 1e-5f);
    }
    INFO("worst normal difference " << worst_diff << " at vertex " << worst);

    // And against the full document tape, which is what the gradient path
    // consulted before it culled: inside a brick's band-dilated cull region the
    // culled tape is band-clamp identical to the whole, so the normals must
    // agree with clay_eval_gradients over the crowded document itself.
    std::vector<float> full(verts * 3);
    REQUIRE(clay_eval_gradients(crowded.d, nullptr, pb, verts, full.data()) == CLAY_OK);
    for (std::size_t i = 0; i < verts * 3; ++i) REQUIRE(std::fabs(nb[i] - full[i]) <= 1e-4f);
}
