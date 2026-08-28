// Dirty-prefix (frontier) tracking across the C ABI (#360).
//
// A continuing Move drag replaces the tail grab deformer every frame, so
// nothing BEFORE the dragged node ever changes -- and before #360 every frame
// still discarded every seed the drag's bound reached and replayed the whole
// chain per dirty brick. Now a seed can carry a PREFIX (the chain folded
// through the first B roots), a parameter edit inside root i marks it
// dirty_from = min(existing, i) instead of dropping it, and the refill folds
// only roots[B..end) onto the prefix.
//
// The fast path is INVISIBLE by contract -- bit-identical to the full walk --
// so these cases assert on two things only: clay_document_resume_stats deltas
// (resumed vs refilled), plus the clay_internal_resume_frontier probe for the
// three numbers the ABI otherwise hides, and memcmp parity against an oracle
// document that never resumed anything. Wrong-direction failures are silent
// (a stale composite served as the answer), which is why every keep has a
// parity check and every refusal shape is pinned to fall back to the full
// walk rather than to a wrong field.
//
// Fixture geometry, shared by most cases (brick width 0.4, band 0.15):
// ordinal 0 base r .5 at x 0, ordinal 1 a SUBTRACT dab at x .5 (mixed ops, so
// a mis-ordered fold cannot hide behind min), ordinal 2 a dab at x .9,
// ordinal 3 the drag target at x 1.35 -- smooth-unioned, so the dragged
// suffix folds with a non-idempotent blend and stale double-folds cannot hide
// either. The drag grabs the target's +x pole (centre x 1.6, radius .2),
// which reaches no other item, so the frontier is ordinal 3; the gesture
// invalidates its own ball, centre dilated by radius + displacement (#358),
// which against the blend's cull pad (quadratic support 4k = 0.2) reaches the
// cull boxes of bricks kx 2..3 while kx -1..1 stay clean.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay_internal.h"

namespace {

constexpr std::uint32_t kClean = 0xFFFFFFFFu;  // dirty_from's "nothing pending"
constexpr int kDim = 8;
constexpr float kVox = 0.05f;
constexpr std::size_t kPer = 8u * 8u * 8u;

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

clay_node_id add_ball(clay_document* d, clay_layer_id layer, float r, float x,
                      std::int32_t op = CLAY_OP_ADD) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    if (op != CLAY_OP_ADD) REQUIRE(clay_item_set_op(it, op) == CLAY_OK);
    clay_node_id id = 0;
    REQUIRE(clay_layer_add_item(d, layer, it, &id) == CLAY_OK);
    clay_item_destroy(it);
    return id;
}

// A smooth-unioned dab. The DRAG TARGET below carries one deliberately: a
// hard-Add suffix folds with min, and min is idempotent, so a stale value
// accidentally folded in a second time can be invisible -- a quadratic blend
// is not, and makes such a fold show up in the parity memcmp.
clay_node_id add_blended_ball(clay_document* d, clay_layer_id layer, float r, float x, float k) {
    clay_item_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = static_cast<std::uint32_t>(sizeof desc);
    desc.prim = CLAY_PRIM_SPHERE;
    desc.params[0] = r;
    desc.op = CLAY_OP_ADD;
    desc.blend = CLAY_BLEND_QUADRATIC;
    desc.blend_k = k;
    desc.position[0] = x;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    clay_node_id id = 0;
    REQUIRE(clay_add_item(d, layer, &desc, &id) == CLAY_OK);
    return id;
}

// One brick, addressed the way the cache addresses it.
clay_brick_request brick(int kx, int ky, int kz) {
    clay_brick_request q;
    std::memset(&q, 0, sizeof q);
    const int k[3] = {kx, ky, kz};
    for (int a = 0; a < 3; ++a) {
        q.key[a] = k[a];
        q.origin[a] = static_cast<float>(k[a]) * kDim * kVox;
        q.dims[a] = kDim;
    }
    q.spacing = kVox;
    q.band = 3.0f * kVox;
    return q;
}

// A row of bricks along the equator, straddling y = z = 0 from below.
std::vector<clay_brick_request> row(int from, int count) {
    std::vector<clay_brick_request> reqs;
    reqs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) reqs.push_back(brick(from + i, -1, -1));
    return reqs;
}

std::vector<float> refill(clay_document* d, const std::vector<clay_brick_request>& reqs) {
    std::vector<float> out(reqs.size() * kPer, 0.0f);
    REQUIRE(clay_brick_cache_eval_requests(d, nullptr, reqs.data(), reqs.size(), out.data(),
                                           out.size(), nullptr, 0) == CLAY_OK);
    return out;
}

clay_resume_stats resume_stats(const clay_document* d) {
    clay_resume_stats s{};
    s.struct_size = sizeof s;
    REQUIRE(clay_document_resume_stats(d, &s) == CLAY_OK);
    return s;
}

// The counters are cumulative; a caller reads them as a difference across the
// one call it cares about. `resumed` counts both the rev-is-current shortcut
// and the seeded suffix walks (append or frontier); `refilled` is the full
// walk. Which of the two resume shapes fired is separated by the probe below,
// because nothing about a brick's VALUES can say which path produced it.
struct RefillSplit {
    std::uint64_t resumed = 0;
    std::uint64_t refilled = 0;
};

RefillSplit refill_counting(clay_document* d, const std::vector<clay_brick_request>& reqs,
                            std::vector<float>* out_values = nullptr) {
    const clay_resume_stats before = resume_stats(d);
    std::vector<float> v = refill(d, reqs);
    const clay_resume_stats after = resume_stats(d);
    if (out_values) *out_values = std::move(v);
    return RefillSplit{after.resumed_bricks - before.resumed_bricks,
                       after.refilled_bricks - before.refilled_bricks};
}

// The three numbers the frontier keeps per brick, or found == false when the
// store holds no entry for it.
struct FrontierProbe {
    bool found = false;
    std::uint32_t dirty = 0;
    std::uint32_t boundary = 0;
    std::uint64_t structure = 0;
};

FrontierProbe probe(const clay_document* d, const clay_brick_request& q) {
    FrontierProbe p;
    const clay_result r =
        clay_internal_resume_frontier(d, &q, &p.dirty, &p.boundary, &p.structure);
    if (r == CLAY_ERROR_NOT_FOUND) return p;
    REQUIRE(r == CLAY_OK);
    p.found = true;
    return p;
}

std::uint64_t resume_order_size(const clay_document* d) {
    std::uint64_t n = 0;
    REQUIRE(clay_internal_resume_order_size(d, &n) == CLAY_OK);
    return n;
}

void drag(clay_document* d, clay_layer_id layer, const float centre[3], float radius, float dx) {
    clay_move_params p;
    std::memset(&p, 0, sizeof p);
    p.struct_size = static_cast<std::uint32_t>(sizeof p);
    p.radius = radius;
    const float displacement[3] = {dx, 0.0f, 0.0f};
    std::size_t applied = 0;
    REQUIRE(clay_layer_move_surface(d, layer, centre, displacement, &p, &applied) == CLAY_OK);
    REQUIRE(applied >= 1);  // the drag must be real before anything about it is
}

void nudge(clay_document* d, clay_layer_id layer, clay_node_id node, float x) {
    const float pos[3] = {x, 0.0f, 0.0f};
    const float axis[3] = {0.0f, 0.0f, 1.0f};
    REQUIRE(clay_layer_set_transform(d, layer, node, pos, axis, 0.0f, 1.0f) == CLAY_OK);
}

// The single-layer drag fixture described in the header comment. frame(f)
// holds centre and radius fixed and grows only the displacement, which is what
// makes moved_chain REPLACE the leading grab rather than stack another -- the
// continuing drag #360 exists for. Displacement stays under 0.09 so the drag's
// growing ball (centre dilated by radius + displacement, the region the
// gesture states, #358) never crosses into brick kx 1's cull box and the dirty
// set stays put across the whole test.
struct DragFixture {
    Doc doc;
    clay_node_id base, h1, h2, target;
    DragFixture() {
        base = add_ball(doc.d, doc.layer, 0.5f, 0.0f);
        h1 = add_ball(doc.d, doc.layer, 0.3f, 0.5f, CLAY_OP_SUBTRACT);
        h2 = add_ball(doc.d, doc.layer, 0.3f, 0.9f);
        // Smooth-unioned, so the dragged suffix does not fold with an
        // idempotent min -- see add_blended_ball. The blend also gives the
        // document a cull pad (quadratic support 4k = 0.2), which widens
        // every cull box -- even so the drag's ball (#358) reaches only
        // bricks kx 2..3.
        target = add_blended_ball(doc.d, doc.layer, 0.25f, 1.35f, 0.05f);
    }
    void frame(int f) {
        const float centre[3] = {1.6f, 0.0f, 0.0f};
        drag(doc.d, doc.layer, centre, 0.2f, 0.05f + 0.005f * static_cast<float>(f - 1));
    }
};

// DragFixture with the target moved OUT of the chain's reach: same first three
// items, target at x 4.0, the grab at its +x pole. The chain's influence ends
// near x 1.4 pad included, while the dirtied ground sits at kx 8..11 -- so the
// prefix the prepare pass records for a dirtied brick is roots[0..3) culled to
// a region none of them reach: the empty tape, CLAY_TAPE_FAR everywhere, no
// accumulator (prefix_had_acc false). The one fixture that reaches that
// refusal in frontier_seed_for.
struct IsolatedDragFixture {
    Doc doc;
    clay_node_id base, h1, h2, target;
    IsolatedDragFixture() {
        base = add_ball(doc.d, doc.layer, 0.5f, 0.0f);
        h1 = add_ball(doc.d, doc.layer, 0.3f, 0.5f, CLAY_OP_SUBTRACT);
        h2 = add_ball(doc.d, doc.layer, 0.3f, 0.9f);
        target = add_blended_ball(doc.d, doc.layer, 0.25f, 4.0f, 0.05f);
    }
    void frame(int f) {
        const float centre[3] = {4.25f, 0.0f, 0.0f};
        drag(doc.d, doc.layer, centre, 0.2f, 0.05f + 0.005f * static_cast<float>(f - 1));
    }
};

// The same shape with a longer tail, for the cases that dirty at several
// ordinals: the drag at centre x 1.5, radius .2 reaches s3, s4 and s5 (its box
// [1.3, 1.7] misses s2's bound [.55, 1.15]), so the recorded boundary is 3 and
// the ordinals above it are free for min-merge edits.
struct SweepFixture {
    Doc doc;
    clay_node_id base, s1, s2, s3, s4, s5;
    SweepFixture() {
        base = add_ball(doc.d, doc.layer, 0.5f, 0.0f);
        s1 = add_ball(doc.d, doc.layer, 0.3f, 0.5f, CLAY_OP_SUBTRACT);
        s2 = add_ball(doc.d, doc.layer, 0.3f, 0.85f);
        s3 = add_ball(doc.d, doc.layer, 0.3f, 1.1f);
        s4 = add_ball(doc.d, doc.layer, 0.25f, 1.35f);
        s5 = add_ball(doc.d, doc.layer, 0.25f, 1.6f);
    }
    void drag_once() {
        const float centre[3] = {1.5f, 0.0f, 0.0f};
        drag(doc.d, doc.layer, centre, 0.2f, 0.06f);
    }
};

// Two layers; the drag and the edits under test land on the ACTIVE one while
// the layer beneath supplies the below half of every seed -- plus one far node
// on the below layer whose edits exercise the legacy region drop.
struct TwoLayerFixture {
    clay_document* d = nullptr;
    clay_layer_id below = 0, active = 0;
    clay_node_id b0, b1, base, h1, h2, target;
    TwoLayerFixture() {
        d = clay_document_create();
        REQUIRE(d != nullptr);
        REQUIRE(clay_add_sdf_layer(d, "below", &below) == CLAY_OK);
        REQUIRE(clay_add_sdf_layer(d, "active", &active) == CLAY_OK);
        b0 = add_ball(d, below, 0.4f, 0.2f);
        b1 = add_ball(d, below, 0.3f, 5.0f);
        base = add_ball(d, active, 0.5f, 0.0f);
        h1 = add_ball(d, active, 0.3f, 0.5f, CLAY_OP_SUBTRACT);
        h2 = add_ball(d, active, 0.3f, 0.9f);
        target = add_ball(d, active, 0.25f, 1.35f);
    }
    ~TwoLayerFixture() { clay_document_destroy(d); }
    TwoLayerFixture(const TwoLayerFixture&) = delete;
    TwoLayerFixture& operator=(const TwoLayerFixture&) = delete;
    void frame(int f) {
        const float centre[3] = {1.6f, 0.0f, 0.0f};
        drag(d, active, centre, 0.2f, 0.05f + 0.005f * static_cast<float>(f - 1));
    }
};

}  // namespace

TEST_CASE("frontier: a continuing drag resumes every frame") {
    // The #360 headline. Every frame replaces the tail grab; the bricks it
    // dirties must refill from their prefix seed (nothing takes the full
    // walk), the accepted submit must clear the frontier while PRESERVING the
    // prefix (or frame three would be the full walk again), and the values
    // must be the full walk's to the bit. The window slides so warm and
    // skipped ground mix -- a brick left dirty while unrequested has to catch
    // up in one go when the window swings back over it.
    const std::vector<clay_brick_request> wide = row(-1, 5);   // kx -1..3
    const std::vector<clay_brick_request> tight = row(0, 4);   // kx 0..3
    const clay_brick_request hot = brick(3, -1, -1);           // always in the bound

    DragFixture fix;
    refill(fix.doc.d, wide);  // warm: every brick holds a seed
    REQUIRE(probe(fix.doc.d, hot).found);
    REQUIRE(probe(fix.doc.d, hot).dirty == kClean);

    std::vector<float> prev_wide;
    for (int f = 1; f <= 8; ++f) {
        CAPTURE(f);
        fix.frame(f);

        // The pre-drag pass recorded a prefix at the boundary BEFORE the
        // dragged node, and the applies min-merged the frontier onto it. A
        // dirty brick cannot take the rev-is-current shortcut, so a refill
        // with zero full walks below proves the frontier path itself fired.
        const FrontierProbe before = probe(fix.doc.d, hot);
        REQUIRE(before.found);
        CHECK(before.dirty == 3);
        CHECK(before.boundary == 3);  // equality valid: item 3 is the first suffix item
        CHECK(before.structure != 0);

        const std::vector<clay_brick_request>& reqs = (f % 2 == 0) ? tight : wide;
        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, reqs, &got);
        CHECK(split.refilled == 0);
        CHECK(split.resumed == reqs.size());

        // The accepted current-generation submit clears the frontier...
        const FrontierProbe after = probe(fix.doc.d, hot);
        REQUIRE(after.found);
        CHECK(after.dirty == kClean);
        // ...and keeps the prefix for the next frame.
        CHECK(after.boundary == 3);
        CHECK(after.structure == before.structure);

        // Parity: an oracle built fresh, dragged the same way, that never
        // held a seed -- its refill is the full walk this one must match.
        {
            DragFixture oracle;
            for (int g = 1; g <= f; ++g) oracle.frame(g);
            const std::vector<float> want = refill(oracle.doc.d, reqs);
            REQUIRE(got.size() == want.size());
            CHECK(got == want);  // bit-identical, not within a tolerance
        }

        // Teeth: the drag must MOVE the field frame over frame, or the parity
        // above is two readings of a static document agreeing.
        if (f % 2 == 1) {
            if (!prev_wide.empty()) CHECK(got != prev_wide);
            prev_wide = std::move(got);
        }
    }
}

TEST_CASE("frontier: dirty_from is min-merged and cleared only by a submit") {
    // The historical invariant: dirty_from sits at or before the earliest
    // changed boundary. Two edits at two ordinals must leave the EARLIER one
    // whichever order they land in -- an overwrite would resurrect a prefix
    // the first edit already invalidated -- and only a refill's ACCEPTED
    // submit may reset it. (The stale-submit half -- a submit whose plan
    // predates an edit -- is unreachable single-threaded through the ABI;
    // the interleave seam in clay_internal.h stands in for the racing
    // thread, and the stale-submit case below pins the plan->now == now
    // gate directly. The laundering case further down pins the sweep half.)
    const std::vector<clay_brick_request> window = row(-1, 5);
    const clay_brick_request hot = brick(3, -1, -1);
    const std::vector<clay_brick_request> hot_only{hot};

    SweepFixture fix;
    refill(fix.doc.d, window);
    fix.drag_once();
    refill(fix.doc.d, window);  // settle: prefixes recorded at boundary 3, all clean

    const FrontierProbe settled = probe(fix.doc.d, hot);
    REQUIRE(settled.found);
    REQUIRE(settled.dirty == kClean);
    REQUIRE(settled.boundary == 3);

    SUBCASE("later then earlier leaves the earlier") {
        nudge(fix.doc.d, fix.doc.layer, fix.s5, 1.62f);
        CHECK(probe(fix.doc.d, hot).dirty == 5);
        nudge(fix.doc.d, fix.doc.layer, fix.s4, 1.37f);
        CHECK(probe(fix.doc.d, hot).dirty == 4);
    }

    SUBCASE("earlier then later HOLDS the earlier") {
        // The overwrite bug reads dirty_from == 5 here.
        nudge(fix.doc.d, fix.doc.layer, fix.s4, 1.37f);
        CHECK(probe(fix.doc.d, hot).dirty == 4);
        nudge(fix.doc.d, fix.doc.layer, fix.s5, 1.62f);
        CHECK(probe(fix.doc.d, hot).dirty == 4);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, hot_only, &got);
        CHECK(split.resumed == 1);
        CHECK(split.refilled == 0);
        CHECK(probe(fix.doc.d, hot).dirty == kClean);

        SweepFixture oracle;
        oracle.drag_once();
        nudge(oracle.doc.d, oracle.doc.layer, oracle.s4, 1.37f);
        nudge(oracle.doc.d, oracle.doc.layer, oracle.s5, 1.62f);
        CHECK(got == refill(oracle.doc.d, hot_only));
    }

    SUBCASE("an edit AT the boundary is still resumable -- equality is valid") {
        // Boundary 3 means three items folded; an edit inside item 3 makes it
        // the first suffix item, which is exactly what the suffix compiles.
        // Reverting <= to < in the eligibility check flips this to a refill.
        nudge(fix.doc.d, fix.doc.layer, fix.s3, 1.12f);
        CHECK(probe(fix.doc.d, hot).dirty == 3);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, hot_only, &got);
        CHECK(split.resumed == 1);
        CHECK(split.refilled == 0);

        SweepFixture oracle;
        oracle.drag_once();
        nudge(oracle.doc.d, oracle.doc.layer, oracle.s3, 1.12f);
        CHECK(got == refill(oracle.doc.d, hot_only));
    }

    SUBCASE("an edit BEFORE the boundary drops the seed -- conservative, never wrong") {
        // The prefix folded s2, so an edit inside s2 falsifies it; the only
        // honest frontier would be earlier than the prefix can serve, and the
        // entry takes the legacy drop.
        nudge(fix.doc.d, fix.doc.layer, fix.s2, 0.87f);
        CHECK_FALSE(probe(fix.doc.d, hot).found);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, hot_only, &got);
        CHECK(split.refilled == 1);
        CHECK(split.resumed == 0);

        SweepFixture oracle;
        oracle.drag_once();
        nudge(oracle.doc.d, oracle.doc.layer, oracle.s2, 0.87f);
        CHECK(got == refill(oracle.doc.d, hot_only));
    }

    SUBCASE("a second refill with no edit answers by the shortcut, probe untouched") {
        const RefillSplit again = refill_counting(fix.doc.d, window);
        CHECK(again.resumed == window.size());
        CHECK(again.refilled == 0);
        const FrontierProbe p = probe(fix.doc.d, hot);
        CHECK(p.dirty == kClean);
        CHECK(p.boundary == 3);
    }
}

TEST_CASE("frontier: a stale submit cannot clear a frontier a newer edit set") {
    // The other half of the invariant above: the plan->now == now gate in
    // front of store_active. A resumed refill runs its walks off the lock and
    // retakes it to store; an edit landing in that window makes the coming
    // submit STALE -- its composite predates the edit -- and letting it clear
    // dirty_from would hand the next far sweep a clean-looking entry to
    // launder current, after which the shortcut serves the pre-edit field as
    // the whole answer. Single-threaded the ABI cannot land an edit there, so
    // the interleave seam (clay_internal.h) stands in for the racing thread:
    // it fires once, between the walks and the retaken lock. Deleting the
    // gate reads kFrontierClean at the probe below and fails the parity check
    // after the far sweep.
    const std::vector<clay_brick_request> window = row(-1, 5);
    const clay_brick_request hot = brick(3, -1, -1);

    TwoLayerFixture fix;
    const std::vector<float> warm = refill(fix.d, window);
    fix.frame(1);  // dirties kx 2..3 at frontier 3, prefixes recorded
    REQUIRE(probe(fix.d, hot).dirty == 3);

    // The racing edit: the same parameter nudge any next frame would make,
    // landed where no single-threaded caller can land one. One-shot by the
    // seam's contract, so the refills below run unmolested.
    REQUIRE(clay_internal_set_resume_store_interleave(
                fix.d,
                [](void* user) {
                    auto* f = static_cast<TwoLayerFixture*>(user);
                    nudge(f->d, f->active, f->target, 1.37f);
                },
                &fix) == CLAY_OK);

    const RefillSplit raced = refill_counting(fix.d, window);
    CHECK(raced.resumed == window.size());  // the walks ran; only the STORE is stale
    CHECK(raced.refilled == 0);

    // The gate's whole job: the frontier the interleaved edit set survives
    // the stale submit.
    const FrontierProbe after = probe(fix.d, hot);
    REQUIRE(after.found);
    CHECK(after.dirty == 3);

    // The far sweep that would launder a wrongly-cleaned entry current --
    // dirty entries are exempt from it, but only while they are still dirty.
    nudge(fix.d, fix.below, fix.b1, 5.2f);
    CHECK(probe(fix.d, hot).dirty == 3);

    // The next refill folds the suffix in and is the accepted submit.
    std::vector<float> got;
    const RefillSplit second = refill_counting(fix.d, window, &got);
    CHECK(second.resumed == window.size());
    CHECK(second.refilled == 0);
    CHECK(probe(fix.d, hot).dirty == kClean);

    TwoLayerFixture oracle;
    oracle.frame(1);
    nudge(oracle.d, oracle.active, oracle.target, 1.37f);
    nudge(oracle.d, oracle.below, oracle.b1, 5.2f);
    const std::vector<float> want = refill(oracle.d, window);
    REQUIRE(got.size() == want.size());
    CHECK(got == want);
    CHECK(got != warm);  // the edits moved the field, so a stale serve WOULD show
}

TEST_CASE("frontier: a far edit does not launder a dirty seed current") {
    // touch_region advances every surviving out-of-bound entry to the new
    // revision, and the rev-is-current shortcut hands such an entry straight
    // to the caller. A frontier-dirty entry swept past by a LATER edit
    // elsewhere must NOT be stamped current: its stored composite predates the
    // drag, and laundering it current would serve that stale field as the
    // whole answer with nothing ever folding the suffix in. Reverting the
    // dirty-entry guard in touch_region_locked flips the parity check here.
    const std::vector<clay_brick_request> window = row(-1, 5);
    const clay_brick_request hot = brick(3, -1, -1);

    TwoLayerFixture fix;
    const std::vector<float> warm = refill(fix.d, window);
    fix.frame(1);  // dirties kx 2..3 at frontier 3; NO refill yet
    const FrontierProbe before = probe(fix.d, hot);
    REQUIRE(before.found);
    REQUIRE(before.dirty == 3);

    // A parameter edit on the layer BENEATH, far away: not a frontier command
    // (wrong layer), so it takes the legacy region drop -- which reaches
    // nothing here and sweeps the window as "untouched".
    nudge(fix.d, fix.below, fix.b1, 5.2f);
    const FrontierProbe after = probe(fix.d, hot);
    REQUIRE(after.found);
    CHECK(after.dirty == 3);  // still dirty: not laundered, not dropped

    std::vector<float> got;
    const RefillSplit split = refill_counting(fix.d, window, &got);
    CHECK(split.refilled == 0);
    CHECK(split.resumed == window.size());

    TwoLayerFixture oracle;
    oracle.frame(1);
    nudge(oracle.d, oracle.below, oracle.b1, 5.2f);
    const std::vector<float> want = refill(oracle.d, window);
    REQUIRE(got.size() == want.size());
    CHECK(got == want);
    CHECK(got != warm);  // the drag moved the field, so laundering WOULD have shown
}

TEST_CASE("frontier: a structural edit retires every prefix -- full walk, never a remap") {
    // Ordinals are positions and positions do not survive the root list
    // changing shape. Each case leaves a brick dirty with a prefix taken under
    // the old structure, changes the structure, and requires the refill to
    // take the full walk with full parity -- a raw-ordinal comparison across
    // the revisions would fold the wrong suffix onto the wrong seed silently.
    const std::vector<clay_brick_request> window = row(-1, 5);
    const clay_brick_request hot = brick(3, -1, -1);
    const std::vector<clay_brick_request> hot_only{hot};

    SUBCASE("a mid-drag append") {
        DragFixture fix;
        refill(fix.doc.d, window);
        fix.frame(1);  // dirty at 3, no refill
        REQUIRE(probe(fix.doc.d, hot).dirty == 3);

        // A stamp landing mid-drag: a tail append, far from the window. It
        // does not break the ground spatially, but it grows the root list, so
        // the prefix boundaries count a different tail now.
        add_ball(fix.doc.d, fix.doc.layer, 0.2f, -3.0f);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, window, &got);
        CHECK(split.refilled == 2);  // the drag-dirtied kx 2..3: stale structure
        CHECK(split.resumed == 3);   // the clean ground rides the APPEND path

        DragFixture oracle;
        oracle.frame(1);
        add_ball(oracle.doc.d, oracle.doc.layer, 0.2f, -3.0f);
        CHECK(got == refill(oracle.doc.d, window));

        // The full path re-stored the brick and cleared the stale prefix.
        const FrontierProbe p = probe(fix.doc.d, hot);
        REQUIRE(p.found);
        CHECK(p.dirty == kClean);
        CHECK(p.structure == 0);

        // And the NEXT plain stamp still takes the append fast path -- the
        // append machinery is untouched by #360, asserted rather than assumed.
        add_ball(fix.doc.d, fix.doc.layer, 0.2f, 0.3f);
        const RefillSplit stamp = refill_counting(fix.doc.d, window);
        CHECK(stamp.resumed == window.size());
        CHECK(stamp.refilled == 0);
    }

    SUBCASE("a mid-drag node removal shifts the ordinals under the boundary") {
        // The corruption case: removing s1 renumbers everything after it, so
        // boundary 3 -- taken over [base, s1, s2] -- would now claim
        // [base, s2, s3] and the suffix would skip the dragged s3 entirely.
        // Structure mismatch must refuse; the parity check is what fails if a
        // raw ordinal is ever compared across the two shapes.
        SweepFixture fix;
        refill(fix.doc.d, window);
        fix.drag_once();  // dirty at 3, no refill
        REQUIRE(probe(fix.doc.d, hot).dirty == 3);

        REQUIRE(clay_remove_node(fix.doc.d, fix.doc.layer, fix.s1) == CLAY_OK);
        const FrontierProbe p = probe(fix.doc.d, hot);
        REQUIRE(p.found);  // out of the removal's bound: kept, dirty, stale

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, hot_only, &got);
        CHECK(split.refilled == 1);
        CHECK(split.resumed == 0);

        SweepFixture oracle;
        oracle.drag_once();
        REQUIRE(clay_remove_node(oracle.doc.d, oracle.doc.layer, oracle.s1) == CLAY_OK);
        CHECK(got == refill(oracle.doc.d, hot_only));

        // The rest of the window, for the wider parity.
        CHECK(refill(fix.doc.d, window) == refill(oracle.doc.d, window));
    }

    SUBCASE("a mid-drag visibility toggle on a lower layer") {
        TwoLayerFixture fix;
        refill(fix.d, window);
        fix.frame(1);
        REQUIRE(probe(fix.d, hot).dirty == 3);

        REQUIRE(clay_document_set_layer_visible(fix.d, fix.below, 0) == CLAY_OK);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.d, hot_only, &got);
        CHECK(split.refilled == 1);
        CHECK(split.resumed == 0);

        TwoLayerFixture oracle;
        oracle.frame(1);
        REQUIRE(clay_document_set_layer_visible(oracle.d, oracle.below, 0) == CLAY_OK);
        CHECK(got == refill(oracle.d, hot_only));
        CHECK(refill(fix.d, window) == refill(oracle.d, window));
    }
}

TEST_CASE("frontier: every fallback is the full walk, never a wrong field") {
    const std::vector<clay_brick_request> window = row(-1, 5);

    SUBCASE("an evicted seed mid-drag is slow, not wrong") {
        DragFixture fix;
        refill(fix.doc.d, window);
        fix.frame(1);
        REQUIRE(refill_counting(fix.doc.d, window).refilled == 0);

        // Shrink the store under the drag: everything but the most recently
        // used entry goes, prefixes with it.
        REQUIRE(clay_internal_set_resume_budget(fix.doc.d, 0) == CLAY_OK);
        CHECK(resume_stats(fix.doc.d).entries == 1);

        fix.frame(2);
        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, window, &got);
        CHECK(split.refilled >= window.size() - 1);

        DragFixture oracle;
        oracle.frame(1);
        oracle.frame(2);
        CHECK(got == refill(oracle.doc.d, window));
    }

    SUBCASE("a drag on a NON-active layer takes the legacy drop") {
        // The frontier path carries a seed's below half forward untouched on
        // the claim that only the active layer moves. A drag on the layer
        // beneath moves that half, so its edits must fall to the legacy
        // spatial drop -- the same family of silent corruption the append
        // path's layer gate (#354) refuses.
        TwoLayerFixture fix;
        refill(fix.d, window);
        const clay_brick_request near_brick = brick(0, -1, -1);
        REQUIRE(probe(fix.d, near_brick).found);

        const float centre[3] = {0.6f, 0.0f, 0.0f};
        drag(fix.d, fix.below, centre, 0.15f, 0.05f);

        // Dropped, not kept-dirty: no prefix could ever serve this edit.
        CHECK_FALSE(probe(fix.d, near_brick).found);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.d, window, &got);
        CHECK(split.refilled >= 1);

        TwoLayerFixture oracle;
        drag(oracle.d, oracle.below, centre, 0.15f, 0.05f);
        CHECK(got == refill(oracle.d, window));
    }

    SUBCASE("a layer-wide parameter edit takes the legacy drop, prefixes and all") {
        // SetLayerMirrorCmd is a parameter edit but not a (layer, node) one:
        // it reaches the whole layer, and the legacy region drop already says
        // everything true about it. A prefix-holding entry inside its bound is
        // dropped -- the legacy path must never become optimistic.
        SweepFixture fix;
        refill(fix.doc.d, window);
        fix.drag_once();
        refill(fix.doc.d, window);
        const clay_brick_request hot = brick(3, -1, -1);
        REQUIRE(probe(fix.doc.d, hot).found);
        REQUIRE(probe(fix.doc.d, hot).boundary == 3);

        REQUIRE(clay_set_layer_mirror(fix.doc.d, fix.doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);
        CHECK_FALSE(probe(fix.doc.d, hot).found);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, window, &got);
        CHECK(split.refilled == window.size());
        CHECK(split.resumed == 0);

        SweepFixture oracle;
        oracle.drag_once();
        REQUIRE(clay_set_layer_mirror(oracle.doc.d, oracle.doc.layer, 1, 0, 0, 0.0f) == CLAY_OK);
        CHECK(got == refill(oracle.doc.d, window));
    }

    SUBCASE("a prefix the cull emptied is refused -- no accumulator to seed") {
        // The drag target sits spatially ISOLATED from the rest of the chain
        // (IsolatedDragFixture), so the recorded prefix at every dirtied
        // brick is the all-far empty tape: prefix_had_acc false. The plan
        // still states layer_have_acc = true, and a suffix folded onto a
        // seed that never existed would combine against far-outside instead
        // of seeding the chain -- the prefix_had_acc gate in
        // frontier_seed_for is what turns that into the full walk. The probe
        // pins boundary and structure so every OTHER gate provably passes,
        // making the refilled == 1 below that one gate's own doing.
        const std::vector<clay_brick_request> iso = row(8, 4);  // kx 8..11: x 3.2..4.8
        const clay_brick_request lone = brick(10, -1, -1);      // holds the grabbed pole

        IsolatedDragFixture fix;
        std::vector<float> warm;
        refill_counting(fix.doc.d, iso, &warm);
        REQUIRE(probe(fix.doc.d, lone).found);

        fix.frame(1);

        // The keep and the prepare pass both fired: dirty at the target's
        // ordinal, prefix recorded at boundary 3 under the live structure.
        const FrontierProbe before = probe(fix.doc.d, lone);
        REQUIRE(before.found);
        REQUIRE(before.dirty == 3);
        REQUIRE(before.boundary == 3);
        REQUIRE(before.structure != 0);

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, {lone}, &got);
        CHECK(split.refilled == 1);
        CHECK(split.resumed == 0);

        // The full path re-stored the brick: clean, no prefix kept beside it.
        const FrontierProbe after = probe(fix.doc.d, lone);
        REQUIRE(after.found);
        CHECK(after.dirty == kClean);
        CHECK(after.structure == 0);

        IsolatedDragFixture oracle;
        oracle.frame(1);
        const std::vector<float> want = refill(oracle.doc.d, {lone});
        REQUIRE(got.size() == want.size());
        CHECK(got == want);
        // Teeth: the drag really moved this brick's field.
        const std::vector<float> lone_warm(warm.begin() + 2 * kPer, warm.begin() + 3 * kPer);
        CHECK(got != lone_warm);

        // The rest of the dirtied ground falls back the same way.
        CHECK(refill(fix.doc.d, iso) == refill(oracle.doc.d, iso));
    }

    SUBCASE("a late frontier does not imply a seed exists") {
        // Dirtying ground the store never held: the refill is all full walk.
        // Assuming the seed from the frontier alone would resume from nothing.
        SweepFixture fix;
        nudge(fix.doc.d, fix.doc.layer, fix.s4, 1.37f);  // frontier-shaped edit, no entries
        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, window, &got);
        CHECK(split.refilled == window.size());
        CHECK(split.resumed == 0);

        SweepFixture oracle;
        nudge(oracle.doc.d, oracle.doc.layer, oracle.s4, 1.37f);
        CHECK(got == refill(oracle.doc.d, window));
    }
}

TEST_CASE("frontier: spatial dirtying is the influence union, not a raw box") {
    const std::vector<clay_brick_request> wide = row(-1, 7);  // kx -1..5

    SUBCASE("a move dirties where the item WAS, not only where it lands") {
        // Dirtying only the new transform bound leaves the vacated ground
        // serving the item that is no longer there, by the shortcut, forever.
        SweepFixture fix;
        std::vector<float> warm;
        refill_counting(fix.doc.d, wide, &warm);
        const clay_brick_request hot = brick(3, -1, -1);  // holds s4/s5's surface
        REQUIRE(probe(fix.doc.d, hot).found);

        nudge(fix.doc.d, fix.doc.layer, fix.s5, -9.0f);  // clean out of the window
        CHECK_FALSE(probe(fix.doc.d, hot).found);  // old bound reached it: dropped

        std::vector<float> got;
        const RefillSplit split = refill_counting(fix.doc.d, {hot}, &got);
        CHECK(split.refilled == 1);

        SweepFixture oracle;
        nudge(oracle.doc.d, oracle.doc.layer, oracle.s5, -9.0f);
        CHECK(got == refill(oracle.doc.d, {hot}));
        // Teeth: the vacated brick's field really changed.
        const std::vector<float> hot_warm(warm.begin() + 4 * kPer, warm.begin() + 5 * kPer);
        CHECK(got != hot_warm);
    }

    SUBCASE("a deformer's reach dirties past the primitive's own box") {
        // A grab whose support sits BEYOND the item's raw AABB changes values
        // in bricks the raw box never touches. Influence taken from the
        // primitive AABB would leave those bricks clean on the shortcut with
        // the un-warped field. The target's blended influence is [0.9, 1.8]
        // and brick kx 7's cull box [2.45, 3.55] misses it entirely; the grab
        // (world centre 2.5, radius .5, displacement -.8) dilates the
        // influence to [0.1, 2.6], which reaches it.
        const std::vector<clay_brick_request> span = row(-3, 11);  // kx -3..7
        DragFixture fix;
        std::vector<float> warm;
        refill_counting(fix.doc.d, span, &warm);
        const clay_brick_request beyond = brick(7, -1, -1);
        // The untouched control must HOLD FIELD CONTENT: a brick whose culled
        // tape kept nothing stores all-FAR values, and seed_for declines an
        // accumulator-less seed (had_acc), taking the cheap full walk instead
        // of the shortcut this subcase pins. kx -2 straddles the base ball's
        // surface at x = -0.5; kx -3 only held values by grace of the cull
        // pad's width, which #335 narrowed.
        const clay_brick_request untouched = brick(-2, -1, -1);
        REQUIRE(probe(fix.doc.d, beyond).found);

        // Deformer parameters are node-LOCAL; the target sits at x 1.35.
        const float grab[8] = {1.15f, 0.0f, 0.0f, 0.5f, -0.8f, 0.0f, 0.0f, 0.0f};
        REQUIRE(clay_layer_add_deformer(fix.doc.d, fix.doc.layer, fix.target,
                                        CLAY_DEFORM_GRAB, grab, 8, 0, 1) == CLAY_OK);

        CHECK_FALSE(probe(fix.doc.d, beyond).found);  // reached through the warp
        // ...while ground outside old AND new influence stays on the shortcut.
        CHECK(probe(fix.doc.d, untouched).found);
        CHECK(probe(fix.doc.d, untouched).dirty == kClean);
        CHECK(refill_counting(fix.doc.d, {untouched}).resumed == 1);

        std::vector<float> got;
        refill_counting(fix.doc.d, span, &got);
        DragFixture oracle;
        REQUIRE(clay_layer_add_deformer(oracle.doc.d, oracle.doc.layer, oracle.target,
                                        CLAY_DEFORM_GRAB, grab, 8, 0, 1) == CLAY_OK);
        const std::vector<float> want = refill(oracle.doc.d, span);
        REQUIRE(got.size() == want.size());
        CHECK(got == want);
        // Teeth: the beyond-the-box brick's values really moved.
        const std::vector<float> beyond_warm(warm.begin() + 10 * kPer, warm.begin() + 11 * kPer);
        const std::vector<float> beyond_got(got.begin() + 10 * kPer, got.begin() + 11 * kPer);
        CHECK(beyond_got != beyond_warm);
    }
}

TEST_CASE("frontier: authoring is untouched -- saves, undo and redo") {
    const std::vector<clay_brick_request> window = row(-1, 5);

    SUBCASE("a document that resumed saves the bytes of one that never did") {
        DragFixture resumed;
        refill(resumed.doc.d, window);
        resumed.frame(1);
        refill(resumed.doc.d, window);
        resumed.frame(2);
        refill(resumed.doc.d, window);

        DragFixture plain;
        plain.frame(1);
        plain.frame(2);

        clay_blob* a = nullptr;
        clay_blob* b = nullptr;
        REQUIRE(clay_document_save_memory(resumed.doc.d, &a) == CLAY_OK);
        REQUIRE(clay_document_save_memory(plain.doc.d, &b) == CLAY_OK);
        REQUIRE(clay_blob_size(a) == clay_blob_size(b));
        CHECK(std::memcmp(clay_blob_data(a), clay_blob_data(b), clay_blob_size(a)) == 0);
        clay_blob_destroy(a);
        clay_blob_destroy(b);
    }

    SUBCASE("undoing a resumed drag restores the field; redo restores the drag") {
        DragFixture fix;
        REQUIRE(clay_document_enable_undo(fix.doc.d) == CLAY_OK);
        const std::vector<float> before = refill(fix.doc.d, window);
        fix.frame(1);
        const std::vector<float> after = refill(fix.doc.d, window);
        CHECK(after != before);

        std::int32_t undone = 0;
        REQUIRE(clay_document_undo(fix.doc.d, &undone) == CLAY_OK);
        REQUIRE(undone == 1);
        CHECK(refill(fix.doc.d, window) == before);

        std::int32_t redone = 0;
        REQUIRE(clay_document_redo(fix.doc.d, &redone) == CLAY_OK);
        REQUIRE(redone == 1);
        CHECK(refill(fix.doc.d, window) == after);
    }
}

TEST_CASE("frontier: the store's budget and order hold, and bytes do not grow with history") {
    // One order node per entry (#346's probe), bytes inside the budget, and --
    // the shape #360 explicitly rules out -- per-brick metadata that scales
    // with drag length. One prefix per entry means a fifty-frame drag costs
    // what a three-frame drag costs.
    const std::vector<clay_brick_request> window = row(-1, 5);
    DragFixture fix;
    refill(fix.doc.d, window);

    std::uint64_t settled_bytes = 0;
    for (int f = 1; f <= 50; ++f) {
        CAPTURE(f);
        // Displacement growth stops at frame 8 so the dirty set cannot creep
        // into fresh bricks; a fixed displacement still replaces the lead grab
        // and dirties every frame, which is what a hover mid-drag does.
        fix.frame(f < 8 ? f : 8);
        const RefillSplit split = refill_counting(fix.doc.d, window);
        CHECK(split.refilled == 0);

        const clay_resume_stats st = resume_stats(fix.doc.d);
        CHECK(resume_order_size(fix.doc.d) == st.entries);
        CHECK(st.entries == window.size());
        CHECK(st.bytes <= st.budget);
        if (f == 3) settled_bytes = st.bytes;
        if (f > 3) CHECK(st.bytes == settled_bytes);  // FLAT across the drag
    }
}

TEST_CASE("frontier: the probe's error paths") {
    Doc doc;
    add_ball(doc.d, doc.layer, 0.5f, 0.0f);
    const clay_brick_request q = brick(0, -1, -1);
    std::uint32_t dirty = 0, boundary = 0;
    std::uint64_t structure = 0;

    CHECK(clay_internal_resume_frontier(nullptr, &q, &dirty, &boundary, &structure) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_internal_resume_frontier(doc.d, nullptr, &dirty, &boundary, &structure) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_internal_resume_frontier(doc.d, &q, nullptr, &boundary, &structure) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_internal_resume_frontier(doc.d, &q, &dirty, nullptr, &structure) ==
          CLAY_ERROR_INVALID_ARGUMENT);
    CHECK(clay_internal_resume_frontier(doc.d, &q, &dirty, &boundary, nullptr) ==
          CLAY_ERROR_INVALID_ARGUMENT);

    // A brick the store never held is NOT_FOUND, not a zeroed answer.
    CHECK(clay_internal_resume_frontier(doc.d, &q, &dirty, &boundary, &structure) ==
          CLAY_ERROR_NOT_FOUND);

    // And after a refill it is found, clean, with no prefix recorded.
    refill(doc.d, {q});
    REQUIRE(clay_internal_resume_frontier(doc.d, &q, &dirty, &boundary, &structure) == CLAY_OK);
    CHECK(dirty == kClean);
    CHECK(boundary == 0);
    CHECK(structure == 0);
}
