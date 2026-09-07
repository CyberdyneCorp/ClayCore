#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "clay.h"

// THE ORACLE FOR #471: a full rebuild.
//
// The probe next door (test_intersect_delta_bound.cpp) samples the field and
// checks a classification. That is necessary and not sufficient: what a host
// actually does is keep a brick cache across the edit, dirty the region the
// engine reported, refill it, and mesh. If the region is one term short, the
// bricks outside it keep values that no longer describe the document -- stale
// geometry, no error, nothing on the host's side to point at.
//
// So this drives exactly that loop and compares it against a cache built from
// nothing on the moved document. THE FULL REBUILD IS THE ORACLE, and the
// comparison is the stored fp16 payload of every surface brick, key by key:
// bit-identical, not "close". Both caches are marked over the SAME world box
// so their tracked key sets are comparable rather than an artefact of what
// each one happened to be told about.
//
// The counts come with it, and they are the acceptance condition: the
// incremental refill must touch a small, roughly constant number of bricks
// while the layer around it grows. A clock cannot say that on a shared
// machine; a brick count can, and it is deterministic.

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr int kBandVoxels = 3;

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
    Cache() {
        clay_brick_config cfg;
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        cfg.dim = kDim;
        cfg.voxel_size = kVoxel;
        cfg.band_voxels = kBandVoxels;
        cfg.memory_budget = 0;
        c = clay_brick_cache_create(&cfg);
        REQUIRE(c != nullptr);
    }
    ~Cache() { clay_brick_cache_destroy(c); }
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    operator clay_brick_cache*() const { return c; }
};

// The host frame loop: drain, evaluate, submit. Returns how many bricks were
// evaluated -- the count this file's performance claim is made in.
std::size_t refill(clay_brick_cache* cache, const clay_document* doc) {
    constexpr std::size_t chunk = 64;
    std::vector<clay_brick_request> reqs(chunk);
    std::vector<float> values(chunk * kSamples);
    std::vector<std::int32_t> results(chunk);
    std::size_t evaluated = 0;
    for (;;) {
        std::size_t count = chunk, remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        if (count == 0) break;
        REQUIRE(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                               count * kSamples, nullptr, 0) == CLAY_OK);
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(cache, reqs.data(), count, values.data(),
                                        count * kSamples, nullptr, 0, results.data(),
                                        &accepted) == CLAY_OK);
        evaluated += count;
        if (remaining == 0) break;
    }
    return evaluated;
}

using Key = std::tuple<int, int, int>;

// Every surface brick's stored payload, keyed by brick coordinate.
std::map<Key, std::vector<std::uint16_t>> snapshot(const clay_brick_cache* cache) {
    std::size_t count = 0;
    REQUIRE(clay_brick_cache_surface_bricks(cache, nullptr, &count) == CLAY_OK);
    std::map<Key, std::vector<std::uint16_t>> out;
    if (count == 0) return out;
    std::vector<std::int32_t> keys(count * 3);
    std::size_t capacity = count;
    REQUIRE(clay_brick_cache_surface_bricks(cache, keys.data(), &capacity) == CLAY_OK);
    std::vector<std::uint16_t> halves(count * kSamples);
    std::vector<std::int32_t> states(count);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys.data(), count, 0, states.data(),
                                         halves.data(), count * kSamples, nullptr, 0) == CLAY_OK);
    for (std::size_t i = 0; i < count; ++i)
        out[{keys[i * 3], keys[i * 3 + 1], keys[i * 3 + 2]}].assign(
            halves.begin() + static_cast<std::ptrdiff_t>(i * kSamples),
            halves.begin() + static_cast<std::ptrdiff_t>((i + 1) * kSamples));
    return out;
}

// What a mesher makes of the cache, as counts and a checksum of the vertex
// positions -- the "no stale triangles, no holes" half of the comparison,
// which brick equality already implies and which is cheap to state directly.
struct MeshDigest {
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    double checksum = 0.0;
};

MeshDigest mesh_of(const clay_brick_cache* cache, const clay_document* doc) {
    clay_brick_mesh_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof(p);
    clay_mesh* m = nullptr;
    REQUIRE(clay_brick_cache_mesh(cache, doc, &p, nullptr, 0, nullptr, &m) == CLAY_OK);
    REQUIRE(m != nullptr);
    MeshDigest d;
    d.vertices = clay_mesh_vertex_count(m);
    d.triangles = clay_mesh_index_count(m) / 3;
    if (d.vertices > 0) {
        clay_vertex_layout layout;
        std::memset(&layout, 0, sizeof layout);
        layout.struct_size = sizeof(layout);
        layout.stride = 3 * sizeof(float);
        layout.position_offset = 0;
        layout.normal_offset = -1;
        layout.color_offset = -1;
        layout.uv_offset = -1;
        std::vector<float> pos(d.vertices * 3);
        REQUIRE(clay_mesh_copy_vertices(m, &layout, pos.data(), pos.size() * sizeof(float)) ==
                CLAY_OK);
        // Position-weighted so a permutation of the same vertices is not
        // mistaken for the same mesh... which it would be under a plain sum.
        for (std::size_t i = 0; i < pos.size(); ++i)
            d.checksum += static_cast<double>(pos[i]) * static_cast<double>(i % 7 + 1);
    }
    clay_mesh_destroy(m);
    return d;
}

clay_node_id add_item(Doc& doc, int32_t prim, const float params[7], const float pos[3],
                      int32_t op, int32_t blend, float blend_k, clay_node_id group = 0) {
    clay_item_desc it;
    std::memset(&it, 0, sizeof it);
    it.struct_size = sizeof(it);
    it.prim = prim;
    for (int i = 0; i < 7; ++i) it.params[i] = params[i];
    for (int i = 0; i < 3; ++i) it.position[i] = pos[i];
    it.rotation[3] = 1.0f;
    it.scale = 1.0f;
    it.op = op;
    it.blend = blend;
    it.blend_k = blend_k;
    clay_node_id id = 0;
    if (group != 0)
        REQUIRE(clay_add_item_in_group(doc.d, doc.layer, group, -1, &it, &id) == CLAY_OK);
    else
        REQUIRE(clay_add_item(doc.d, doc.layer, &it, &id) == CLAY_OK);
    return id;
}

// THE ISSUE'S FIXTURE, in the shapes the matrix asks for: a starting sphere
// plus a spiral of stamps on it, and an INTERSECT cylinder at the root of the
// same layer, dragged in a circle at constant height.
//
// `body` scales THE FORM AND NOTHING ELSE. The cutter and the drag keep their
// size, which is what the 10x-extent case actually is -- an artist dragging
// the same tool across a bigger sculpt -- and it is the condition the
// acceptance claim is stated under: the swept support stays approximately
// constant while the layer grows.
struct Shape {
    const char* name = "";
    float blend_k = 0.0f;      // 0 = hard everywhere
    bool in_group = false;     // the operand under a blended group
    bool mirror = false;
    bool radial = false;
    bool fold_above = false;   // a second layer folding smoothly onto this one
    bool start_outside = false;
    float body = 1.0f;         // the form's radius
    int dabs = 24;
};

struct Built {
    clay_node_id cutter = 0;
    float from[3] = {0, 0, 0};
    float to[3] = {0, 0, 0};
    float span = 0.0f;  // half-width of a world box holding both states
};

Built build(Doc& doc, const Shape& s) {
    const float k = s.blend_k;
    const int32_t blend = k > 0 ? CLAY_BLEND_QUADRATIC : CLAY_BLEND_HARD;
    const float r = s.body;
    {
        const float params[7] = {r, 0, 0, 0, 0, 0, 0};
        const float pos[3] = {0, 0, 0};
        add_item(doc, CLAY_PRIM_SPHERE, params, pos, CLAY_OP_ADD, blend, k * r);
    }
    // Stamps along a spiral ON the sphere, so the surface is a worked one and
    // the chain is a real chain rather than a single primitive.
    for (int i = 0; i < s.dabs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(s.dabs - 1);
        const float phi = t * 7.0f;
        const float y = -0.8f + 1.6f * t;
        const float ring = std::sqrt(std::max(0.05f, 1.0f - y * y));
        const float params[7] = {0.22f * r, 0, 0, 0, 0, 0, 0};
        const float pos[3] = {std::cos(phi) * ring * r, y * r, std::sin(phi) * ring * r};
        add_item(doc, CLAY_PRIM_SPHERE, params, pos, CLAY_OP_ADD, blend, k * r);
    }
    if (s.mirror) REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.08f) == CLAY_OK);
    if (s.radial) REQUIRE(clay_set_layer_radial(doc.d, doc.layer, 1, 5, 0.1f) == CLAY_OK);
    if (s.fold_above) {
        clay_layer_id top = 0;
        REQUIRE(clay_add_sdf_layer(doc.d, "top", &top) == CLAY_OK);
        // A BOX OVER THE WHOLE FORM, not a detail beside it: the fold's own
        // support only reaches the document's surface where the two layers'
        // fields are within it of each other, so a small shape overlapping a
        // corner of the form leaves the term untestable. This is the shape
        // that makes dropping `folds_from_layer_support` visible.
        clay_item_desc it;
        std::memset(&it, 0, sizeof it);
        it.struct_size = sizeof(it);
        it.prim = CLAY_PRIM_BOX;
        it.params[0] = 1.1f * r;
        it.params[1] = 1.1f * r;
        it.params[2] = 1.1f * r;
        it.position[1] = 0.35f * r;
        it.rotation[3] = 1.0f;
        it.scale = 1.0f;
        it.op = CLAY_OP_ADD;
        clay_node_id n = 0;
        REQUIRE(clay_add_item(doc.d, top, &it, &n) == CLAY_OK);
        REQUIRE(clay_document_set_layer_composition(doc.d, top, CLAY_OP_ADD,
                                                    CLAY_BLEND_QUADRATIC, 0.25f, 0.0f) == CLAY_OK);
    }
    clay_node_id group = 0;
    if (s.in_group)
        REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                     0.2f, 0.0f, &group) == CLAY_OK);

    Built b;
    // The issue's cutter and the issue's drag: a cylinder r 0.25 h 1.6 at
    // [0, 0.9, 0], moved around a circle of radius 0.7 at constant height.
    b.from[0] = s.start_outside ? -2.2f : -0.7f;
    b.from[1] = 0.9f;
    b.to[0] = 0.7f;
    b.to[1] = 0.9f;
    const float cut[7] = {0.25f, 0.8f, 0, 0, 0, 0, 0};
    b.cutter = add_item(doc, CLAY_PRIM_CAPPED_CYLINDER, cut, b.from, CLAY_OP_INTERSECT, blend,
                        k, group);
    b.span = r + 1.0f + std::fabs(b.from[0]);
    return b;
}

// The move, through the entry point that reports what it changed.
struct Reach {
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    std::int32_t has = 0;
    std::int32_t infinite = 0;
    double volume() const {
        if (!has || infinite) return 0.0;
        return static_cast<double>(max[0] - min[0]) * (max[1] - min[1]) * (max[2] - min[2]);
    }
};

Reach move_to(Doc& doc, clay_node_id node, const float to[3]) {
    Reach r;
    const float axis[3] = {0, 1, 0};
    REQUIRE(clay_layer_set_transform_bound(doc.d, doc.layer, node, to, axis, 0.0f, 1.0f, r.min,
                                           r.max, &r.has, &r.infinite) == CLAY_OK);
    return r;
}

// One run: build, fill, move, refill the reported region, and compare with a
// cache built from nothing on the moved document.
struct Run {
    std::size_t full_bricks = 0;
    std::size_t incremental_bricks = 0;
    std::size_t surface_bricks = 0;
    Reach reach;
    double layer_volume = 0.0;
};

Run oracle(const Shape& s) {
    Run run;
    Doc doc;
    const Built b = build(doc, s);

    // THE SAME WORLD BOX FOR BOTH CACHES, and it has to contain the region the
    // move reports as well as the form. A cache holds bricks it was told
    // about; one told about a region the other never saw has extra tracked
    // bricks, and a marching cell that straddles into one of them meshes
    // differently -- a difference in the FIXTURE, dressed as a difference in
    // the engine. The box is taken from a throwaway build of the same fixture,
    // so nothing here depends on the answer under test being small.
    const Reach dry = [&s] {
        Doc tmp;
        const Built tb = build(tmp, s);
        return move_to(tmp, tb.cutter, tb.to);
    }();
    REQUIRE(dry.has == 1);
    REQUIRE(dry.infinite == 0);
    float world_min[3], world_max[3];
    for (int i = 0; i < 3; ++i) {
        world_min[i] = std::min(-b.span, dry.min[i] - 0.4f);
        world_max[i] = std::max(b.span, dry.max[i] + 0.4f);
    }

    Cache incremental;
    REQUIRE(clay_brick_cache_mark_dirty(incremental, world_min, world_max) == CLAY_OK);
    run.full_bricks = refill(incremental, doc.d);
    REQUIRE(run.full_bricks > 0);

    run.reach = move_to(doc, b.cutter, b.to);
    REQUIRE(run.reach.has == 1);
    REQUIRE(run.reach.infinite == 0);
    REQUIRE(clay_brick_cache_mark_dirty(incremental, run.reach.min, run.reach.max) == CLAY_OK);
    run.incremental_bricks = refill(incremental, doc.d);

    Cache fresh;
    REQUIRE(clay_brick_cache_mark_dirty(fresh, world_min, world_max) == CLAY_OK);
    refill(fresh, doc.d);

    const std::map<Key, std::vector<std::uint16_t>> a = snapshot(incremental);
    const std::map<Key, std::vector<std::uint16_t>> c = snapshot(fresh);
    run.surface_bricks = c.size();

    const std::string who = std::string(s.name) + ": ";
    const std::string counts = who + std::to_string(a.size()) +
                               " surface bricks incrementally against " +
                               std::to_string(c.size()) + " rebuilt";
    CHECK_MESSAGE(a.size() == c.size(), counts);
    std::size_t missing = 0, extra = 0, differing = 0;
    for (const auto& [key, values] : c) {
        const auto it = a.find(key);
        if (it == a.end()) {
            ++missing;
            continue;
        }
        if (it->second != values) ++differing;
    }
    for (const auto& [key, values] : a) {
        (void)values;
        if (c.find(key) == c.end()) ++extra;
    }
    const std::string holes = who + std::to_string(missing) +
                              " surface bricks the incremental refill never made (holes)";
    CHECK_MESSAGE(missing == 0, holes);
    const std::string stale =
        who + std::to_string(extra) + " surface bricks the rebuild does not have (stale)";
    CHECK_MESSAGE(extra == 0, stale);
    const std::string differ = who + std::to_string(differing) +
                               " bricks whose stored values differ from the rebuild";
    CHECK_MESSAGE(differing == 0, differ);

    const MeshDigest ma = mesh_of(incremental, doc.d);
    const MeshDigest mc = mesh_of(fresh, doc.d);
    const std::string verts = who + std::to_string(ma.vertices) +
                              " vertices incrementally against " + std::to_string(mc.vertices);
    CHECK_MESSAGE(ma.vertices == mc.vertices, verts);
    const std::string tris = who + std::to_string(ma.triangles) +
                             " triangles incrementally against " + std::to_string(mc.triangles);
    CHECK_MESSAGE(ma.triangles == mc.triangles, tris);
    const std::string moved = who + "the meshes differ in vertex positions";
    CHECK_MESSAGE(ma.checksum == mc.checksum, moved);

    // The layer's own box, for the volume ratio the benchmark reports.
    float lmin[3] = {0, 0, 0}, lmax[3] = {0, 0, 0};
    std::int32_t has = 0, inf = 0;
    REQUIRE(clay_layer_influence_bound(doc.d, doc.layer, lmin, lmax, &has, &inf) == CLAY_OK);
    if (has && !inf)
        run.layer_volume = static_cast<double>(lmax[0] - lmin[0]) * (lmax[1] - lmin[1]) *
                           (lmax[2] - lmin[2]);
    return run;
}

}  // namespace

TEST_CASE("intersect oracle: an incremental refill equals a full rebuild") {
    const Shape shapes[] = {
        {"hard intersect at the root", 0.0f},
        {"smooth intersect at the root", 0.09f},
        {"under a blended group", 0.09f, /*in_group=*/true},
        {"mirrored layer", 0.09f, false, /*mirror=*/true},
        {"radial layer", 0.09f, false, false, /*radial=*/true},
        {"a smooth fold above", 0.09f, false, false, false, /*fold_above=*/true},
        {"the operand starts outside", 0.09f, false, false, false, false,
         /*start_outside=*/true},
        {"hard, starting outside", 0.0f, false, false, false, false, true},
        {"everything at once", 0.09f, true, true, false, true, true},
    };
    for (const Shape& s : shapes) {
        INFO(s.name);
        const Run r = oracle(s);
        // The fixture has to have a surface worth comparing, or the equality
        // above is the equality of two empty maps.
        CHECK(r.surface_bricks > 20);
        // ... and the refill has to be smaller than the rebuild, or the
        // comparison passes because nothing was reused.
        CHECK(r.incremental_bricks < r.full_bricks);
    }
}

namespace {

// How many bricks a REGION marks, with nothing evaluated: mark it on a fresh
// cache and drain the queue. It is the count a host's frame pays for -- the
// requests it has to evaluate -- and it is what the issue reports growing
// 165-229x. Counting it without evaluating is what makes the 10x case a test
// rather than a minute.
std::size_t bricks_in(const float lo[3], const float hi[3]) {
    Cache cache;
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    std::size_t total = 0;
    std::vector<clay_brick_request> reqs(4096);
    for (;;) {
        std::size_t count = reqs.size(), remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
        total += count;
        if (count == 0 || remaining == 0) break;
    }
    return total;
}

// The two regions one drag frame can be given: what the engine proved, and
// what the influence query has to say.
struct Regions {
    std::size_t delta_bricks = 0;
    std::size_t conservative_bricks = 0;
};

Regions regions_for(const Shape& s) {
    Doc doc;
    const Built b = build(doc, s);
    // The conservative answer, taken on both sides of the move exactly as the
    // fallback path would union them.
    float cmin[3], cmax[3];
    std::int32_t has = 0, inf = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, b.cutter, cmin, cmax, &has, &inf) ==
            CLAY_OK);
    REQUIRE(has == 1);
    REQUIRE(inf == 0);
    const Reach r = move_to(doc, b.cutter, b.to);
    REQUIRE(r.has == 1);
    REQUIRE(r.infinite == 0);
    float amin[3], amax[3];
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, b.cutter, amin, amax, &has, &inf) ==
            CLAY_OK);
    for (int i = 0; i < 3; ++i) {
        cmin[i] = std::min(cmin[i], amin[i]);
        cmax[i] = std::max(cmax[i], amax[i]);
    }
    Regions out;
    out.delta_bricks = bricks_in(r.min, r.max);
    out.conservative_bricks = bricks_in(cmin, cmax);
    return out;
}

}  // namespace

TEST_CASE("intersect oracle: the refill does not scale with the layer's extent") {
    // THE ACCEPTANCE CONDITION OF #471, in counts.
    //
    // The same item count and the same drag on a form whose radius is
    // sqrt(10) -- ten times the cross-section, 31.6 times the volume. The
    // issue measured the region an intersect drag dirties growing with the
    // LAYER: 165-229x the bricks, seconds a frame. The swept bound must grow
    // with the SWEPT SUPPORT instead, which here is the same cutter making the
    // same move and so is very nearly constant.
    const Shape reference{"reference", 0.09f};
    Shape large = reference;
    large.name = "ten times the cross-section";
    large.body = std::sqrt(10.0f);

    const Regions a = regions_for(reference);
    const Regions b = regions_for(large);

    const double conservative_growth = static_cast<double>(b.conservative_bricks) /
                                       static_cast<double>(a.conservative_bricks);
    const double delta_growth =
        static_cast<double>(b.delta_bricks) / static_cast<double>(a.delta_bricks);
    INFO("conservative ", a.conservative_bricks, " -> ", b.conservative_bricks, " (x",
         conservative_growth, "), delta ", a.delta_bricks, " -> ", b.delta_bricks, " (x",
         delta_growth, ")");

    // The layer-wide region grows with the layer. That is the number the issue
    // reports and it is not in dispute -- it is what the fallback still costs.
    // Measured on this fixture: 900 -> 15,600 bricks, x17.3.
    CHECK(conservative_growth > 12.0);
    // The proved one does not. The cutter and the drag are unchanged, so what
    // is left growing is the CHAIN PAD -- the fixture's blend radii scale with
    // the form, as a real sculpt's do, and the pad follows them. Measured:
    // 540 -> 1,152 bricks, x2.13, against the x17.3 beside it and nothing like
    // the 165-229x the issue reports for the region it dirtied.
    CHECK(delta_growth < 4.0);
    // And at the large size the difference is the whole point: 13.5x fewer
    // bricks to evaluate per drag frame.
    CHECK(b.delta_bricks * 8 < b.conservative_bricks);
    // At the REFERENCE size the win is real and modest -- the form is barely
    // bigger than the cutter's sweep, which is why the issue measures 11x in
    // time there and seconds a frame at ten times the extent.
    CHECK(a.delta_bricks < a.conservative_bricks);
}
