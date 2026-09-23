#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "clay.h"

// What undoing ONE grab of a Move must refill (issue #639).
//
// A host Move is a grab deformer put at the head of a node's chain, one per
// segment, and the host dirties what clay_document_undo_bound reports. Through
// 0.120.0 that was the NODE's whole influence bound -- the command's target --
// so undoing one segment refilled every brick of the node, and the node's
// bound itself grows with every grab (each one dilates it by its pull). Undo
// cost was bricks-in-the-node times chain length.
//
// A grab is the identity outside its own ball, so the field cannot change
// anywhere else. This file pins both halves of that through the C ABI, which is
// the path the host takes:
//
//   - the COUNT: undoing one small grab on a large node marks roughly the
//     grab's ball, and the same number whatever the node's size and however
//     long its chain;
//   - the ORACLE: a cache refilled over that bound is bit-identical to a cache
//     built from nothing on the undone document -- at a smooth-union seam, on
//     a mirrored layer, inside a blended group, under a smooth fold, and for a
//     grab whose ball crosses the node's own box. That is the never-tighter
//     contract, checked on the stored payload rather than argued.

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
        REQUIRE(clay_document_enable_undo(d) == CLAY_OK);
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
};

struct Box {
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};
    std::int32_t has = 0;
    std::int32_t infinite = 0;
    float volume() const { return (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]); }
    bool contains(const Box& o) const {
        for (int a = 0; a < 3; ++a)
            if (o.lo[a] < lo[a] || o.hi[a] > hi[a]) return false;
        return true;
    }
};

Box ball_box(const float c[3], float r) {
    Box b;
    b.has = 1;
    for (int a = 0; a < 3; ++a) {
        b.lo[a] = c[a] - r;
        b.hi[a] = c[a] + r;
    }
    return b;
}

Box hull(const Box& a, const Box& b) {
    Box out = a;
    for (int i = 0; i < 3; ++i) {
        out.lo[i] = std::fmin(a.lo[i], b.lo[i]);
        out.hi[i] = std::fmax(a.hi[i], b.hi[i]);
    }
    return out;
}

Box clip(Box b, const Box& to) {
    for (int a = 0; a < 3; ++a) {
        b.lo[a] = std::fmax(b.lo[a], to.lo[a]);
        b.hi[a] = std::fmin(b.hi[a], to.hi[a]);
    }
    return b;
}

bool same_box(const Box& a, const Box& b) {
    for (int i = 0; i < 3; ++i)
        if (a.lo[i] != b.lo[i] || a.hi[i] != b.hi[i]) return false;
    return a.has == b.has && a.infinite == b.infinite;
}

Box dilated(Box b, float r) {
    for (int a = 0; a < 3; ++a) {
        b.lo[a] -= r;
        b.hi[a] += r;
    }
    return b;
}

// One assertion site for every C call a helper makes. Doctest's macros each
// expand to a branch, so a helper spelling REQUIRE per call reads as far more
// complex than it is.
void ok(clay_result r) { REQUIRE(r == CLAY_OK); }

clay_node_id add_sphere(Doc& doc, float radius, const float pos[3], float blend_k = 0.0f,
                        const clay_node_id* group = nullptr) {
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(item != nullptr);
    ok(clay_item_set_position(item, pos));
    if (blend_k > 0.0f) ok(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, blend_k));
    clay_node_id node = 0;
    const clay_result r = group ? clay_layer_add_item_in_group(doc.d, doc.layer, *group, -1, item,
                                                               &node)
                                : clay_layer_add_item(doc.d, doc.layer, item, &node);
    clay_item_destroy(item);
    ok(r);
    return node;
}

// A "base" layer UNDER the document's own, holding one sphere at `under`, and
// the document's layer composed onto it with a quadratic smooth union of
// radius `k` -- so the edited layer's value passes through one fold.
void fold_onto_base(Doc& doc, float k, const float under[3]) {
    clay_layer_id base = 0;
    ok(clay_add_sdf_layer(doc.d, "base", &base));
    ok(clay_document_move_layer(doc.d, base, 0));
    ok(clay_document_set_layer_composition(doc.d, doc.layer, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, k,
                                           0.0f));
    const clay_layer_id keep = doc.layer;
    doc.layer = base;
    add_sphere(doc, 0.5f, under);
    doc.layer = keep;
}

// One Move segment, the way the host records it: a grab at the HEAD of the
// node's chain. Centre and pull are in the node's frame.
struct Grab {
    float centre[3];
    float radius;
    float pull[3];
};

void add_grab(Doc& doc, clay_node_id node, const Grab& g) {
    const float params[8] = {g.centre[0], g.centre[1], g.centre[2], g.radius,
                             g.pull[0],   g.pull[1],   g.pull[2],   0.0f};
    REQUIRE(clay_layer_add_deformer(doc.d, doc.layer, node, CLAY_DEFORM_GRAB, params, 8,
                                    CLAY_EASE_LINEAR, 1) == CLAY_OK);
}

// `count` grabs spread around the equator of a sphere node of the given
// radius, in its own frame and well away from the pole the test's own grab
// sits on -- the chain a long Move session leaves behind.
void add_chain(Doc& doc, clay_node_id node, float node_radius, int count) {
    for (int i = 0; i < count; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        Grab g{{node_radius * std::cos(t), node_radius * std::sin(t), 0.0f},
               0.2f,
               {0.1f * std::cos(t), 0.1f * std::sin(t), 0.0f}};
        add_grab(doc, node, g);
    }
}

Box undo_bound(Doc& doc) {
    Box b;
    std::int32_t undone = 0;
    REQUIRE(clay_document_undo_bound(doc.d, &undone, b.lo, b.hi, &b.has, &b.infinite) == CLAY_OK);
    REQUIRE(undone == 1);
    return b;
}

Box redo_bound(Doc& doc) {
    Box b;
    std::int32_t redone = 0;
    REQUIRE(clay_document_redo_bound(doc.d, &redone, b.lo, b.hi, &b.has, &b.infinite) == CLAY_OK);
    REQUIRE(redone == 1);
    return b;
}

Box node_bound(const Doc& doc, clay_node_id node) {
    Box b;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, node, b.lo, b.hi, &b.has,
                                            &b.infinite) == CLAY_OK);
    return b;
}

std::uint64_t dirty_count(const Cache& cache) {
    clay_brick_stats stats;
    std::memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    REQUIRE(clay_brick_cache_stats(cache.c, &stats) == CLAY_OK);
    return stats.dirty_bricks;
}

// How many bricks a box marks in a fresh cache -- the count a host pays.
std::uint64_t bricks_marked(const Box& b) {
    REQUIRE(b.has == 1);
    REQUIRE(b.infinite == 0);
    Cache cache;
    REQUIRE(clay_brick_cache_mark_dirty(cache.c, b.lo, b.hi) == CLAY_OK);
    return dirty_count(cache);
}

// Mark what a step reported. "No bounds" marks nothing -- which is what a
// host does with it, and what the oracle then has to catch if it was wrong.
void mark(const Cache& cache, const Box& b) {
    REQUIRE(b.infinite == 0);
    if (b.has == 1) ok(clay_brick_cache_mark_dirty(cache.c, b.lo, b.hi));
}

// The host frame loop: drain, evaluate, submit.
std::size_t refill(const Cache& cache, const clay_document* doc) {
    constexpr std::size_t chunk = 64;
    std::vector<clay_brick_request> reqs(chunk);
    std::vector<float> values(chunk * kSamples);
    std::vector<std::int32_t> results(chunk);
    std::size_t evaluated = 0;
    for (;;) {
        std::size_t count = chunk, remaining = 0;
        ok(clay_brick_cache_take_dirty(cache.c, reqs.data(), &count, &remaining));
        if (count == 0) break;
        ok(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                          count * kSamples, nullptr, 0));
        std::size_t accepted = 0;
        ok(clay_brick_cache_submit(cache.c, reqs.data(), count, values.data(), count * kSamples,
                                   nullptr, 0, results.data(), &accepted));
        evaluated += count;
        if (remaining == 0) break;
    }
    return evaluated;
}

using Key = std::tuple<int, int, int>;

// Every brick key inside `world`, as brick coordinates.
std::vector<std::int32_t> keys_in(const Box& world) {
    const float brick = kVoxel * static_cast<float>(kDim);
    int lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int>(std::floor(world.lo[a] / brick));
        hi[a] = static_cast<int>(std::floor(world.hi[a] / brick));
    }
    std::vector<std::int32_t> keys;
    for (int z = lo[2]; z <= hi[2]; ++z)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int x = lo[0]; x <= hi[0]; ++x) {
                keys.push_back(x);
                keys.push_back(y);
                keys.push_back(z);
            }
    return keys;
}

// Every brick's state and, for a surface brick, its stored fp16 payload.
struct Snapshot {
    std::vector<std::int32_t> states;
    std::vector<std::uint16_t> halves;
};

Snapshot snapshot(const Cache& cache, const std::vector<std::int32_t>& keys) {
    const std::size_t n = keys.size() / 3;
    Snapshot s;
    s.states.assign(n, -1);
    s.halves.assign(n * kSamples, 0);
    REQUIRE(clay_brick_cache_read_bricks(cache.c, 0, keys.data(), n, 0, s.states.data(),
                                         s.halves.data(), n * kSamples, nullptr, 0) == CLAY_OK);
    return s;
}

// Bricks whose state or payload differ between two snapshots of one key list.
std::size_t differing_bricks(const Snapshot& a, const Snapshot& b) {
    std::size_t diff = 0;
    for (std::size_t i = 0; i < a.states.size(); ++i) {
        const bool same_state = a.states[i] == b.states[i];
        const bool same_values =
            std::memcmp(&a.halves[i * kSamples], &b.halves[i * kSamples],
                        kSamples * sizeof(std::uint16_t)) == 0;
        if (!same_state || !same_values) ++diff;
    }
    return diff;
}

// A cache built from nothing on a COPY of the document.
//
// A copy and not the document itself, and that is load-bearing: the seed store
// belongs to the DOCUMENT (clay.h, clay_resume_stats), and a refill resumes
// from any seed the invalidation left alone. So a cache rebuilt on the same
// document after a too-narrow bound resumes from the very seeds the bound
// failed to drop, reproduces the stale bricks exactly, and agrees with them.
// Measured while writing this file: with the mirror image dropped from the
// bound, the same-document oracle reported zero stale bricks.
Snapshot rebuilt(const Doc& doc, const Box& world, const std::vector<std::int32_t>& keys) {
    clay_blob* blob = nullptr;
    ok(clay_document_save_memory(doc.d, &blob));
    clay_document* copy = nullptr;
    const clay_result r = clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob),
                                                    &copy);
    clay_blob_destroy(blob);
    ok(r);
    Cache fresh;
    mark(fresh, world);
    refill(fresh, copy);
    Snapshot s = snapshot(fresh, keys);
    clay_document_destroy(copy);
    return s;
}

// THE ORACLE, for one step in one direction. `world` must hold everything the
// document draws; both caches are marked over it so their key sets agree.
struct OracleResult {
    std::size_t stale = 0;          // bricks the bound-refill got wrong
    std::size_t edit_visible = 0;   // bricks the step changed at all
    std::size_t refilled = 0;       // bricks the bound made the host refill
    std::size_t tracked = 0;        // bricks in `world`
};

template <typename Step>
OracleResult check_step(Doc& doc, const Box& world, Step step) {
    const std::vector<std::int32_t> keys = keys_in(world);
    Cache kept;
    mark(kept, world);
    refill(kept, doc.d);
    const Snapshot before = snapshot(kept, keys);

    mark(kept, step(doc));
    OracleResult r;
    r.refilled = refill(kept, doc.d);

    const Snapshot oracle = rebuilt(doc, world, keys);
    r.stale = differing_bricks(snapshot(kept, keys), oracle);
    r.edit_visible = differing_bricks(before, oracle);
    r.tracked = keys.size() / 3;
    return r;
}

// One step, checked: it changed something (or the pass would be for the wrong
// reason) and the bound-refill agrees with the rebuild everywhere.
template <typename Step>
void check_exact(Doc& doc, const Box& world, Step step) {
    const OracleResult r = check_step(doc, world, step);
    CAPTURE(r.refilled);
    CAPTURE(r.tracked);
    REQUIRE(r.edit_visible > 0);
    CHECK(r.stale == 0);
}

// Undo, then redo, each checked against its own full rebuild.
void check_undo_and_redo(Doc& doc, const Box& world) {
    check_exact(doc, world, undo_bound);
    check_exact(doc, world, redo_bound);
}

}  // namespace

// ---------------------------------------------------------------------------
// the box
// ---------------------------------------------------------------------------

TEST_CASE("undoing one grab reports its ball at both ends, dilated by the folds above") {
    Doc doc;
    // A base layer beneath and the edited layer composed onto it with a SMOOTH
    // union, so the edited layer's value passes through one fold -- a
    // quadratic blend of radius k, whose support is 4k (kernel/ops.h) -- and
    // the bound must carry it.
    const float fold_k = 0.1f;
    const float fold_support = 4.0f * fold_k;
    const float under[3] = {0.0f, -6.0f, 0.0f};
    fold_onto_base(doc, fold_k, under);
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const float node_radius = 3.0f;
    const clay_node_id node = add_sphere(doc, node_radius, at);
    add_chain(doc, node, node_radius, 8);
    // A sideways pull at the pole, so both ends of it lie well inside the
    // node's box on the axis they differ along.
    const Grab g{{0.0f, 0.0f, node_radius}, 0.25f, {0.3f, 0.0f, 0.0f}};
    add_grab(doc, node, g);
    const Box node_before = node_bound(doc, node);

    const Box b = undo_bound(doc);
    REQUIRE(b.has == 1);
    REQUIRE(b.infinite == 0);

    // Both ends of the pull -- the centre's ball and the displaced end's --
    // each dilated by the fold's whole support, as far as the node's own
    // bound reaches: past it the node's field is beyond the band on both sides
    // of the undo, which is the contract every influence bound already makes.
    // The node sits at the origin with no rotation or scale, so its frame is
    // the world's.
    const float end[3] = {g.centre[0] + g.pull[0], g.centre[1] + g.pull[1],
                          g.centre[2] + g.pull[2]};
    const Box ends = hull(ball_box(g.centre, g.radius), ball_box(end, g.radius));
    CHECK(b.contains(clip(dilated(ends, fold_support), node_before)));

    // ...and NO LARGER than the node's own bound: this narrows the command's
    // target, it never widens it.
    CHECK(node_before.contains(b));
    // The point of the change: a small fraction of the node, not the node.
    CHECK(b.volume() < 0.05f * node_before.volume());
}

TEST_CASE("redo of one grab reports the same box its undo did") {
    Doc doc;
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const clay_node_id node = add_sphere(doc, 2.0f, at);
    add_chain(doc, node, 2.0f, 4);
    add_grab(doc, node, Grab{{0.0f, 0.0f, 2.0f}, 0.25f, {0.0f, 0.0f, 0.2f}});
    const Box u = undo_bound(doc);
    const Box r = redo_bound(doc);
    CHECK(same_box(u, r));
}

// ---------------------------------------------------------------------------
// the count
// ---------------------------------------------------------------------------

namespace {

// The issue's shape: a node of radius `node_radius` whose TOP sits at z = 1.5
// whatever its size, carrying `chain` grabs around its equator, and one more
// small grab on that top. Returns the bricks undoing that one grab marks.
struct UndoCount {
    std::uint64_t by_undo = 0;
    std::uint64_t by_node = 0;
};

UndoCount undo_one_grab(float node_radius, int chain) {
    Doc doc;
    const float at[3] = {0.0f, 0.0f, 1.5f - node_radius};
    const clay_node_id node = add_sphere(doc, node_radius, at);
    add_chain(doc, node, node_radius, chain);
    add_grab(doc, node, Grab{{0.0f, 0.0f, node_radius}, 0.2f, {0.0f, 0.0f, 0.1f}});
    UndoCount c;
    c.by_node = bricks_marked(node_bound(doc, node));
    c.by_undo = bricks_marked(undo_bound(doc));
    return c;
}

}  // namespace

TEST_CASE("undoing one grab marks a count independent of the node's size and its chain") {
    // The grab's own ball at both ends, marked directly: the floor.
    const float c[3] = {0.0f, 0.0f, 1.5f};
    const float e[3] = {0.0f, 0.0f, 1.6f};
    const std::uint64_t ball = bricks_marked(hull(ball_box(c, 0.2f), ball_box(e, 0.2f)));

    const UndoCount small = undo_one_grab(1.5f, 1);
    const UndoCount large = undo_one_grab(6.0f, 1);
    const UndoCount long_chain = undo_one_grab(6.0f, 40);
    CAPTURE(ball);
    CAPTURE(small.by_undo);
    CAPTURE(small.by_node);
    CAPTURE(large.by_undo);
    CAPTURE(large.by_node);
    CAPTURE(long_chain.by_undo);
    CAPTURE(long_chain.by_node);

    // The node's own count grows with the node and with the chain: this is the
    // cost the issue measured, and the fixture has to show it or the equalities
    // below would hold for the wrong reason.
    REQUIRE(large.by_node > 4 * small.by_node);
    REQUIRE(long_chain.by_node > large.by_node);

    // The undo marks the SAME bricks at every size and chain length...
    CHECK(large.by_undo == small.by_undo);
    CHECK(long_chain.by_undo == small.by_undo);
    // ...and that number is the grab's ball, not the node.
    CHECK(small.by_undo >= ball);
    CHECK(small.by_undo <= 2 * ball);
    CHECK(long_chain.by_undo * 20 < long_chain.by_node);
}

// ---------------------------------------------------------------------------
// the oracle: never tighter than what changed
// ---------------------------------------------------------------------------

TEST_CASE("a grab undone at a smooth-union seam refills exactly what a rebuild does") {
    Doc doc;
    const float a[3] = {-0.45f, 0.0f, 0.0f};
    const float b[3] = {0.45f, 0.0f, 0.0f};
    add_sphere(doc, 0.5f, a);
    const clay_node_id node = add_sphere(doc, 0.5f, b, 0.3f);
    add_chain(doc, node, 0.5f, 3);
    // On the seam (world x = 0), pulling across it.
    add_grab(doc, node, Grab{{-0.45f, 0.0f, 0.3f}, 0.3f, {0.0f, 0.0f, 0.2f}});
    Box world;
    world.has = 1;
    for (int i = 0; i < 3; ++i) {
        world.lo[i] = -1.4f;
        world.hi[i] = 1.4f;
    }
    check_undo_and_redo(doc, world);
}

TEST_CASE("a grab undone on a mirrored layer refills its reflection as well") {
    Doc doc;
    REQUIRE(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.1f) == CLAY_OK);
    // Off the plane, so the item and its reflection are apart and a bound
    // covering only the item's own side would leave the reflection stale.
    const float at[3] = {0.8f, 0.0f, 0.0f};
    const clay_node_id node = add_sphere(doc, 0.5f, at);
    add_chain(doc, node, 0.5f, 3);
    add_grab(doc, node, Grab{{0.0f, 0.0f, 0.5f}, 0.25f, {0.0f, 0.0f, 0.2f}});
    Box world;
    world.has = 1;
    for (int i = 0; i < 3; ++i) {
        world.lo[i] = -1.6f;
        world.hi[i] = 1.6f;
    }
    check_undo_and_redo(doc, world);
}

TEST_CASE("a grab undone inside a blended group refills exactly what a rebuild does") {
    Doc doc;
    // A root node beneath the group, so the group's blend really combines
    // (issue #515) and its support is a term.
    const float under[3] = {0.0f, -0.9f, 0.0f};
    add_sphere(doc, 0.5f, under);
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, 0.3f,
                                 0.0f, &group) == CLAY_OK);
    const float sib[3] = {-0.6f, 0.0f, 0.0f};
    add_sphere(doc, 0.4f, sib, 0.0f, &group);
    const float at[3] = {0.3f, 0.0f, 0.0f};
    const clay_node_id node = add_sphere(doc, 0.5f, at, 0.0f, &group);
    add_chain(doc, node, 0.5f, 3);
    // On top of the seam with the sibling (world x = -0.2), pulling up.
    add_grab(doc, node, Grab{{-0.5f, 0.3f, 0.0f}, 0.3f, {0.0f, 0.2f, 0.0f}});
    Box world;
    world.has = 1;
    for (int i = 0; i < 3; ++i) {
        world.lo[i] = -1.6f;
        world.hi[i] = 1.6f;
    }
    check_undo_and_redo(doc, world);
}

TEST_CASE("a grab undone under a smooth fold refills exactly what a rebuild does") {
    Doc doc;
    const float under[3] = {0.0f, 0.0f, -0.7f};
    fold_onto_base(doc, 0.3f, under);
    const float at[3] = {0.0f, 0.0f, 0.2f};
    const clay_node_id node = add_sphere(doc, 0.5f, at);
    add_chain(doc, node, 0.5f, 3);
    // On the neck the fold welds between the two layers, pulling outward.
    add_grab(doc, node, Grab{{0.45f, 0.0f, -0.3f}, 0.3f, {0.2f, 0.0f, 0.0f}});
    Box world;
    world.has = 1;
    for (int i = 0; i < 3; ++i) {
        world.lo[i] = -1.4f;
        world.hi[i] = 1.4f;
    }
    check_undo_and_redo(doc, world);
}

TEST_CASE("a grab whose ball crosses the node's own box refills exactly what a rebuild does") {
    Doc doc;
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const clay_node_id node = add_sphere(doc, 0.5f, at);
    // Centred OUTSIDE the node's box and reaching into it: the part of the ball
    // beyond the box sees no material before the pull and some after.
    add_grab(doc, node, Grab{{0.0f, 0.0f, 0.75f}, 0.5f, {0.0f, 0.0f, -0.3f}});
    add_grab(doc, node, Grab{{0.6f, 0.0f, 0.0f}, 0.4f, {0.3f, 0.0f, 0.0f}});
    Box world;
    world.has = 1;
    for (int i = 0; i < 3; ++i) {
        world.lo[i] = -1.4f;
        world.hi[i] = 1.4f;
    }
    // Both grabs, newest first -- each reaches past the node's box -- then both
    // back again.
    for (int i = 0; i < 2; ++i) check_exact(doc, world, undo_bound);
    for (int i = 0; i < 2; ++i) check_exact(doc, world, redo_bound);
}


// ---------------------------------------------------------------------------
// the other finite-support kinds, the host's own Move, and mixed steps
// ---------------------------------------------------------------------------

namespace {

Box cube(float half) {
    Box b;
    b.has = 1;
    for (int i = 0; i < 3; ++i) {
        b.lo[i] = -half;
        b.hi[i] = half;
    }
    return b;
}

void add_deformer(Doc& doc, clay_node_id node, int32_t kind, const std::vector<float>& params,
                  int32_t ease = CLAY_EASE_LINEAR) {
    REQUIRE(clay_layer_add_deformer(doc.d, doc.layer, node, kind, params.data(), params.size(),
                                    ease, 1) == CLAY_OK);
}

// The undo marked something, and at least `factor` times fewer bricks than
// the node's bound.
void check_narrowed(const UndoCount& c, std::uint64_t factor) {
    CAPTURE(c.by_undo);
    CAPTURE(c.by_node);
    CHECK((c.by_undo > 0 && c.by_undo * factor < c.by_node));
}

// A sphere at the origin carrying one alpha stamp on its +z pole.
clay_node_id add_alpha_sphere(Doc& doc, float r) {
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(item != nullptr);
    std::vector<float> stamp(16 * 16);
    for (std::size_t i = 0; i < stamp.size(); ++i) stamp[i] = static_cast<float>(i % 7) / 7.0f;
    const float centre[3] = {0.0f, 0.0f, r};
    const float dir[3] = {0.0f, 0.0f, 1.0f};
    const float tangent[3] = {1.0f, 0.0f, 0.0f};
    ok(clay_item_add_alpha(item, stamp.data(), 16, 16, centre, dir, tangent, 0.4f, 0.3f, 0.05f,
                           CLAY_EASE_LINEAR));
    clay_node_id node = 0;
    const clay_result added = clay_layer_add_item(doc.d, doc.layer, item, &node);
    clay_item_destroy(item);
    ok(added);
    return node;
}

// One segment of the host's Move: a clay_layer_move_surface call that reaches
// exactly one item.
void move_segment(Doc& doc, const float centre[3], const float pull[3], float radius) {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.radius = radius;
    std::size_t applied = 0;
    ok(clay_layer_move_surface(doc.d, doc.layer, centre, pull, &p, &applied));
    CHECK(applied == 1);
}

// Undo one step and report both the bricks it marked and the node's.
UndoCount count_undo(Doc& doc, clay_node_id node) {
    UndoCount c;
    c.by_node = bricks_marked(node_bound(doc, node));
    c.by_undo = bricks_marked(undo_bound(doc));
    return c;
}

}  // namespace

TEST_CASE("magnify and blob at the head are narrowed too, and stay exact") {
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const std::vector<std::pair<int32_t, std::vector<float>>> heads = {
        {CLAY_DEFORM_MAGNIFY, {0.0f, 0.0f, 0.6f, 0.3f, 0.5f}},
        {CLAY_DEFORM_BLOB, {0.0f, 0.0f, 0.6f, 0.3f, 0.08f, 6.0f, 3.0f, 0.5f, 7.0f}},
    };
    for (const auto& [kind, params] : heads) {
        CAPTURE(kind);
        Doc doc;
        const clay_node_id node = add_sphere(doc, 0.6f, at);
        add_chain(doc, node, 0.6f, 3);
        add_deformer(doc, node, kind, params);
        check_undo_and_redo(doc, cube(1.4f));
        // Narrowed, and not to nothing: the step is undone again for the count.
        check_narrowed(count_undo(doc, node), 2);
    }
}

TEST_CASE("an alpha taken off the head of a chain is narrowed and stays exact") {
    // The one kind clay_layer_add_deformer does not take: it rides the item
    // builder, and a placed node loses it through clay_layer_remove_deformer,
    // which is a SetDeformersCmd like any other.
    Doc doc;
    const clay_node_id node = add_alpha_sphere(doc, 1.5f);
    add_chain(doc, node, 1.5f, 3);
    // The alpha is at the TAIL now; take off the three grabs ahead of it and
    // then the alpha itself, which is then the head.
    for (int i = 0; i < 4; ++i) ok(clay_layer_remove_deformer(doc.d, doc.layer, node, 0));
    check_undo_and_redo(doc, cube(2.0f));
    check_narrowed(count_undo(doc, node), 10);
}

TEST_CASE("a head the argument does not cover keeps the node's bound") {
    const float at[3] = {0.0f, 0.0f, 0.0f};
    SUBCASE("a radial pose, which is not the identity outside its ball") {
        Doc doc;
        const clay_node_id node = add_sphere(doc, 0.6f, at);
        add_chain(doc, node, 0.6f, 3);
        add_deformer(doc, node, CLAY_DEFORM_POSE, {0.0f, 0.0f, 0.6f, 0.3f, 0.0f, 1.0f, 0.0f, 0.5f});
        const UndoCount c = count_undo(doc, node);
        CHECK(c.by_undo == c.by_node);
    }
    SUBCASE("a grab behind a twist, whose ball the twist has moved") {
        Doc doc;
        const clay_node_id node = add_sphere(doc, 0.6f, at);
        add_deformer(doc, node, CLAY_DEFORM_TWIST, {0.5f});
        const float p[8] = {0.0f, 0.0f, 0.6f, 0.3f, 0.0f, 0.0f, 0.2f, 0.0f};
        REQUIRE(clay_layer_add_deformer(doc.d, doc.layer, node, CLAY_DEFORM_GRAB, p, 8,
                                        CLAY_EASE_LINEAR, /*at_front=*/0) == CLAY_OK);
        const UndoCount c = count_undo(doc, node);
        CHECK(c.by_undo == c.by_node);
    }
}

TEST_CASE("a grab on an intersecting item is narrowed and stays exact") {
    // An intersect's influence is its layer's extent, not its own box. The
    // grab's argument does not care -- the item's own field is untouched
    // outside the ball and max() is pointwise -- and the oracle says so.
    Doc doc;
    const float body[3] = {0.0f, 0.0f, 0.0f};
    add_sphere(doc, 0.7f, body);
    const float cut_at[3] = {0.0f, 0.0f, 0.3f};
    float cr = 0.7f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &cr, 1);
    REQUIRE(clay_item_set_position(item, cut_at) == CLAY_OK);
    REQUIRE(clay_item_set_op(item, CLAY_OP_INTERSECT) == CLAY_OK);
    clay_node_id cutter = 0;
    REQUIRE(clay_layer_add_item(doc.d, doc.layer, item, &cutter) == CLAY_OK);
    clay_item_destroy(item);
    add_grab(doc, cutter, Grab{{0.0f, 0.0f, -0.7f}, 0.3f, {0.0f, 0.0f, -0.15f}});
    check_undo_and_redo(doc, cube(1.4f));
}

TEST_CASE("a step that also carries another command still covers that command") {
    Doc doc;
    const float a_at[3] = {-0.6f, 0.0f, 0.0f};
    const float b_at[3] = {0.7f, 0.0f, 0.0f};
    const clay_node_id a = add_sphere(doc, 0.4f, a_at);
    const clay_node_id b = add_sphere(doc, 0.3f, b_at);
    REQUIRE(clay_document_begin_undo_group(doc.d) == CLAY_OK);
    add_grab(doc, a, Grab{{0.0f, 0.4f, 0.0f}, 0.2f, {0.0f, 0.15f, 0.0f}});
    const float axis[3] = {0.0f, 1.0f, 0.0f};
    const float moved[3] = {0.7f, 0.0f, 0.6f};
    REQUIRE(clay_layer_set_transform(doc.d, doc.layer, b, moved, axis, 0.0f, 1.0f) == CLAY_OK);
    REQUIRE(clay_document_end_undo_group(doc.d) == CLAY_OK);
    // The transform's two ends are nowhere near the grab's ball: a bound that
    // narrowed the whole step to the grab would leave them stale.
    check_undo_and_redo(doc, cube(1.4f));
}

TEST_CASE("undoing one segment of the host's Move marks the segment, not the node") {
    // The path the issue's host takes: clay_layer_move_surface, one call per
    // segment, each leaving grabs at the head of the chains it reached.
    Doc doc;
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const float node_radius = 1.5f;
    const clay_node_id node = add_sphere(doc, node_radius, at);
    for (int i = 0; i < 12; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / 12.0f;
        const float c[3] = {node_radius * std::cos(t), node_radius * std::sin(t), 0.0f};
        const float d[3] = {0.08f * std::cos(t), 0.08f * std::sin(t), 0.0f};
        move_segment(doc, c, d, 0.2f);
    }
    const float top[3] = {0.0f, 0.0f, node_radius};
    const float up[3] = {0.0f, 0.0f, 0.08f};
    move_segment(doc, top, up, 0.2f);

    const float end[3] = {top[0] + up[0], top[1] + up[1], top[2] + up[2]};
    const std::uint64_t ball = bricks_marked(hull(ball_box(top, 0.2f), ball_box(end, 0.2f)));
    const UndoCount c = count_undo(doc, node);
    CAPTURE(ball);
    CHECK((c.by_undo >= ball && c.by_undo <= 2 * ball));
    check_narrowed(c, 20);
    redo_bound(doc);
    check_undo_and_redo(doc, cube(2.0f));
}

namespace {

enum class Tail { lattice, curve, twist };

// A sphere whose chain ends in a whole-item link -- a lattice cage, a bend
// curve or a twist -- with a short grab chain added AHEAD of it, the way a
// Move lands on a node that already carries one.
clay_node_id add_tailed_sphere(Doc& doc, Tail tail) {
    const float at[3] = {0.0f, 0.0f, 0.0f};
    const clay_node_id node = add_sphere(doc, 0.6f, at);
    if (tail == Tail::twist) {
        const float k = 0.5f;
        ok(clay_layer_add_deformer(doc.d, doc.layer, node, CLAY_DEFORM_TWIST, &k, 1,
                                   CLAY_EASE_LINEAR, 0));
    } else if (tail == Tail::lattice) {
        const float lo[3] = {-0.7f, -0.7f, -0.7f};
        const float hi[3] = {0.7f, 0.7f, 0.7f};
        std::vector<float> offsets(2 * 2 * 2 * 3, 0.0f);
        offsets[0] = 0.1f;  // one corner dragged, so the cage is not the identity
        ok(clay_layer_add_lattice(doc.d, doc.layer, node, lo, hi, 2, 2, 2, offsets.data(), 0));
    } else {
        const float guide[9] = {0.0f, -0.7f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.7f, 0.0f};
        ok(clay_layer_add_bend_curve(doc.d, doc.layer, node, guide, 3, 0, 0.0f, 1.0f, 0));
    }
    add_chain(doc, node, 0.6f, 3);
    add_grab(doc, node, Grab{{0.0f, 0.0f, 0.6f}, 0.2f, {0.0f, 0.0f, 0.1f}});
    return node;
}

}  // namespace

TEST_CASE("a grab ahead of a twist, a lattice or a bend curve in the common tail is narrowed") {
    // The tail is the same on both sides of the step and sees the same point,
    // whatever it does with it; a whole-item link there -- a payload link
    // included -- must be stripped like any other rather than refuse.
    for (const Tail tail : {Tail::lattice, Tail::curve, Tail::twist}) {
        CAPTURE(static_cast<int>(tail));
        Doc doc;
        const clay_node_id node = add_tailed_sphere(doc, tail);
        check_undo_and_redo(doc, cube(1.4f));
        check_narrowed(count_undo(doc, node), 4);
    }
}

TEST_CASE("a ball that misses the node's box but lies within the band of it is still refilled") {
    // The node's bound is reported WITHOUT the band -- every consumer adds it
    // (mark_dirty dilates by the band). So the field can change in the band
    // OUTSIDE that box, and a link whose ball sits there changes it. Cutting
    // the ball down to the part inside the box leaves nothing to dirty; the
    // band the consumer adds must be able to reach the ball from what is
    // reported. Found by a randomized oracle: a magnify just past a node's
    // face, undone, reported no bounds and left one brick stale.
    const float at[3] = {0.0f, 0.0f, 0.0f};
    for (const int32_t kind : {CLAY_DEFORM_MAGNIFY, CLAY_DEFORM_GRAB}) {
        CAPTURE(kind);
        Doc doc;
        const clay_node_id node = add_sphere(doc, 0.5f, at);
        const Box before = node_bound(doc, node);
        if (kind == CLAY_DEFORM_MAGNIFY)
            add_deformer(doc, node, kind, {0.0f, 0.0f, 0.62f, 0.1f, -0.6f});
        else
            add_deformer(doc, node, kind, {0.0f, 0.0f, 0.66f, 0.08f, 0.0f, 0.0f, -0.03f, 0.0f});
        // The fixture's premise: the ball is clear of the node's box on z, and
        // within the band (3 voxels, 0.15) of it.
        const float ball_lo = kind == CLAY_DEFORM_MAGNIFY ? 0.52f : 0.58f;
        const float top = node_bound(doc, node).hi[2];
        CAPTURE(top);
        REQUIRE((top < ball_lo && before.hi[2] < ball_lo));
        REQUIRE(ball_lo - top < 0.15f);
        check_undo_and_redo(doc, cube(1.0f));
    }
}

TEST_CASE("a ball past the node's band but inside a later sibling's blend is still refilled") {
    // Issue #650. The ball is clamped into the node's bound, and the node's
    // bound used to stop at the node's own box: outside it the node's value is
    // beyond the band, which a hard union leaves alone. A SMOOTH sibling after
    // the node does not -- it reads the running value out to its support, and
    // the node is the running value wherever it is the nearest thing. So a
    // grab well clear of the node's band, sitting on the sibling's surface,
    // moves the sibling's fillet, and a bound clamped short of it left bricks
    // stale. Seed 5128 of the undo-bound oracle is the same mechanism inside a
    // blended group, at one ulp.
    //
    // Both shapes: at the layer root, and inside a group that does not combine
    // (nothing beneath it, #515), the shape the seed has.
    for (const bool in_group : {false, true}) {
        CAPTURE(in_group);
        Doc doc;
        clay_node_id group = 0;
        if (in_group)
            REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD,
                                         CLAY_BLEND_QUADRATIC, 0.2f, 0.0f, &group) == CLAY_OK);
        const clay_node_id* parent = in_group ? &group : nullptr;
        const float at[3] = {0.0f, 0.0f, 0.0f};
        const clay_node_id node = add_sphere(doc, 0.5f, at, 0.0f, parent);
        const float sib[3] = {1.3f, 0.0f, 0.0f};
        add_sphere(doc, 0.5f, sib, 0.3f, parent);
        // On the sibling's near surface (x = 0.8), clear of the node's box
        // (x <= 0.5) by more than the band.
        add_deformer(doc, node, CLAY_DEFORM_GRAB,
                     {0.8f, 0.0f, 0.0f, 0.12f, -0.1f, 0.0f, 0.0f, 0.0f});
        REQUIRE(0.8f - 0.12f - 0.5f > 0.15f);
        check_undo_and_redo(doc, cube(2.0f));
    }
}
