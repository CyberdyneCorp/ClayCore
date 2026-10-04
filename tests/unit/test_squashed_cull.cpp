// A squashed placement is culled for the field it emits, not for a distance
// (issue #649, mechanism B).
//
// A per-axis scale -- an item's or a layer's -- makes the field a BOUND on the
// distance rather than the distance: the tape evaluates at `p / s` and
// multiplies back by min(s) (`cscale_nu_dist`), so the value can be short of
// the true distance by up to q = max(s) / min(s). The per-brick cull argued
// "the item's bound is more than band + pad from the brick, so its field there
// is more than band + pad", and that holds only out to q * (band + pad) for a
// squashed one. A brick in between dropped the item although its field was
// inside the band there, and the refill disagreed with clay_eval_points.
//
// Driven through the C ABI refill and clay_eval_points, so the same source
// compiled against the pre-fix library and failed there: random documents in
// `undo_bound_oracle_probe` (seeds 5743 and 6290, `rich`) shrink to this.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay.h"
#include "clay/scene/bounds.h"
#include "clay/scene/cull_index.h"
#include "clay/scene/tape.h"
#include "kernel_utils.h"
#include "scene_utils.h"

namespace {

constexpr float kVoxel = 0.05f;
constexpr int kDim = 8;
constexpr std::size_t kSamples = 8 * 8 * 8;
constexpr float kBand = 3.0f * kVoxel;
constexpr float kWidth = kVoxel * static_cast<float>(kDim);

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

struct Ball {
    float radius = 0.5f;
    float x = 0.0f;
    float axes[3] = {1.0f, 1.0f, 1.0f};
    int32_t blend = CLAY_BLEND_HARD;
    float k = 0.0f;
    float rounding = 0.0f;
};

clay_node_id add_ball(Doc& doc, const Ball& b, clay_node_id group = 0) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &b.radius, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {b.x, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_item_set_scale_nonuniform(it, b.axes) == CLAY_OK);
    REQUIRE(clay_item_set_blend(it, b.blend, b.k) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(it, b.rounding) == CLAY_OK);
    clay_node_id id = 0;
    if (group)
        REQUIRE(clay_layer_add_item_in_group(doc.d, doc.layer, group, -1, it, &id) == CLAY_OK);
    else
        REQUIRE(clay_layer_add_item(doc.d, doc.layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

void squash_layer(Doc& doc, float sx) {
    const float pos[3] = {0.0f, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 0.0f, 1.0f};
    const float scale[3] = {sx, 1.0f, 1.0f};
    REQUIRE(clay_document_set_layer_transform_nonuniform(doc.d, doc.layer, pos, axis, 0.0f,
                                                         scale) == CLAY_OK);
}

// +X edge of the node's influence bound as the library reports it: the box the
// cull tested before this fix, with no allowance for the squash.
float influence_max_x(const Doc& doc, clay_node_id node) {
    float lo[3], hi[3];
    int32_t has = 0, inf = 0;
    REQUIRE(clay_layer_node_influence_bound(doc.d, doc.layer, node, lo, hi, &has, &inf) ==
            CLAY_OK);
    REQUIRE(has == 1);
    REQUIRE(inf == 0);
    return hi[0];
}

clay_brick_request brick_at(float x, float y, float z) {
    clay_brick_request r{};
    r.origin[0] = x;
    r.origin[1] = y;
    r.origin[2] = z;
    r.spacing = kVoxel;
    r.dims[0] = r.dims[1] = r.dims[2] = kDim;
    r.band = kBand;
    return r;
}

float clamp_band(float v) { return v < -kBand ? -kBand : (v > kBand ? kBand : v); }

struct Outcome {
    std::size_t reached = 0;   // in-band samples beyond the unsquashed band
    std::size_t disagree = 0;  // in-band samples the refill got wrong
    float worst = 0.0f;
};

// A row of bricks along +X whose band-dilated boxes all MISS the node's
// unsquashed bound -- every one of them was culled before -- refilled in one
// batch (so through the cull index's plan), and every in-band sample compared
// with the document's own raw field.
Outcome refill_past(const Doc& doc, float edge) {
    std::vector<clay_brick_request> reqs;
    const float x0 = edge + kBand + 0.01f;
    for (int i = 0; i < 3; ++i)
        for (float y : {-kWidth * 0.5f, -kWidth * 1.5f, kWidth * 0.5f})
            reqs.push_back(brick_at(x0 + kWidth * static_cast<float>(i), y, -kWidth * 0.5f));
    std::vector<float> values(reqs.size() * kSamples);
    REQUIRE(clay_brick_cache_eval_requests(doc.d, nullptr, reqs.data(), reqs.size(),
                                           values.data(), values.size(), nullptr, 0) == CLAY_OK);

    std::vector<float> pts;
    pts.reserve(values.size() * 3);
    for (const clay_brick_request& r : reqs)
        for (int k = 0; k < kDim; ++k)
            for (int j = 0; j < kDim; ++j)
                for (int i = 0; i < kDim; ++i) {
                    pts.push_back(r.origin[0] + kVoxel * static_cast<float>(i));
                    pts.push_back(r.origin[1] + kVoxel * static_cast<float>(j));
                    pts.push_back(r.origin[2] + kVoxel * static_cast<float>(k));
                }
    std::vector<float> raw(values.size());
    REQUIRE(clay_eval_points(doc.d, nullptr, pts.data(), raw.size(), raw.data(), nullptr) ==
            CLAY_OK);

    Outcome out;
    for (std::size_t s = 0; s < raw.size(); ++s) {
        if (std::fabs(raw[s]) > kBand) continue;
        if (pts[s * 3] > edge + kBand) ++out.reached;
        const float err = std::fabs(clamp_band(values[s]) - clamp_band(raw[s]));
        if (err > 1e-4f) ++out.disagree;
        if (err > out.worst) out.worst = err;
    }
    return out;
}

void check(const Outcome& o) {
    // The field really is inside the band out there -- otherwise every brick
    // would agree because there was nothing to drop, and this would pass on
    // the code it is meant to catch.
    REQUIRE(o.reached > 0);
    INFO("in-band samples off: " << o.disagree << ", worst " << o.worst);
    CHECK(o.disagree == 0);
}

}  // namespace

TEST_CASE("squashed cull: an item's per-axis scale keeps it in the bricks its field reaches") {
    Doc doc;
    Ball b;
    b.axes[0] = 4.0f;  // q = 4: the field at +X is a quarter of the distance
    const clay_node_id id = add_ball(doc, b);
    check(refill_past(doc, influence_max_x(doc, id)));
}

TEST_CASE("squashed cull: a layer's per-axis scale keeps its items in the bricks they reach") {
    Doc doc;
    const clay_node_id id = add_ball(doc, Ball{});
    squash_layer(doc, 4.0f);
    check(refill_past(doc, influence_max_x(doc, id)));
}

TEST_CASE("squashed cull: both levels, blended, rounded and grouped") {
    // q = 2 * 2. The item's own reach (rounding and blend support) and the
    // group's blend support are reached at q times their width too, and the
    // smooth chain gives the cull a pad, so every term of the dilation is live.
    Doc doc;
    Ball base;
    base.radius = 0.3f;
    base.x = -0.4f;
    add_ball(doc, base);
    clay_node_id group = 0;
    REQUIRE(clay_layer_add_group(doc.d, doc.layer, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC, 0.04f,
                                 0.0f, &group) == CLAY_OK);
    Ball b;
    b.axes[0] = 2.0f;
    b.blend = CLAY_BLEND_QUADRATIC;
    b.k = 0.06f;
    b.rounding = 0.02f;
    add_ball(doc, b, group);
    squash_layer(doc, 2.0f);
    check(refill_past(doc, influence_max_x(doc, group)));
}

// -- the planned path widens exactly as the walk does ------------------------
//
// The cull index caches each node's squash beside its bound, and the coarse
// plan widens a chain holding one by the widest entry's widening at the band it
// was planned for. Both are accelerations, so a planned compile must be
// BYTE-IDENTICAL to the plain one -- and a plan made for a narrower band than
// the region carries must be refused rather than trusted.

namespace {

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

void require_same_tape(const scene::Tape& a, const scene::Tape& b) {
    REQUIRE(a.instrs.size() == b.instrs.size());
    for (std::size_t i = 0; i < a.instrs.size(); ++i) {
        REQUIRE(a.instrs[i].op == b.instrs[i].op);
        REQUIRE(a.instrs[i].param_offset == b.instrs[i].param_offset);
    }
    REQUIRE(a.params == b.params);
    REQUIRE(a.blob == b.blob);
}

// gnarly_document with its body layer squashed and items squashed at the root,
// inside the nested groups and in the instanced copy that shares them.
scene::Document squashed_gnarly() {
    scene::Document doc = clay_test::gnarly_document();
    scene::Layer& body = doc.layers[0];
    body.scale_axes = cf3(2.5f, 0.6f, 1.0f);
    scene::SdfContent& c = *body.sdf;
    int touched = 0;
    for (const auto& [id, n] : c.nodes()) {
        if (n.is_group || (id % 3) != 0) continue;
        c.find_mut(id)->scale_axes = cf3(0.4f, 1.0f, 2.0f);
        ++touched;
    }
    REQUIRE(touched >= 3);
    return doc;
}

// Brick-sized boxes over the document. Each is planned ALONE: a plan over the
// union of scattered bricks covers the whole document and prunes nothing, and
// then a coarse cull that forgot the squash would go unseen.
std::vector<math::Aabb> random_bricks(std::uint64_t seed) {
    clay_test::Lcg rng(seed);
    std::vector<math::Aabb> out;
    for (int b = 0; b < 64; ++b) {
        const cfloat3 corner = rng.vec3(-3.0f, 3.0f);
        out.push_back(math::Aabb{corner, corner + cf3(0.4f, 0.4f, 0.4f)});
    }
    return out;
}

}  // namespace

TEST_CASE("squashed cull: a planned compile widens exactly as the plain one does") {
    const scene::Document doc = squashed_gnarly();
    const float band = 0.15f;
    const scene::Tape full = scene::compile_document(doc);
    const scene::CullIndex index(doc);

    clay_test::Lcg rng(6490);
    std::size_t in_band = 0;
    for (const math::Aabb& brick : random_bricks(649)) {
        const scene::CullRegion cull{brick.dilated(band), band};
        const scene::CullPlan plan = index.plan(cull.region, band);
        // Planned for no band: a region that carries one cannot be served by
        // it, and the compile walks instead.
        const scene::CullPlan narrow = index.plan(cull.region);
        CHECK(plan.serves_band(band));
        CHECK_FALSE(narrow.serves_band(band));
        const scene::Tape plain = scene::compile_document(doc, &cull);
        require_same_tape(plain, scene::compile_document(doc, &cull, &index, &plan));
        require_same_tape(plain, scene::compile_document(doc, &cull, &index, &narrow));
        for (int i = 0; i < 64; ++i) {
            const cfloat3 p = cf3(rng.range(brick.min.x, brick.max.x),
                                  rng.range(brick.min.y, brick.max.y),
                                  rng.range(brick.min.z, brick.max.z));
            const float df = kernel::cclamp(full.eval(p).d, -band, band);
            if (kernel::cabs(df) < band) ++in_band;
            CHECK(df == kernel::cclamp(plain.eval(p).d, -band, band));
        }
    }
    REQUIRE(in_band > 0);
}

TEST_CASE("squashed cull: a document with no per-axis scale culls as it always did") {
    // The squash is zero for every similarity, so the band a region carries
    // cannot change a single decision: the tape is the one a region without
    // a band compiles, byte for byte, planned or not.
    const scene::Document doc = clay_test::gnarly_document();
    const float band = 0.15f;
    const scene::CullIndex index(doc);
    for (const math::Aabb& brick : random_bricks(652)) {
        const scene::CullRegion bare{brick.dilated(band)};
        const scene::CullRegion banded{brick.dilated(band), band};
        const scene::CullPlan plan = index.plan(banded.region);
        CHECK(plan.serves_band(1e9f));  // nothing to widen, so any band is served
        const scene::Tape before = scene::compile_document(doc, &bare);
        require_same_tape(before, scene::compile_document(doc, &banded));
        require_same_tape(before, scene::compile_document(doc, &banded, &index, &plan));
    }
}

TEST_CASE("squashed cull: the squash is zero for a similarity and q - 1 otherwise") {
    scene::Layer layer;
    scene::Node n;
    n.prim = scene::Prim::sphere(0.5f);
    CHECK(scene::item_cull_squash(n, layer).none());
    n.scale_axes = cf3(2.0f, 2.0f, 2.0f);  // uniform: still a similarity
    CHECK(scene::item_cull_squash(n, layer).none());

    n.scale_axes = cf3(1.0f, 4.0f, 2.0f);
    n.blend = scene::Blend{scene::BlendProfile::Quadratic, 0.1f};
    const scene::CullSquash item = scene::item_cull_squash(n, layer);
    CHECK(item.slope == doctest::Approx(3.0f));
    CHECK(item.reach > 0.0f);  // the blend support is reached three times further too

    // The two levels multiply: q = 4 at the item and 2 at the layer.
    layer.scale_axes = cf3(1.0f, 1.0f, 2.0f);
    CHECK(scene::item_cull_squash(n, layer).slope == doctest::Approx(7.0f));
}
