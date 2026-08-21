#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "clay.h"

// Meshing a LEVEL of the brick cache (brick-cache spec: LOD mips; c-abi spec:
// the brick cache's meshing entry point).
//
// Issue #93: the cache could BUILD a mip (clay_brick_cache_build_mip), READ one
// (clay_brick_cache_read_bricks takes a lod) and REPORT one
// (clay_brick_cache_current_lod) — and nothing could mesh one, because
// clay_brick_cache_mesh took a key list and no level. So the half of LOD a
// meshing host could reach was the half it could not use: the only route to
// coarse triangles was to reimplement the marcher over the fp16 samples, which
// is the lattice, the band clamping and the straddler attribution this cache
// exists to own.
//
// The regression case is "mesh at lod 1 and get real geometry", which on main
// cannot even be expressed — there is no argument for it. The rest of this
// suite is the boundary the level brings with it: a level that was never built
// is an ERROR rather than the empty mesh that already means "no surface", a
// level above 1 is refused rather than clamped, and the field attributes stay
// at level 0 where their exactness argument holds.

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr int kBandVoxels = 3;
// A mip keeps every second lattice point: same dim, twice the spacing.
constexpr float kCoarseCell = kVoxel * 2.0f;

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

struct MeshHandle {
    clay_mesh* m = nullptr;
    ~MeshHandle() { clay_mesh_destroy(m); }
    MeshHandle() = default;
    MeshHandle(const MeshHandle&) = delete;
    MeshHandle& operator=(const MeshHandle&) = delete;
};

clay_brick_cache* make_cache(bool colors = false) {
    clay_brick_config c;
    c.struct_size = sizeof(c);
    REQUIRE(clay_brick_config_defaults(&c) == CLAY_OK);
    c.dim = kDim;
    c.voxel_size = kVoxel;
    c.band_voxels = kBandVoxels;
    c.memory_budget = 0;
    c.colors = colors ? 1 : 0;
    return clay_brick_cache_create(&c);
}

void add_sphere(Doc& doc, float r, float x, float y, float z) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, y, z};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    const float grey[3] = {0.5f, 0.5f, 0.5f};
    REQUIRE(clay_item_set_color(it, grey) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
}

// The documented refill loop, distances only.
void mark_and_fill(clay_brick_cache* cache, Doc& doc) {
    REQUIRE(clay_brick_cache_mark_dirty_layer(cache, doc.d, doc.layer) == CLAY_OK);
    constexpr std::size_t kChunk = 64;
    std::vector<clay_brick_request> reqs(kChunk);
    std::vector<float> values(kChunk * kSamples);
    std::vector<std::int32_t> results(kChunk);
    for (;;) {
        std::size_t count = kChunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                        count * kSamples, nullptr, 0, results.data(),
                                        &accepted) == CLAY_OK);
        if (remaining == 0) break;
    }
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

// Floor-halved, which is how a fine key maps onto the 2x2x2 block it belongs
// to — the same arithmetic BrickCache::invalidate_mip_of uses.
std::array<std::int32_t, 3> coarse_of(const std::int32_t k[3]) {
    std::array<std::int32_t, 3> c{};
    for (int a = 0; a < 3; ++a) c[a] = k[a] >= 0 ? k[a] / 2 : (k[a] - 1) / 2;
    return c;
}

// Build the mip of every coarse block a surface brick falls in. Returns the
// coarse keys that built, packed as triples.
std::vector<std::int32_t> build_all_mips(clay_brick_cache* cache) {
    const std::vector<std::int32_t> fine = surface_keys(cache);
    std::set<std::array<std::int32_t, 3>> wanted;
    for (std::size_t i = 0; i < fine.size() / 3; ++i) wanted.insert(coarse_of(fine.data() + i * 3));
    std::vector<std::int32_t> built;
    for (const auto& c : wanted) {
        std::int32_t ok = 0;
        REQUIRE(clay_brick_cache_build_mip(cache, c.data(), &ok) == CLAY_OK);
        if (!ok) continue;
        built.insert(built.end(), {c[0], c[1], c[2]});
    }
    return built;
}

clay_brick_mesh_params face_params() {
    clay_brick_mesh_params p{};
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.normals = CLAY_NORMAL_FACE;  // needs no document, so it works at every level
    p.colors = 0;
    p.gradient_eps = 0.0f;
    return p;
}

struct Bounds {
    float min[3] = {1e30f, 1e30f, 1e30f};
    float max[3] = {-1e30f, -1e30f, -1e30f};
};

Bounds bounds_of(const clay_mesh* m) {
    Bounds b;
    const float* p = clay_mesh_positions(m);
    const std::size_t n = clay_mesh_vertex_count(m);
    for (std::size_t i = 0; i < n; ++i)
        for (int a = 0; a < 3; ++a) {
            b.min[a] = std::min(b.min[a], p[i * 3 + a]);
            b.max[a] = std::max(b.max[a], p[i * 3 + a]);
        }
    return b;
}

// Every float and index a mesh carries, as raw bytes: what "identical" has to
// mean for the proof that lod 0 is the call it always was.
std::vector<std::uint8_t> mesh_bytes(const clay_mesh* m) {
    const std::size_t verts = clay_mesh_vertex_count(m);
    const std::size_t idx = clay_mesh_index_count(m);
    std::vector<std::uint8_t> out;
    auto append = [&out](const void* src, std::size_t bytes) {
        if (!src || bytes == 0) return;
        const auto* p = static_cast<const std::uint8_t*>(src);
        out.insert(out.end(), p, p + bytes);
    };
    append(clay_mesh_positions(m), verts * 3 * sizeof(float));
    append(clay_mesh_normals(m), verts * 3 * sizeof(float));
    append(clay_mesh_colors(m), verts * 3 * sizeof(float));
    append(clay_mesh_uvs(m), verts * 2 * sizeof(float));
    append(clay_mesh_indices(m), idx * sizeof(std::uint32_t));
    return out;
}

}  // namespace

// -- the regression ------------------------------------------------------------

TEST_CASE("brick lod meshing: a built mip meshes, and meshes coarser") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);

    const std::vector<std::int32_t> coarse = build_all_mips(cache);
    REQUIRE(coarse.size() >= 3);
    // The sphere sits well inside the dirtied box, so every coarse block a
    // surface brick falls in has all eight children evaluated and clean.
    {
        const std::vector<std::int32_t> fine = surface_keys(cache);
        std::set<std::array<std::int32_t, 3>> wanted;
        for (std::size_t i = 0; i < fine.size() / 3; ++i)
            wanted.insert(coarse_of(fine.data() + i * 3));
        CHECK(coarse.size() / 3 == wanted.size());
    }

    const clay_brick_mesh_params params = face_params();
    MeshHandle fine_mesh, coarse_mesh;
    REQUIRE(clay_brick_cache_mesh_lod(cache, nullptr, &params, 0, nullptr, 0, nullptr,
                                      &fine_mesh.m) == CLAY_OK);
    // THE REGRESSION: on main there is no argument that expresses this call.
    REQUIRE(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, nullptr, 0, nullptr,
                                      &coarse_mesh.m) == CLAY_OK);

    const std::size_t fine_tris = clay_mesh_index_count(fine_mesh.m) / 3;
    const std::size_t coarse_tris = clay_mesh_index_count(coarse_mesh.m) / 3;
    REQUIRE(fine_tris > 0);
    // Real geometry, not an empty mesh dressed as success.
    CHECK(coarse_tris > 0);
    CHECK(clay_mesh_vertex_count(coarse_mesh.m) > 0);
    // Coarser in the direction a mip is coarse: the lattice halves on every
    // axis, so the surface's triangle count falls by roughly four. Asserted as
    // "well under half" rather than as a ratio, since the exact count depends
    // on where the surface cuts the coarse cells.
    CHECK(coarse_tris < fine_tris / 2);

    // Same surface, so the same box to within one coarse cell — the most a
    // crossing can move when the lattice it is found on doubles its spacing.
    const Bounds f = bounds_of(fine_mesh.m);
    const Bounds c = bounds_of(coarse_mesh.m);
    for (int a = 0; a < 3; ++a) {
        CHECK(std::fabs(f.min[a] - c.min[a]) <= kCoarseCell);
        CHECK(std::fabs(f.max[a] - c.max[a]) <= kCoarseCell);
    }
    // And it is the SPHERE's box, not some lattice artefact of the mip.
    for (int a = 0; a < 3; ++a) {
        CHECK(std::fabs(c.min[a] + 0.4f) <= kCoarseCell);
        CHECK(std::fabs(c.max[a] - 0.4f) <= kCoarseCell);
    }
}

TEST_CASE("brick lod meshing: a coarse key subset partitions its mesh like a fine one") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);
    const std::vector<std::int32_t> coarse = build_all_mips(cache);
    REQUIRE(coarse.size() >= 6);

    const clay_brick_mesh_params params = face_params();
    const std::size_t count = coarse.size() / 3;
    std::vector<clay_brick_mesh_range> ranges(count);
    MeshHandle m;
    REQUIRE(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, coarse.data(), count,
                                      ranges.data(), &m.m) == CLAY_OK);
    CHECK(clay_mesh_index_count(m.m) > 0);
    // The ranges partition the mesh at lod 1 exactly as they do at lod 0: the
    // level changes the lattice, not the bookkeeping.
    std::uint32_t v = 0, i = 0;
    for (std::size_t k = 0; k < count; ++k) {
        CHECK(ranges[k].key[0] == coarse[k * 3]);
        CHECK(ranges[k].key[1] == coarse[k * 3 + 1]);
        CHECK(ranges[k].key[2] == coarse[k * 3 + 2]);
        CHECK(ranges[k].vertex_first == v);
        CHECK(ranges[k].index_first == i);
        v += ranges[k].vertex_count;
        i += ranges[k].index_count;
    }
    CHECK(v == clay_mesh_vertex_count(m.m));
    CHECK(i == clay_mesh_index_count(m.m));
}

// -- the refusals --------------------------------------------------------------

TEST_CASE("brick lod meshing: an unbuilt level is NOT FOUND, never an empty mesh") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);
    const clay_brick_mesh_params params = face_params();
    REQUIRE(!surface_keys(cache).empty());

    // Nothing has built a mip. An empty mesh here would say "no surface",
    // which is false and is the one thing the caller cannot then tell apart.
    MeshHandle m;
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, nullptr, 0, nullptr, &m.m) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(m.m == nullptr);

    // A named coarse key with no mip is the same "not yet", reported per key.
    const std::vector<std::int32_t> fine = surface_keys(cache);
    const std::array<std::int32_t, 3> c = coarse_of(fine.data());
    MeshHandle one;
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, c.data(), 1, nullptr, &one.m) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(one.m == nullptr);

    // Build that one, and the same request answers.
    std::int32_t ok = 0;
    REQUIRE(clay_brick_cache_build_mip(cache, c.data(), &ok) == CLAY_OK);
    REQUIRE(ok == 1);
    REQUIRE(clay_brick_cache_current_lod(cache, c.data(), &ok) == CLAY_OK);
    CHECK(ok == 1);
    MeshHandle built;
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, c.data(), 1, nullptr, &built.m) ==
          CLAY_OK);
    CHECK(built.m != nullptr);

    // A DIFFERENT coarse key, still unbuilt, is still refused — the check is
    // per key rather than "some mip exists somewhere".
    std::array<std::int32_t, 3> far = c;
    far[0] += 64;
    MeshHandle miss;
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, far.data(), 1, nullptr, &miss.m) ==
          CLAY_ERROR_NOT_FOUND);
}

TEST_CASE("brick lod meshing: a level above 1 is refused, not clamped") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);
    build_all_mips(cache);
    const clay_brick_mesh_params params = face_params();

    for (std::int32_t lod : {2, 3, 17, -1}) {
        MeshHandle m;
        CAPTURE(lod);
        // Answering level 0 for a request for level 4 would put geometry at
        // sixteen times the intended size on screen.
        CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, lod, nullptr, 0, nullptr,
                                        &m.m) == CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(m.m == nullptr);
    }
}

TEST_CASE("brick lod meshing: field attributes are level 0 only") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);
    REQUIRE(!build_all_mips(cache).empty());

    clay_brick_mesh_params gradient = face_params();
    gradient.normals = CLAY_NORMAL_GRADIENT;
    clay_brick_mesh_params coloured = face_params();
    coloured.colors = 1;

    // Both are fine at lod 0 with a document, so what follows is about the
    // LEVEL and not about the parameters.
    {
        MeshHandle g, c;
        CHECK(clay_brick_cache_mesh_lod(cache, doc.d, &gradient, 0, nullptr, 0, nullptr, &g.m) ==
              CLAY_OK);
        CHECK(clay_brick_cache_mesh_lod(cache, doc.d, &coloured, 0, nullptr, 0, nullptr, &c.m) ==
              CLAY_OK);
        CHECK(clay_mesh_normals(g.m) != nullptr);
        CHECK(clay_mesh_colors(c.m) != nullptr);
    }
    // At lod 1 they are refused rather than quietly downgraded: a coarse vertex
    // is not on the field's surface, so the per-brick culled tape that makes
    // them exact at lod 0 no longer agrees with the whole document's — and the
    // mip carries no colour lattice, which read_bricks already reports rather
    // than averaging.
    {
        MeshHandle g, c;
        CHECK(clay_brick_cache_mesh_lod(cache, doc.d, &gradient, 1, nullptr, 0, nullptr, &g.m) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(clay_brick_cache_mesh_lod(cache, doc.d, &coloured, 1, nullptr, 0, nullptr, &c.m) ==
              CLAY_ERROR_INVALID_ARGUMENT);
        CHECK(g.m == nullptr);
        CHECK(c.m == nullptr);
    }
    // Face normals come from the triangles and need no field, so they answer at
    // every level — otherwise "refused" would mean "no normals at lod 1".
    {
        const clay_brick_mesh_params params = face_params();
        MeshHandle f;
        REQUIRE(clay_brick_cache_mesh_lod(cache, nullptr, &params, 1, nullptr, 0, nullptr,
                                          &f.m) == CLAY_OK);
        REQUIRE(clay_mesh_vertex_count(f.m) > 0);
        CHECK(clay_mesh_normals(f.m) != nullptr);
    }
}

TEST_CASE("brick lod meshing: an empty cache is EMPTY at every valid level") {
    Cache cache(make_cache());
    const clay_brick_mesh_params params = face_params();
    // Never marked, never filled: no surface and no mip. That is an ordinary
    // state of a session, so it is an empty mesh at both levels and not the
    // "unbuilt level" error — which is exactly the distinction this call has to
    // keep.
    for (std::int32_t lod : {0, 1}) {
        MeshHandle m;
        CAPTURE(lod);
        REQUIRE(clay_brick_cache_mesh_lod(cache, nullptr, &params, lod, nullptr, 0, nullptr,
                                          &m.m) == CLAY_OK);
        REQUIRE(m.m != nullptr);
        CHECK(clay_mesh_vertex_count(m.m) == 0);
        CHECK(clay_mesh_index_count(m.m) == 0);
    }
}

TEST_CASE("brick lod meshing: null arguments are refused as they always were") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);
    const clay_brick_mesh_params params = face_params();
    const std::int32_t key[3] = {0, 0, 0};
    clay_mesh* m = nullptr;

    CHECK(clay_brick_cache_mesh_lod(nullptr, nullptr, &params, 0, nullptr, 0, nullptr, &m) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, nullptr, 0, nullptr, 0, nullptr, &m) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 0, nullptr, 0, nullptr, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // A key count without keys, and ranges without a key list to size them by.
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 0, nullptr, 4, nullptr, &m) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    clay_brick_mesh_range range{};
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &params, 0, nullptr, 0, &range, &m) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    // Gradient normals still need a document, whatever the level.
    clay_brick_mesh_params gradient = face_params();
    gradient.normals = CLAY_NORMAL_GRADIENT;
    CHECK(clay_brick_cache_mesh_lod(cache, nullptr, &gradient, 0, key, 1, nullptr, &m) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(m == nullptr);
}

// -- the did-not-break-anything proof -----------------------------------------

TEST_CASE("brick lod meshing: lod 0 is the call clay_brick_cache_mesh always was") {
    Doc doc;
    add_sphere(doc, 0.4f, 0, 0, 0);
    Cache cache(make_cache());
    mark_and_fill(cache, doc);
    // Mips exist, so the level path is live and cannot be said to be untested
    // when the two agree.
    REQUIRE(!build_all_mips(cache).empty());

    const std::vector<std::int32_t> keys = surface_keys(cache);
    const std::size_t count = keys.size() / 3;
    REQUIRE(count >= 4);

    struct Case {
        const char* what;
        const std::int32_t* keys;
        std::size_t count;
        bool ranges;
        bool with_doc;
    };
    const Case cases[] = {
        {"whole cache, face normals", nullptr, 0, false, false},
        {"whole cache, gradient normals and colours", nullptr, 0, false, true},
        {"one key", keys.data(), 1, true, false},
        {"a subset", keys.data(), count / 2, true, false},
        {"every key", keys.data(), count, true, true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        clay_brick_mesh_params params = face_params();
        if (c.with_doc) {
            params.normals = CLAY_NORMAL_GRADIENT;
            params.colors = 1;
        }
        const clay_document* d = c.with_doc ? doc.d : nullptr;
        std::vector<clay_brick_mesh_range> old_ranges(c.ranges ? c.count : 0);
        std::vector<clay_brick_mesh_range> new_ranges(c.ranges ? c.count : 0);
        MeshHandle old_mesh, new_mesh;
        REQUIRE(clay_brick_cache_mesh(cache, d, &params, c.keys, c.count,
                                      c.ranges ? old_ranges.data() : nullptr,
                                      &old_mesh.m) == CLAY_OK);
        REQUIRE(clay_brick_cache_mesh_lod(cache, d, &params, 0, c.keys, c.count,
                                          c.ranges ? new_ranges.data() : nullptr,
                                          &new_mesh.m) == CLAY_OK);
        // Byte for byte, not triangle set for triangle set: the older entry
        // point is the newer one at lod 0, so anything less than equality here
        // would be a behaviour change smuggled in beside a new argument.
        CHECK(mesh_bytes(old_mesh.m) == mesh_bytes(new_mesh.m));
        CHECK(clay_mesh_vertex_count(old_mesh.m) == clay_mesh_vertex_count(new_mesh.m));
        CHECK(clay_mesh_index_count(old_mesh.m) == clay_mesh_index_count(new_mesh.m));
        // Guarded: the no-ranges cases leave both vectors empty, and data() on
        // an empty vector may be null, which memcmp forbids however zero the
        // length is.
        if (!old_ranges.empty())
            REQUIRE(std::memcmp(old_ranges.data(), new_ranges.data(),
                                old_ranges.size() * sizeof(clay_brick_mesh_range)) == 0);
    }
}
