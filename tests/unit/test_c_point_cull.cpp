// The cull region a bulk point query compiles under (c-abi spec: picking and
// evaluation parity; issue #452).
//
// clay_layer_eval_points, clay_layer_eval_gradients and the two _excluding
// forms compile their own tape, and now cull that compile to the AABB of the
// probes they were handed. THE ONLY THING THAT MAKES THAT LEGITIMATE IS THAT
// NOTHING OBSERVABLE CHANGES: a culled tape agrees with the full one only for
// values inside the band the region was dilated by, and these calls are what
// clay.h points a host at for the field's own distance PAST a band. So the
// band is verified against the answers that come back and a query that reaches
// past it is answered again with no region at all, and every case here is the
// same claim — the same probes, the same document, the cull off and then on,
// and bit equality between them. Not "close": a distance a host steps a march
// with, or bakes into a volume, is a number it is entitled to reproduce.
//
// The two paths agree BY CONSTRUCTION, which is exactly what would let a cull
// that had quietly stopped culling pass every case here. So the cases that
// mean to exercise a culled answer assert that one happened, and the ones that
// mean to exercise the give-up assert that it gave up
// (clay_internal_point_cull_stats).
//
// The fixtures are chosen for the ways the argument can fail rather than for
// coverage: probes clustered where a cull bites hardest, probes spread so it
// bites at all, probes straddling the region's own edge, probes far outside
// every item and probes deep inside the material (the two that reach past any
// band a probe box can name), a layer with no warps and a layer with many, a
// DISTANT SUBTRACTING item (whose field far from itself is a large negative,
// so dropping it is not the identity the way dropping a union is), and an
// ellipsoid, whose Lipschitz number bounds a march's step and not the field's
// slope — which is what the gradient taps would have to reason from.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
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

// Deterministic everywhere, and the same sequence on every machine: a probe
// set that moved between the two arms would prove nothing.
struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    float unit() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((s >> 33) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

clay_item_desc sphere_desc(float r) {
    clay_item_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = static_cast<std::uint32_t>(sizeof d);
    d.prim = CLAY_PRIM_SPHERE;
    d.params[0] = r;
    d.rotation[3] = 1.0f;
    d.scale = 1.0f;
    d.color[0] = 0.8f;
    return d;
}

// A ball worked over with blended dabs, so the chain pad is a real term, and
// `grabs` move-surface warps clustered near +Z, so the deformer cull has
// something to drop for a probe set that is not near them.
clay_layer_id worked_ball(clay_document* doc, int dabs, int grabs) {
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);
    const clay_item_desc base = sphere_desc(1.0f);
    clay_node_id n = 0;
    REQUIRE(clay_add_item(doc, layer, &base, &n) == CLAY_OK);

    Lcg rng(12345);
    for (int i = 0; i < dabs; ++i) {
        const float u = rng.range(-1.0f, 1.0f);
        const float phi = rng.range(0.0f, 6.2831853f);
        const float r = std::sqrt(std::fmax(0.0f, 1.0f - u * u));
        clay_item_desc d = sphere_desc(0.13f);
        d.position[0] = r * std::cos(phi);
        d.position[1] = r * std::sin(phi);
        d.position[2] = u;
        d.blend = CLAY_BLEND_QUADRATIC;
        d.blend_k = 0.06f;
        d.color[1] = 0.5f;
        REQUIRE(clay_add_item(doc, layer, &d, &n) == CLAY_OK);
    }
    for (int i = 0; i < grabs; ++i) {
        clay_move_params mp;
        std::memset(&mp, 0, sizeof mp);
        mp.struct_size = static_cast<std::uint32_t>(sizeof mp);
        mp.radius = 0.3f;
        const float centre[3] = {0.02f * static_cast<float>(i), 0.0f, 1.0f};
        const float pull[3] = {0.0f, 0.0f, 0.03f};
        std::size_t applied = 0;
        REQUIRE(clay_layer_move_surface(doc, layer, centre, pull, &mp, &applied) == CLAY_OK);
    }
    return layer;
}

std::vector<float> cluster(float cx, float cy, float cz, float half, int count,
                           std::uint64_t seed) {
    Lcg rng(seed);
    std::vector<float> p;
    p.reserve(static_cast<std::size_t>(count) * 3);
    for (int i = 0; i < count; ++i) {
        p.push_back(cx + rng.range(-half, half));
        p.push_back(cy + rng.range(-half, half));
        p.push_back(cz + rng.range(-half, half));
    }
    return p;
}

struct Answers {
    std::vector<float> distances, colors, gradients;
    std::uint64_t culled = 0, widened = 0, fallbacks = 0;
};

// Every one of the four calls, over the same probes, with the cull as the
// document has it set.
Answers answer(const clay_document* doc, clay_layer_id layer, clay_layer_id excluded,
               const std::vector<float>& pts) {
    const std::size_t n = pts.size() / 3;
    Answers a;
    a.distances.assign(n ? n : 1, 0.0f);
    a.colors.assign(n ? n * 3 : 3, 0.0f);
    a.gradients.assign(n ? n * 3 : 3, 0.0f);
    std::uint64_t was_culled = 0, was_widened = 0, was_fallbacks = 0;
    REQUIRE(clay_internal_point_cull_stats(doc, &was_culled, &was_widened, &was_fallbacks) ==
            CLAY_OK);
    REQUIRE(clay_layer_eval_points(doc, layer, "cpu", pts.data(), n, a.distances.data(),
                                   a.colors.data()) == CLAY_OK);
    REQUIRE(clay_layer_eval_gradients(doc, layer, "cpu", pts.data(), n, a.gradients.data()) ==
            CLAY_OK);
    if (excluded != 0) {
        std::vector<float> d(n ? n : 1), g(n ? n * 3 : 3);
        REQUIRE(clay_eval_points_excluding(doc, excluded, "cpu", pts.data(), n, d.data(),
                                           nullptr) == CLAY_OK);
        REQUIRE(clay_eval_gradients_excluding(doc, excluded, "cpu", pts.data(), n, g.data()) ==
                CLAY_OK);
        a.distances.insert(a.distances.end(), d.begin(), d.end());
        a.gradients.insert(a.gradients.end(), g.begin(), g.end());
    }
    std::uint64_t culled = 0, widened = 0, fallbacks = 0;
    REQUIRE(clay_internal_point_cull_stats(doc, &culled, &widened, &fallbacks) == CLAY_OK);
    a.culled = culled - was_culled;
    a.widened = widened - was_widened;
    a.fallbacks = fallbacks - was_fallbacks;
    return a;
}

// The whole claim, in one place: the same query with the region and without it
// is the same answer, bit for bit, in every buffer it fills.
void require_identical(clay_document* doc, clay_layer_id layer, clay_layer_id excluded,
                       const std::vector<float>& pts, Answers* out_culled = nullptr) {
    REQUIRE(clay_internal_set_point_cull(doc, 0) == CLAY_OK);
    const Answers plain = answer(doc, layer, excluded, pts);
    REQUIRE(plain.culled == 0);
    REQUIRE(plain.widened == 0);
    REQUIRE(plain.fallbacks == 0);
    REQUIRE(clay_internal_set_point_cull(doc, 1) == CLAY_OK);
    const Answers culled = answer(doc, layer, excluded, pts);

    std::size_t differing = 0;
    for (std::size_t i = 0; i < plain.distances.size(); ++i)
        if (plain.distances[i] != culled.distances[i]) {
            if (differing == 0)
                MESSAGE("first differing distance at probe " << i << ": " << plain.distances[i]
                                                             << " vs " << culled.distances[i]);
            ++differing;
        }
    CHECK(differing == 0);
    std::size_t colours = 0, gradients = 0;
    for (std::size_t i = 0; i < plain.colors.size(); ++i)
        if (plain.colors[i] != culled.colors[i]) ++colours;
    for (std::size_t i = 0; i < plain.gradients.size(); ++i)
        if (plain.gradients[i] != culled.gradients[i]) ++gradients;
    CHECK(colours == 0);
    CHECK(gradients == 0);
    if (out_culled) *out_culled = culled;
}

}  // namespace

TEST_CASE("point cull: clustered probes are answered from a culled tape, exactly") {
    for (int grabs : {0, 12}) {
        CAPTURE(grabs);
        Doc d;
        const clay_layer_id layer = worked_ball(d.doc, 400, grabs);
        // A brush-sized box sitting on the surface at +X, which is the far
        // side of the ball from every grab: the case the cull exists for.
        Answers culled;
        require_identical(d.doc, layer, 0, cluster(1.0f, 0.0f, 0.0f, 0.05f, 512, 7), &culled);
        // And it really was culled — all four queries, none of them giving up.
        CHECK(culled.culled == 2);
        CHECK(culled.fallbacks == 0);
    }
}

TEST_CASE("point cull: probes spread over the whole model") {
    Doc d;
    const clay_layer_id layer = worked_ball(d.doc, 400, 12);
    // The region here covers the model, so nothing is dropped and the tape is
    // the whole one. The case earns its place by pinning that the query is
    // still ANSWERED from the culled path rather than falling back — a
    // covering region is not a failure, and a fallback here would be a second
    // compile for nothing.
    Answers culled;
    require_identical(d.doc, layer, 0, cluster(0.0f, 0.0f, 0.0f, 1.3f, 512, 11), &culled);
    CHECK(culled.culled == 2);
    CHECK(culled.fallbacks == 0);
}

TEST_CASE("point cull: probes straddling the region's own edge") {
    Doc d;
    const clay_layer_id layer = worked_ball(d.doc, 400, 12);
    // A thin slab through the ball's equator: its own diagonal is wide, its
    // thickness is not, so the region's Z faces cut through material and the
    // items the cull keeps are decided by the pad rather than by the box. This
    // is where a dilation that is one term short shows up as a different
    // number at one probe rather than as a different picture.
    std::vector<float> pts;
    Lcg rng(31);
    for (int i = 0; i < 512; ++i) {
        pts.push_back(rng.range(-1.2f, 1.2f));
        pts.push_back(rng.range(-1.2f, 1.2f));
        pts.push_back(rng.range(-0.02f, 0.02f));
    }
    require_identical(d.doc, layer, 0, pts);
}

TEST_CASE("point cull: probes far outside every item fall back and stay exact") {
    Doc d;
    const clay_layer_id layer = worked_ball(d.doc, 400, 12);
    // Nine units out on every axis, in a box a tenth of a unit across. The
    // region keeps NOTHING — so there is no tape to verify a band against, and
    // the query gives the region up and compiles the layer. THE ANSWERS ARE
    // THE DISTANCES TO THE BALL, which is the whole reason the fallback
    // exists: a host asking how far away it is gets a number rather than the
    // CLAY_TAPE_FAR stand-in an empty tape reports.
    Answers culled;
    const std::vector<float> pts = cluster(9.0f, 9.0f, 9.0f, 0.05f, 512, 13);
    require_identical(d.doc, layer, 0, pts, &culled);
    CHECK(culled.culled == 0);
    CHECK(culled.widened == 0);
    CHECK(culled.fallbacks == 2);
    for (std::size_t i = 0; i < 512; ++i) {
        REQUIRE(culled.distances[i] > 13.0f);   // sqrt(3) * 9, less the ball
        REQUIRE(culled.distances[i] < 15.0f);
    }
}

TEST_CASE("point cull: probes deep inside the material fall back and stay exact") {
    Doc d;
    const clay_layer_id layer = worked_ball(d.doc, 400, 12);
    // A unit of material in every direction: the answers are about -1, which
    // no band a 0.1-wide box can name covers either. The interior is the arm
    // of the band test that a one-sided "is the answer small and positive"
    // check would have got wrong.
    // The region keeps the items around it, so there IS a tape, and its
    // answers of about -1 are what say the band was too small — by a factor of
    // six, which is past the point where a wider band is worth compiling (it
    // would swallow the whole model), so this one gives the region up.
    Answers culled;
    require_identical(d.doc, layer, 0, cluster(0.0f, 0.0f, 0.0f, 0.05f, 512, 17), &culled);
    CHECK(culled.culled == 0);
    CHECK(culled.widened == 0);
    CHECK(culled.fallbacks == 2);
    for (std::size_t i = 0; i < 512; ++i) REQUIRE(culled.distances[i] < -0.5f);
}

TEST_CASE("point cull: a distant subtracting item is not dropped out from under a probe") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "body", &layer) == CLAY_OK);
    const clay_item_desc ball = sphere_desc(1.0f);
    clay_node_id n = 0;
    REQUIRE(clay_add_item(d.doc, layer, &ball, &n) == CLAY_OK);
    // A big sphere off to +X, subtracting. Far from the probes below, and its
    // field THERE is a large negative — so `max(acc, -far)` is not the
    // identity the way `min(acc, far)` is, and a cull that treated a subtract
    // like a union would answer an interior probe with the wrong sign of the
    // wrong number. What actually keeps it is that an item whose influence is
    // not local is never dropped; this is the case that would notice if that
    // stopped being true.
    clay_item_desc cut = sphere_desc(2.5f);
    cut.position[0] = 4.0f;
    cut.op = CLAY_OP_SUBTRACT;
    REQUIRE(clay_add_item(d.doc, layer, &cut, &n) == CLAY_OK);

    require_identical(d.doc, layer, 0, cluster(-0.6f, 0.0f, 0.0f, 0.05f, 256, 19));
    require_identical(d.doc, layer, 0, cluster(0.0f, 0.0f, 0.0f, 0.9f, 256, 23));
    require_identical(d.doc, layer, 0, cluster(1.6f, 0.0f, 0.0f, 0.1f, 256, 29));
}

TEST_CASE("point cull: an item the probes' own box does not reach can still be the answer") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "body", &layer) == CLAY_OK);
    // A big blob, and a subtracting sphere pressing a dimple into it well away
    // from the probes below. At the centre the blob alone reads -3; the dimple
    // is the nearest surface there and the document reads -0.9. The dimple's
    // influence bound is 0.85 from the probe box, which is further than any
    // band that box can name — so THE FIRST ATTEMPT DROPS THE ITEM THAT OWNS
    // THE ANSWER, reads -3, and that number is what tells it the band was too
    // small. This is the fixture the whole verification exists for, and the
    // one that fails loudly if the band is ever admitted wider than the region
    // it certifies.
    clay_item_desc blob = sphere_desc(3.0f);
    clay_node_id n = 0;
    REQUIRE(clay_add_item(d.doc, layer, &blob, &n) == CLAY_OK);
    clay_item_desc dimple = sphere_desc(0.6f);
    dimple.position[2] = 1.5f;
    dimple.op = CLAY_OP_SUBTRACT;
    REQUIRE(clay_add_item(d.doc, layer, &dimple, &n) == CLAY_OK);

    Answers culled;
    require_identical(d.doc, layer, 0, cluster(0.0f, 0.0f, 0.0f, 0.05f, 256, 61), &culled);
    CHECK(culled.culled == 0);
    CHECK(culled.fallbacks == 2);
    // The dimple, not the blob: -0.9 and not -3.
    for (std::size_t i = 0; i < 256; ++i) REQUIRE(culled.distances[i] > -1.1f);
}

TEST_CASE("point cull: a cluster hovering off the surface is answered under a wider band") {
    Doc d;
    const clay_layer_id layer = worked_ball(d.doc, 400, 12);
    // A brush-sized box a third of a unit OFF the surface. Its answers are
    // about twice the band its own diagonal names — too far for the first
    // attempt, near enough that a band twice as wide still drops most of the
    // ball rather than swallowing it. This is the query the second attempt
    // exists for, and it is here so that a change which quietly stopped
    // widening fails rather than merely slows down (it measured 5.7x with the
    // second attempt and 0.98x without).
    Answers culled;
    require_identical(d.doc, layer, 0, cluster(1.45f, 0.0f, 0.0f, 0.05f, 512, 67), &culled);
    CHECK(culled.culled == 0);
    CHECK(culled.widened == 2);
    CHECK(culled.fallbacks == 0);
}

TEST_CASE("point cull: a single probe, an empty set, and a count below the floor") {
    Doc d;
    const clay_layer_id layer = worked_ball(d.doc, 64, 4);
    // None of these is culled: a box with no extent names no neighbourhood,
    // and a box too few probes span is too small an estimate of one to be
    // worth a compile (kMinCulledProbes says what that measured). They are
    // here because they are the inputs where the derivation degenerates — an
    // empty box, a zero diagonal — and answering them at all is the claim.
    for (int count : {1, 2, 7, 32}) {
        CAPTURE(count);
        Answers culled;
        require_identical(d.doc, layer, 0, cluster(1.0f, 0.0f, 0.0f, 0.05f, count, 37), &culled);
        CHECK(culled.culled == 0);
        CHECK(culled.widened == 0);
        CHECK(culled.fallbacks == 0);
    }
    // No probes at all, through a buffer that exists: the calls refuse a null
    // one (they always have), and a count of zero is an answer with nothing in
    // it rather than an error.
    const float none[3] = {0.0f, 0.0f, 0.0f};
    float sink = 0.0f;
    CHECK(clay_layer_eval_points(d.doc, layer, "cpu", none, 0, &sink, nullptr) == CLAY_OK);
    CHECK(clay_layer_eval_gradients(d.doc, layer, "cpu", none, 0, &sink) == CLAY_OK);
    // Coincident probes are a degenerate box however many of them there are.
    std::vector<float> same;
    for (int i = 0; i < 64; ++i) {
        same.push_back(1.0f);
        same.push_back(0.0f);
        same.push_back(0.0f);
    }
    Answers culled;
    require_identical(d.doc, layer, 0, same, &culled);
    CHECK(culled.culled == 0);
}

TEST_CASE("point cull: the document without one layer culls the same way") {
    Doc d;
    const clay_layer_id body = worked_ball(d.doc, 200, 6);
    clay_layer_id other = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "bump", &other) == CLAY_OK);
    clay_item_desc d2 = sphere_desc(0.5f);
    d2.position[0] = 1.4f;
    clay_node_id n = 0;
    REQUIRE(clay_add_item(d.doc, other, &d2, &n) == CLAY_OK);

    // Both _excluding forms run inside require_identical when `excluded` is
    // non-zero, so each of these is four culled queries against four uncalled
    // ones.
    Answers culled;
    require_identical(d.doc, body, other, cluster(1.0f, 0.0f, 0.0f, 0.06f, 512, 41), &culled);
    CHECK(culled.culled == 4);
    require_identical(d.doc, body, other, cluster(0.0f, 0.0f, 0.0f, 1.4f, 256, 43));
    require_identical(d.doc, body, other, cluster(7.0f, 0.0f, 0.0f, 0.05f, 256, 47));
}

TEST_CASE("point cull: a field whose Lipschitz bound is not a slope bound") {
    Doc d;
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(d.doc, "shapes", &layer) == CLAY_OK);
    clay_item_desc ell;
    std::memset(&ell, 0, sizeof ell);
    ell.struct_size = static_cast<std::uint32_t>(sizeof ell);
    ell.prim = CLAY_PRIM_ELLIPSOID;
    ell.params[0] = 1.0f;
    ell.params[1] = 0.35f;
    ell.params[2] = 0.2f;
    ell.rotation[3] = 1.0f;
    ell.scale = 1.0f;
    clay_node_id n = 0;
    REQUIRE(clay_add_item(d.doc, layer, &ell, &n) == CLAY_OK);
    clay_item_desc ball = sphere_desc(0.3f);
    ball.position[1] = 0.6f;
    ball.blend = CLAY_BLEND_QUADRATIC;
    ball.blend_k = 0.05f;
    REQUIRE(clay_add_item(d.doc, layer, &ball, &n) == CLAY_OK);

    // An ellipsoid's field UNDERESTIMATES the distance, so its declared
    // Lipschitz number bounds a march's step and not the field's slope — 1.09
    // measured near its tips, 3.6 on a needle (scene/tape.h). The gradient
    // path reasons about VALUES at the taps from that number, so where it does
    // not bound the slope the query must not be culled at all. The distance
    // path is unaffected, and both must still answer exactly.
    require_identical(d.doc, layer, 0, cluster(0.95f, 0.0f, 0.0f, 0.04f, 256, 53));
    require_identical(d.doc, layer, 0, cluster(0.0f, 0.6f, 0.0f, 0.08f, 256, 59));
}
