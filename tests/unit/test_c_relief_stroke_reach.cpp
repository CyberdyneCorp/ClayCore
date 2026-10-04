#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"

// Where an edit to one stamp of a relief or incise stroke lands (issue #672).
//
// One clay_layer_apply_stroke with CLAY_OP_RELIEF authors one node per stamp.
// Through 0.122.0 every stamp but the LAST reported an influence bound widened
// by 4 x strength world units on each side, whatever the radius: the #650 term
// for the combines after a node read the next stamp's relief AMPLITUDE (its
// blend.k) as a quadratic blend support. A four-sample stroke on a unit sphere
// dirtied 2,548 bricks where one stamp dirties 48, and the host re-meshed the
// whole form.
//
// A relief combine is `a - k * w(b)` with w zero outside the item's own bound,
// so editing a relief node leaves the running value BIT-IDENTICAL outside that
// bound, and every combine after it is a function of its operands at the same
// point. There is no beyond-band difference for a later smooth combine to
// carry, and the stamp's own bound is the whole answer.
//
// Both halves are pinned through the C ABI, which is the path a host takes:
//
//   - the COUNT: every stamp is bounded within the stroke's footprint, and the
//     stroke dirties on the order of one stamp's bricks;
//   - the ORACLE: a cache refilled over the reported bounds after removing or
//     moving any one stamp is bit-identical to a cache rebuilt from nothing --
//     at the root, inside a smooth group followed by a smooth sibling, and on
//     a mirrored layer.

namespace {

// Fine enough that a bound 0.1 too tight leaves a brick stale -- checked by
// shrinking the reported box while writing this: at 0.05 voxels the bricks
// were coarse enough to hide a 0.1 shrink and caught only 0.3.
constexpr float kVoxel = 0.025f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr int kBandVoxels = 3;

// The issue's stroke: radius 0.12, strength 0.5, four samples x = 0..0.09 on
// the pole of a unit sphere.
constexpr float kRadius = 0.12f;
constexpr float kStrength = 0.5f;

void ok(clay_result r) { REQUIRE(r == CLAY_OK); }

struct Box {
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};
    std::int32_t has = 0;
    std::int32_t infinite = 0;
    float width(int axis) const { return hi[axis] - lo[axis]; }
};

void expand(Box* into, const Box& b) {
    if (b.has == 0) return;
    REQUIRE(b.infinite == 0);
    if (into->has == 0) {
        *into = b;
        return;
    }
    for (int a = 0; a < 3; ++a) {
        into->lo[a] = std::fmin(into->lo[a], b.lo[a]);
        into->hi[a] = std::fmax(into->hi[a], b.hi[a]);
    }
}

struct Doc {
    clay_document* d = nullptr;
    clay_layer_id layer = 0;
    clay_node_id base = 0;
    std::vector<clay_node_id> stamps;
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
    explicit Cache(bool host_defaults = false) {
        clay_brick_config cfg;
        cfg.struct_size = sizeof(cfg);
        REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
        if (!host_defaults) {
            cfg.dim = kDim;
            cfg.voxel_size = kVoxel;
            cfg.band_voxels = kBandVoxels;
        }
        cfg.memory_budget = 0;
        c = clay_brick_cache_create(&cfg);
        REQUIRE(c != nullptr);
    }
    ~Cache() { clay_brick_cache_destroy(c); }
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
};

clay_node_id add_sphere(Doc& doc, float radius, const float pos[3], float blend_k = 0.0f) {
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1);
    REQUIRE(item != nullptr);
    ok(clay_item_set_position(item, pos));
    if (blend_k > 0.0f) ok(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, blend_k));
    clay_node_id node = 0;
    const clay_result r = clay_layer_add_item(doc.d, doc.layer, item, &node);
    clay_item_destroy(item);
    ok(r);
    return node;
}

// The host's relief template (ClaySpaceIOS FieldSculpt.swift): a unit sphere,
// quadratic k = 1 and rounding 1, so a stamp's amplitude is k * strength in
// world units and its falloff is its radius.
void apply_stroke(Doc& doc, std::int32_t op, int sample_count = 4) {
    const float unit = 1.0f;
    clay_item* stamp = clay_item_create(CLAY_PRIM_SPHERE, &unit, 1);
    REQUIRE(stamp != nullptr);
    ok(clay_item_set_op(stamp, op));
    ok(clay_item_set_blend(stamp, CLAY_BLEND_QUADRATIC, 1.0f));
    ok(clay_item_set_rounding(stamp, 1.0f));

    clay_stroke_preset p;
    p.struct_size = sizeof(p);
    ok(clay_stroke_preset_defaults(&p));
    p.radius = kRadius;
    p.strength = kStrength;

    std::vector<float> samples;
    for (int i = 0; i < sample_count; ++i) {
        const float s[5] = {0.03f * static_cast<float>(i), 0.0f, 1.0f, 1.0f, 0.0f};
        samples.insert(samples.end(), s, s + 5);
    }
    const auto n = static_cast<std::size_t>(sample_count);
    std::size_t count = 0;
    ok(clay_stroke_resolve(samples.data(), n, &p, nullptr, &count));
    REQUIRE(count >= 2);  // several stamps, or there is no "earlier" one
    doc.stamps.assign(count, 0);
    ok(clay_layer_apply_stroke(doc.d, doc.layer, samples.data(), n, &p, stamp, nullptr,
                               doc.stamps.data(), &count));
    REQUIRE(count == doc.stamps.size());
    clay_item_destroy(stamp);
}

// The issue's document: one unit sphere and one stroke.
void build_plain(Doc& doc, std::int32_t op) {
    const float origin[3] = {0, 0, 0};
    doc.base = add_sphere(doc, 1.0f, origin);
    apply_stroke(doc, op);
}

// A longer stroke, so some stamps have stamps both before and after them.
void build_long(Doc& doc, std::int32_t op) {
    const float origin[3] = {0, 0, 0};
    doc.base = add_sphere(doc, 1.0f, origin);
    apply_stroke(doc, op, /*sample_count=*/12);
    REQUIRE(doc.stamps.size() >= 4);
}

// The sphere and its stroke INSIDE a smooth group, with a smooth sibling after
// the group: both terms the #650 walk adds at a level are present. The sphere
// goes in too -- a relief that opens a chain has nothing to offset.
void build_grouped(Doc& doc, std::int32_t op) {
    build_plain(doc, op);
    clay_node_id group = 0;
    ok(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, 0.3f,
                            0.0f, &group));
    ok(clay_layer_move(doc.d, doc.layer, doc.base, group, -1));
    for (clay_node_id id : doc.stamps) ok(clay_layer_move(doc.d, doc.layer, id, group, -1));
    const float beside[3] = {0.3f, 0.0f, 1.1f};
    add_sphere(doc, 0.2f, beside, /*blend_k=*/0.3f);
}

void build_mirrored(Doc& doc, std::int32_t op) {
    ok(clay_set_layer_mirror(doc.d, doc.layer, 1, 0, 0, 0.1f));
    build_plain(doc, op);
}

Box node_bound(const Doc& doc, clay_node_id node) {
    Box b;
    ok(clay_layer_node_influence_bound(doc.d, doc.layer, node, b.lo, b.hi, &b.has, &b.infinite));
    REQUIRE(b.infinite == 0);
    return b;
}

std::uint64_t dirty_count(const Cache& cache) {
    clay_brick_stats stats;
    std::memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    ok(clay_brick_cache_stats(cache.c, &stats));
    return stats.dirty_bricks;
}

// What clay_brick_cache_mark_dirty_nodes marks for `nodes`, in a fresh cache
// at the host's default brick configuration -- the count the issue measured.
std::uint64_t bricks_for(const Doc& doc, const std::vector<clay_node_id>& nodes) {
    Cache cache(/*host_defaults=*/true);
    std::size_t marked = 0;
    ok(clay_brick_cache_mark_dirty_nodes(cache.c, doc.d, doc.layer, nodes.data(), nodes.size(),
                                         &marked));
    REQUIRE(marked == nodes.size());
    return dirty_count(cache);
}

// -- the brick oracle ---------------------------------------------------------

void mark(const Cache& cache, const Box& b) {
    REQUIRE(b.infinite == 0);
    if (b.has == 1) ok(clay_brick_cache_mark_dirty(cache.c, b.lo, b.hi));
}

void refill(const Cache& cache, const clay_document* doc) {
    constexpr std::size_t chunk = 64;
    std::vector<clay_brick_request> reqs(chunk);
    std::vector<float> values(chunk * kSamples);
    std::vector<std::int32_t> results(chunk);
    for (;;) {
        std::size_t count = chunk, remaining = 0;
        ok(clay_brick_cache_take_dirty(cache.c, reqs.data(), &count, &remaining));
        if (count == 0) break;
        ok(clay_brick_cache_eval_requests(doc, nullptr, reqs.data(), count, values.data(),
                                          count * kSamples, nullptr, 0));
        std::size_t accepted = 0;
        ok(clay_brick_cache_submit(cache.c, reqs.data(), count, values.data(), count * kSamples,
                                   nullptr, 0, results.data(), &accepted));
        if (remaining == 0) break;
    }
}

// The whole sphere and the band around it, up to the top of the relief: the
// stamps raise the pole by up to their amplitude, 0.5.
Box world() {
    Box b;
    b.has = 1;
    for (int a = 0; a < 3; ++a) {
        b.lo[a] = -1.2f;
        b.hi[a] = 1.2f;
    }
    b.hi[2] = 1.8f;
    return b;
}

std::vector<std::int32_t> keys_in(const Box& w) {
    const float brick = kVoxel * static_cast<float>(kDim);
    int lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int>(std::floor(w.lo[a] / brick));
        hi[a] = static_cast<int>(std::floor(w.hi[a] / brick));
    }
    std::vector<std::int32_t> keys;
    for (int z = lo[2]; z <= hi[2]; ++z)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int x = lo[0]; x <= hi[0]; ++x) keys.insert(keys.end(), {x, y, z});
    return keys;
}

struct Snapshot {
    std::vector<std::int32_t> states;
    std::vector<std::uint16_t> halves;
};

Snapshot snapshot(const Cache& cache, const std::vector<std::int32_t>& keys) {
    const std::size_t n = keys.size() / 3;
    Snapshot s;
    s.states.assign(n, -1);
    s.halves.assign(n * kSamples, 0);
    ok(clay_brick_cache_read_bricks(cache.c, 0, keys.data(), n, 0, s.states.data(),
                                    s.halves.data(), n * kSamples, nullptr, 0));
    return s;
}

std::size_t differing_bricks(const Snapshot& a, const Snapshot& b) {
    std::size_t diff = 0;
    for (std::size_t i = 0; i < a.states.size(); ++i) {
        const bool same_values = std::memcmp(&a.halves[i * kSamples], &b.halves[i * kSamples],
                                             kSamples * sizeof(std::uint16_t)) == 0;
        if (a.states[i] != b.states[i] || !same_values) ++diff;
    }
    return diff;
}

// A cache built from nothing on a COPY of the document: the seed store is the
// document's, so a rebuild on the same document resumes from the very seeds a
// too-narrow bound failed to drop and agrees with the stale bricks.
Snapshot rebuilt(const Doc& doc, const std::vector<std::int32_t>& keys) {
    clay_blob* blob = nullptr;
    ok(clay_document_save_memory(doc.d, &blob));
    clay_document* copy = nullptr;
    const clay_result r =
        clay_document_load_memory(clay_blob_data(blob), clay_blob_size(blob), &copy);
    clay_blob_destroy(blob);
    ok(r);
    Cache fresh;
    mark(fresh, world());
    refill(fresh, copy);
    Snapshot s = snapshot(fresh, keys);
    clay_document_destroy(copy);
    return s;
}

using Build = void (*)(Doc&, std::int32_t);

enum class Edit { Remove, Move };

// Apply `edit` to stamp `index` and return the box a host dirties for it: the
// stamp's reported bound before the edit, unioned with it after.
Box edit_stamp(Doc& doc, std::size_t index, Edit edit) {
    const clay_node_id node = doc.stamps[index];
    Box reported = node_bound(doc, node);
    if (edit == Edit::Remove) {
        ok(clay_remove_node(doc.d, doc.layer, node));
        return reported;
    }
    float pos[3], axis[3], angle = 0.0f, scale = 0.0f;
    ok(clay_layer_node_transform(doc.d, doc.layer, node, pos, axis, &angle, &scale));
    pos[1] += 0.05f;
    ok(clay_layer_set_transform(doc.d, doc.layer, node, pos, axis, angle, scale));
    expand(&reported, node_bound(doc, node));
    return reported;
}

struct OracleRun {
    std::size_t stale = 0;     // bricks the reported bound left wrong
    std::size_t changed = 0;   // bricks the edit changed at all
    std::size_t unmarked = 0;  // bricks left wrong when NOTHING is marked
};

OracleRun run_oracle(Build build, std::int32_t op, std::size_t index, Edit edit) {
    Doc doc;
    build(doc, op);
    const std::vector<std::int32_t> keys = keys_in(world());
    Cache cache;
    mark(cache, world());
    refill(cache, doc.d);
    const Snapshot before = snapshot(cache, keys);

    Cache untouched;  // the same fill, never told about the edit
    mark(untouched, world());
    refill(untouched, doc.d);

    const Box reported = edit_stamp(doc, index, edit);
    mark(cache, reported);
    refill(cache, doc.d);
    const Snapshot truth = rebuilt(doc, keys);

    OracleRun out;
    out.stale = differing_bricks(snapshot(cache, keys), truth);
    out.changed = differing_bricks(before, truth);
    out.unmarked = differing_bricks(snapshot(untouched, keys), truth);
    return out;
}

void check_every_stamp(Build build, std::int32_t op) {
    Doc probe;
    build(probe, op);
    const std::size_t stamps = probe.stamps.size();
    for (std::size_t i = 0; i < stamps; ++i) {
        for (Edit edit : {Edit::Remove, Edit::Move}) {
            CAPTURE(i);
            CAPTURE(static_cast<int>(edit));
            const OracleRun run = run_oracle(build, op, i, edit);
            CHECK(run.changed > 0);   // the edit is visible at all
            CHECK(run.unmarked > 0);  // and the oracle can see a missing mark
            CHECK(run.stale == 0);
        }
    }
}

}  // namespace

TEST_CASE("every stamp of a relief or incise stroke is bounded by its own reach (#672)") {
    for (std::int32_t op : {CLAY_OP_RELIEF, CLAY_OP_INCISE}) {
        CAPTURE(op);
        Doc doc;
        build_plain(doc, op);
        const Box last = node_bound(doc, doc.stamps.back());
        REQUIRE(last.has == 1);
        // The last stamp is local: about 3 x radius each side.
        CHECK(last.width(0) < 8.0f * kRadius);

        // The stroke's footprint: every stamp's centre (x = 0..0.09) widened by
        // what the last stamp, which nothing follows, reports for itself.
        const float half = 0.5f * last.width(0);
        for (std::size_t i = 0; i < doc.stamps.size(); ++i) {
            CAPTURE(i);
            const Box b = node_bound(doc, doc.stamps[i]);
            REQUIRE(b.has == 1);
            // Measured before the fix: 0.72 + 2 * 4 * strength = 4.72.
            CHECK(b.width(0) <= last.width(0) * 1.0001f);
            CHECK(b.lo[0] >= -half - 1e-4f);
            CHECK(b.hi[0] <= 0.09f + half + 1e-4f);
            CHECK(b.width(1) <= last.width(1) * 1.0001f);
            CHECK(b.width(2) <= last.width(2) * 1.0001f);
        }

        // The cost a host pays: the stroke drains on the order of ONE stamp's
        // bricks, not the sphere's. Measured before: 2,548 against 48.
        const std::uint64_t one = bricks_for(doc, {doc.stamps.back()});
        const std::uint64_t stroke = bricks_for(doc, doc.stamps);
        INFO("one stamp marks " << one << " bricks, the stroke " << stroke);
        REQUIRE(one > 0);
        CHECK(stroke <= 2 * one);
    }
}

TEST_CASE("a relief stamp's own bound holds an edit to it, bit for bit (#672)") {
    // The soundness half: the narrow answer is only worth having if a cache
    // refilled over it is exactly a cache built from nothing. Each stamp is
    // removed, and separately moved off the stroke's axis, and the host's
    // refill is compared brick by brick.
    for (std::int32_t op : {CLAY_OP_RELIEF, CLAY_OP_INCISE}) {
        CAPTURE(op);
        SUBCASE("at the layer root") { check_every_stamp(build_plain, op); }
        SUBCASE("a longer stroke, middle stamps included") { check_every_stamp(build_long, op); }
        SUBCASE("inside a smooth group, with a smooth sibling after it") {
            check_every_stamp(build_grouped, op);
        }
        SUBCASE("on a mirrored layer") { check_every_stamp(build_mirrored, op); }
    }
}

TEST_CASE("a node BEFORE a relief stroke still takes the stroke's drag (#672)") {
    // What the fix does not touch: the sphere the stroke is laid on is the
    // running value its stamps offset, and an edit to it is still carried
    // through them. Its bound keeps the #650 term.
    Doc doc;
    const float origin[3] = {0, 0, 0};
    const clay_node_id sphere = add_sphere(doc, 1.0f, origin);
    const Box alone = node_bound(doc, sphere);
    apply_stroke(doc, CLAY_OP_RELIEF);
    const Box dragged = node_bound(doc, sphere);
    CHECK(dragged.width(0) > alone.width(0));
}
