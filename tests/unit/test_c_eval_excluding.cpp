// Evaluating the document WITHOUT one layer (c-abi spec: a host can evaluate
// the document without one layer; issue #378).
//
// What these defend is the reason the calls exist rather than the arithmetic —
// the engine suite owns that. A host previewing ONE layer through a sculpt
// transaction needs the rest of the document beside it, and the two ways it
// could get one today are both wrong: there was no layer filter, and hiding the
// layer is an EDIT that the transaction's commit then refuses. So the cases
// here are: the composition rule the header tells a host to rely on is exact; a
// stale layer id is refused rather than silently answered with the whole
// document; and none of it touches the document, including inside an open
// transaction.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "clay.h"
#include "clay_internal.h"

namespace {

struct Doc {
    clay_document* doc = clay_document_create();
    Doc() = default;
    ~Doc() { clay_document_destroy(doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_layer_id sphere_layer(clay_document* doc, const char* name, float x, float r) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, name, &layer) == CLAY_OK);
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float pos[3] = {x, 0.0f, 0.0f};
    REQUIRE(clay_item_set_position(it, pos) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, it, nullptr) == CLAY_OK);
    clay_item_destroy(it);
    return layer;
}

std::vector<float> lattice(int n, float half) {
    std::vector<float> p;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                const float s = 2.0f * half / static_cast<float>(n - 1);
                p.push_back(-half + s * static_cast<float>(i));
                p.push_back(-half + s * static_cast<float>(j));
                p.push_back(-half + s * static_cast<float>(k));
            }
    return p;
}

std::vector<float> eval_all(const clay_document* doc, const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3);
    REQUIRE(clay_eval_points(doc, nullptr, pts.data(), out.size(), out.data(), nullptr) == CLAY_OK);
    return out;
}

std::vector<float> eval_without(const clay_document* doc, clay_layer_id excluded,
                                const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3);
    REQUIRE(clay_eval_points_excluding(doc, excluded, nullptr, pts.data(), out.size(), out.data(),
                                       nullptr) == CLAY_OK);
    return out;
}

std::vector<float> eval_only(const clay_document* doc, clay_layer_id layer,
                             const std::vector<float>& pts) {
    std::vector<float> out(pts.size() / 3);
    REQUIRE(clay_layer_eval_points(doc, layer, nullptr, pts.data(), out.size(), out.data(),
                                   nullptr) == CLAY_OK);
    return out;
}

}  // namespace

TEST_CASE("c abi: the excluded evaluation composes with min, exactly") {
    Doc d;
    const clay_layer_id a = sphere_layer(d.doc, "a", -0.35f, 0.5f);
    const clay_layer_id b = sphere_layer(d.doc, "b", 0.0f, 0.45f);
    const clay_layer_id c = sphere_layer(d.doc, "c", 0.35f, 0.5f);
    const std::vector<float> pts = lattice(11, 1.0f);
    const std::vector<float> whole = eval_all(d.doc, pts);

    for (clay_layer_id which : {a, b, c}) {
        CAPTURE(which);
        const std::vector<float> rest = eval_without(d.doc, which, pts);
        const std::vector<float> mine = eval_only(d.doc, which, pts);

        // The claim the header makes to a host: min(rest, your own preview) IS
        // the whole document, not an approximation of it. Bit equality, because
        // layers hard-union and a hard union IS the minimum.
        std::size_t differing = 0;
        for (std::size_t i = 0; i < whole.size(); ++i)
            if (std::min(rest[i], mine[i]) != whole[i]) ++differing;
        CHECK(differing == 0);

        // Teeth: excluding a layer must CHANGE the field, or the check above
        // would pass for a call that excluded nothing at all.
        CHECK(rest != whole);
    }

    // The MIDDLE layer is the case that separates this from the "layers below"
    // split the refill already had: that one stops at the named layer, so it
    // would drop `c` as well.
    const std::vector<float> without_b = eval_without(d.doc, b, pts);
    const std::vector<float> only_c = eval_only(d.doc, c, pts);
    std::size_t c_survived = 0;
    for (std::size_t i = 0; i < without_b.size(); ++i)
        if (without_b[i] == only_c[i] && only_c[i] < 0.0f) ++c_survived;
    CHECK(c_survived > 0);  // points inside c that only c decides
}

TEST_CASE("c abi: gradients exclude the same layer the distances do") {
    Doc d;
    const clay_layer_id a = sphere_layer(d.doc, "a", -0.4f, 0.5f);
    const clay_layer_id b = sphere_layer(d.doc, "b", 0.4f, 0.5f);
    const std::vector<float> pts = lattice(7, 0.9f);
    const std::size_t n = pts.size() / 3;

    std::vector<float> excl(n * 3), only_a(n * 3);
    REQUIRE(clay_eval_gradients_excluding(d.doc, b, nullptr, pts.data(), n, excl.data()) ==
            CLAY_OK);
    REQUIRE(clay_layer_eval_gradients(d.doc, a, nullptr, pts.data(), n, only_a.data()) == CLAY_OK);
    // With two layers, "everything except b" IS a.
    CHECK(excl == only_a);
}

TEST_CASE("c abi: a stale layer id is refused, not answered with the whole document") {
    Doc d;
    const clay_layer_id a = sphere_layer(d.doc, "a", 0.0f, 0.5f);
    const clay_layer_id b = sphere_layer(d.doc, "b", 0.6f, 0.4f);
    const std::vector<float> pts = lattice(5, 0.9f);
    const std::size_t n = pts.size() / 3;
    std::vector<float> out(n, -12345.0f), grads(n * 3, -12345.0f);

    // Removing b makes its id stale, which is exactly how a host gets one.
    REQUIRE(clay_document_remove_layer(d.doc, b) == CLAY_OK);
    CHECK(clay_eval_points_excluding(d.doc, b, nullptr, pts.data(), n, out.data(), nullptr) ==
          CLAY_ERROR_NOT_FOUND);
    CHECK(clay_eval_gradients_excluding(d.doc, b, nullptr, pts.data(), n, grads.data()) ==
          CLAY_ERROR_NOT_FOUND);
    // Nothing written: the refusal must not leave a half-filled buffer a caller
    // could draw.
    for (float v : out) CHECK(v == -12345.0f);
    for (float v : grads) CHECK(v == -12345.0f);

    // The layer that IS there still works, so the refusal is about the id and
    // not about the document.
    CHECK(clay_eval_points_excluding(d.doc, a, nullptr, pts.data(), n, out.data(), nullptr) ==
          CLAY_OK);
}

TEST_CASE("c abi: excluding a hidden or empty layer succeeds and changes nothing") {
    Doc d;
    const clay_layer_id a = sphere_layer(d.doc, "a", 0.0f, 0.5f);
    const clay_layer_id hidden = sphere_layer(d.doc, "hidden", 0.5f, 0.4f);
    REQUIRE(clay_document_set_layer_visible(d.doc, hidden, 0) == CLAY_OK);
    clay_layer_id bare = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "bare", &bare) == CLAY_OK);  // no items

    const std::vector<float> pts = lattice(7, 0.9f);
    const std::vector<float> whole = eval_all(d.doc, pts);
    // A layer contributing nothing to the union is one excluding it cannot
    // change — refusing here would make a host branch on state it has no
    // reason to track.
    CHECK(eval_without(d.doc, hidden, pts) == whole);
    CHECK(eval_without(d.doc, bare, pts) == whole);
    // And the visible one still has teeth.
    CHECK(eval_without(d.doc, a, pts) != whole);
}

TEST_CASE("c abi: a brick refill without one layer fills the documented slots") {
    Doc d;
    const clay_layer_id a = sphere_layer(d.doc, "a", -0.3f, 0.45f);
    const clay_layer_id b = sphere_layer(d.doc, "b", 0.3f, 0.45f);

    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = 8;
    cfg.voxel_size = 0.05f;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);
    const float lo[3] = {-0.9f, -0.6f, -0.6f}, hi[3] = {0.9f, 0.6f, 0.6f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);

    std::vector<clay_brick_request> reqs(8192);
    std::size_t count = reqs.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
    REQUIRE(remaining == 0);
    REQUIRE(count > 8);
    const std::size_t per = 8 * 8 * 8, total = count * per;

    // The reference: a SECOND document holding only the layer that survives.
    // Built independently rather than by excluding, so the two cannot agree by
    // sharing a bug.
    Doc ref;
    sphere_layer(ref.doc, "a", -0.3f, 0.45f);
    // The excluded refill takes no seed and runs no gate, so its floats are the
    // field's own at every sample; a whole-document refill proves the bricks
    // deep inside or far outside uniform and returns a stub for them. Same
    // classification, different floats -- and this comparison is of floats,
    // so the reference is read with the gate off.
    REQUIRE(clay_internal_set_uniform_gate(ref.doc, 0) == CLAY_OK);

    std::vector<float> got(total), want(total);
    REQUIRE(clay_brick_cache_eval_requests_excluding(d.doc, b, "cpu", reqs.data(), count,
                                                     got.data(), total, nullptr, 0) == CLAY_OK);
    REQUIRE(clay_brick_cache_eval_requests(ref.doc, "cpu", reqs.data(), count, want.data(), total,
                                           nullptr, 0) == CLAY_OK);
    CHECK(got == want);

    // Teeth: the whole-document refill of the SAME document differs, so the
    // exclusion is doing something.
    std::vector<float> whole(total);
    REQUIRE(clay_brick_cache_eval_requests(d.doc, "cpu", reqs.data(), count, whole.data(), total,
                                           nullptr, 0) == CLAY_OK);
    CHECK(got != whole);

    // A stale id is refused here too, and before any work.
    REQUIRE(clay_document_remove_layer(d.doc, b) == CLAY_OK);
    CHECK(clay_brick_cache_eval_requests_excluding(d.doc, b, "cpu", reqs.data(), count, got.data(),
                                                   total, nullptr, 0) == CLAY_ERROR_NOT_FOUND);
    CHECK(clay_brick_cache_eval_requests_excluding(d.doc, a, "cpu", reqs.data(), count, got.data(),
                                                   total, nullptr, 0) == CLAY_OK);
    clay_brick_cache_destroy(cache);
}

TEST_CASE("c abi: an excluded refill neither reads nor leaves a seed") {
    // The reason this is its own entry point rather than a flag. A seed is a
    // brick's value for THIS document; one computed without a layer is not, and
    // storing it would hand the next whole-document refill a seed with a layer
    // missing — silently, because a seeded answer is bit-identical to a walked
    // one by contract.
    Doc d;
    const clay_layer_id a = sphere_layer(d.doc, "a", -0.3f, 0.45f);
    const clay_layer_id b = sphere_layer(d.doc, "b", 0.3f, 0.45f);

    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = 8;
    cfg.voxel_size = 0.05f;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    REQUIRE(cache != nullptr);
    const float lo[3] = {-0.9f, -0.6f, -0.6f}, hi[3] = {0.9f, 0.6f, 0.6f};
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    std::vector<clay_brick_request> reqs(8192);
    std::size_t count = reqs.size(), remaining = 0;
    REQUIRE(clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) == CLAY_OK);
    const std::size_t per = 8 * 8 * 8, total = count * per;

    clay_resume_stats before;
    std::memset(&before, 0, sizeof before);
    before.struct_size = sizeof before;
    REQUIRE(clay_document_resume_stats(d.doc, &before) == CLAY_OK);

    std::vector<float> got(total);
    REQUIRE(clay_brick_cache_eval_requests_excluding(d.doc, b, "cpu", reqs.data(), count,
                                                     got.data(), total, nullptr, 0) == CLAY_OK);

    clay_resume_stats after;
    std::memset(&after, 0, sizeof after);
    after.struct_size = sizeof after;
    REQUIRE(clay_document_resume_stats(d.doc, &after) == CLAY_OK);
    // Neither counter moved and no seed was kept: nothing resumed, and nothing
    // seedable was walked.
    CHECK(after.resumed_bricks == before.resumed_bricks);
    CHECK(after.refilled_bricks == before.refilled_bricks);
    CHECK(after.entries == before.entries);

    // And the whole-document refill that follows is still correct — which is
    // what would break if the excluded values had been stored as seeds.
    Doc ref;
    sphere_layer(ref.doc, "a", -0.3f, 0.45f);
    sphere_layer(ref.doc, "b", 0.3f, 0.45f);
    std::vector<float> whole(total), want(total);
    REQUIRE(clay_brick_cache_eval_requests(d.doc, "cpu", reqs.data(), count, whole.data(), total,
                                           nullptr, 0) == CLAY_OK);
    REQUIRE(clay_brick_cache_eval_requests(ref.doc, "cpu", reqs.data(), count, want.data(), total,
                                           nullptr, 0) == CLAY_OK);
    CHECK(whole == want);
    (void)a;
    clay_brick_cache_destroy(cache);
}

TEST_CASE("c abi: the rest of the document is reachable DURING a gesture") {
    // The user story the calls exist for (#378), and the one the alternative
    // fails. A host cannot hide the layer and sample the rest, because
    // visibility is an EDIT and the transaction's commit correctly refuses a
    // layer that changed underneath it — so this case checks both halves: the
    // excluded evaluation works mid-gesture, AND the commit that follows still
    // succeeds.
    Doc d;
    const clay_layer_id body = sphere_layer(d.doc, "body", 0.0f, 0.6f);
    const clay_layer_id other = sphere_layer(d.doc, "other", 0.9f, 0.4f);
    const std::vector<float> pts = lattice(9, 1.3f);

    clay_sculpt_policy pol;
    std::memset(&pol, 0, sizeof pol);
    pol.struct_size = sizeof pol;
    pol.cell_size = 0.05f;
    clay_sdf_smooth_tx* tx = clay_sdf_smooth_begin(d.doc, body, &pol, nullptr);
    REQUIRE(tx != nullptr);

    clay_relax_params rp;
    std::memset(&rp, 0, sizeof rp);
    rp.struct_size = sizeof rp;
    rp.strength = 0.5f;
    rp.iterations = 2;
    rp.radius_cells = 2;
    rp.region_radius = 0.4f;
    rp.falloff = 0.15f;
    rp.centre[2] = 0.6f;
    REQUIRE(clay_sdf_smooth_update(tx, &rp, nullptr, nullptr) == CLAY_OK);

    // Mid-gesture, the rest of the document is askable — and it is exactly the
    // other layer, which is what a host composes its preview against.
    const std::vector<float> rest = eval_without(d.doc, body, pts);
    CHECK(rest == eval_only(d.doc, other, pts));

    // It recorded no edit: the commit that follows still succeeds, where it
    // would have refused had we toggled visibility to get the same answer.
    clay_sculpt_budget budget;
    std::memset(&budget, 0, sizeof budget);
    budget.struct_size = sizeof budget;
    CHECK(clay_sdf_smooth_commit(tx, &budget) == CLAY_OK);
    clay_sdf_smooth_destroy(tx);

    // And the contrast, so the case above is not passing for an unrelated
    // reason: toggling visibility mid-gesture IS an edit, and that commit is
    // refused.
    clay_sdf_smooth_tx* tx2 = clay_sdf_smooth_begin(d.doc, body, &pol, nullptr);
    REQUIRE(tx2 != nullptr);
    REQUIRE(clay_sdf_smooth_update(tx2, &rp, nullptr, nullptr) == CLAY_OK);
    REQUIRE(clay_document_set_layer_visible(d.doc, body, 0) == CLAY_OK);
    CHECK(clay_sdf_smooth_commit(tx2, &budget) == CLAY_ERROR_INVALID_ARGUMENT);
    clay_sdf_smooth_destroy(tx2);
}
