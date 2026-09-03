// The uniform-brick gate (bindings/c/clay_c.cpp, prove_uniform).
//
// A dab on a sculpted surface dirties a solid box of bricks, and most of that
// box is clay: on the sculpted-sphere fixture 57 of the 80 bricks one dab
// dirties are uniformly inside. Each used to pay 512 walks of its culled tape
// to discover it stores nothing. One evaluation at the lattice centre and the
// tape's own Lipschitz bound prove the brick uniform -- |f(c)| > band + L*hd
// puts every sample beyond the band with the centre's sign -- and a stub tape
// that classifies the same way stands in for the 512 walks.
//
// What these cases hold, in order of what would hurt most if it broke:
//
//   1. WHAT THE CACHE STORES IS BIT-IDENTICAL with and without the gate --
//      state, halves and colours, brick for brick. The gate is a proof about
//      submit's classification, and a proof that was wrong once would store a
//      hole in the model with nothing in the values to notice it by.
//   2. A GATED BRICK'S SEED IS ITS PROOF, not the stub: the next dab carries
//      it through the suffix and re-proves it, and what the cache then stores
//      equals a from-scratch fill -- with the dab that reaches a gated brick
//      sending it down the full path, and that equal too.
//   3. THE PROOF RATE on the fixture the gate was built on stays above a
//      floor, so a change that quietly switched the gate off fails rather than
//      merely slowing down (a perf win that switches off its own gate reads as
//      correct forever).
//   4. A per-layer half is never gated: it is a partial field.
//   5. A GPU backend classifies a gated brick as the CPU does: the stub is an
//      ordinary tape in an ordinary batch slot.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clay.h"
#include "clay_internal.h"

namespace {

struct Doc {
    clay_document* d = clay_document_create();
    clay_layer_id layer = 0;
    Doc() { REQUIRE(clay_add_sdf_layer(d, "sculpt", &layer) == CLAY_OK); }
    ~Doc() { clay_document_destroy(d); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

clay_node_id add_ball(clay_document* doc, clay_layer_id layer, float r, float x, float y, float z,
                      int32_t op = CLAY_OP_ADD, float k = 0.0f, const float* rgb = nullptr) {
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    REQUIRE(it != nullptr);
    const float p[3] = {x, y, z};
    REQUIRE(clay_item_set_position(it, p) == CLAY_OK);
    REQUIRE(clay_item_set_op(it, op) == CLAY_OK);
    if (k > 0.0f) REQUIRE(clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, k) == CLAY_OK);
    if (rgb) REQUIRE(clay_item_set_color(it, rgb) == CLAY_OK);
    clay_node_id node = 0;
    REQUIRE(clay_layer_add_item(doc, layer, it, &node) == CLAY_OK);
    clay_item_destroy(it);
    return node;
}

// A worked sphere: a coloured base plus `n` blended, individually coloured
// dabs spread over it, so bricks near the surface hold many items and a
// uniform brick's colour sample has something to be wrong about.
void worked_sphere(clay_document* doc, clay_layer_id layer, int n) {
    const float base_rgb[3] = {0.8f, 0.5f, 0.2f};
    add_ball(doc, layer, 1.0f, 0.0f, 0.0f, 0.0f, CLAY_OP_ADD, 0.0f, base_rgb);
    const float golden = 2.39996323f;
    for (int i = 0; i < n; ++i) {
        const float t = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const float r = std::sqrt(std::fmax(0.0f, 1.0f - t * t));
        const float a = golden * static_cast<float>(i);
        const float rgb[3] = {0.2f + 0.6f * static_cast<float>(i % 5) / 4.0f,
                              0.2f + 0.6f * static_cast<float>(i % 7) / 6.0f, 0.4f};
        add_ball(doc, layer, 0.06f, r * std::cos(a), t, r * std::sin(a), CLAY_OP_ADD, 0.06f, rgb);
    }
}

// bench_unspent's fixture: a sphere r=0.5 plus `dabs` smooth-union dabs
// walking a 3-turn helix on its surface. The gate's proof rate was measured
// here, at a 0.01 voxel and a 3-voxel band.
void helix_sphere(clay_document* doc, clay_layer_id layer, int dabs) {
    add_ball(doc, layer, 0.5f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < dabs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(dabs);
        const float a = t * 6.2831853f * 3.0f;
        const float z = -0.9f + 1.8f * t;
        const float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z)) * 0.5f;
        add_ball(doc, layer, 0.05f, r * std::cos(a), r * std::sin(a), z * 0.5f, CLAY_OP_ADD,
                 0.06f);
    }
}

clay_brick_cache* make_cache(float voxel, int32_t band_voxels, bool colours) {
    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    REQUIRE(clay_brick_config_defaults(&cfg) == CLAY_OK);
    cfg.dim = 8;
    cfg.voxel_size = voxel;
    cfg.band_voxels = band_voxels;
    cfg.colors = colours ? 1 : 0;
    cfg.memory_budget = 0;
    clay_brick_cache* c = clay_brick_cache_create(&cfg);
    REQUIRE(c != nullptr);
    return c;
}

std::vector<clay_brick_request> take_all(clay_brick_cache* cache) {
    std::vector<clay_brick_request> out;
    for (;;) {
        clay_brick_stats st;
        std::memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        REQUIRE(clay_brick_cache_stats(cache, &st) == CLAY_OK);
        if (st.dirty_bricks == 0) break;
        std::vector<clay_brick_request> buf(static_cast<std::size_t>(st.dirty_bricks));
        std::size_t count = buf.size(), remaining = 0;
        REQUIRE(clay_brick_cache_take_dirty(cache, buf.data(), &count, &remaining) == CLAY_OK);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(count));
        if (remaining == 0) break;
    }
    return out;
}

std::vector<clay_brick_request> mark_box(clay_brick_cache* cache, const float lo[3],
                                         const float hi[3]) {
    REQUIRE(clay_brick_cache_mark_dirty(cache, lo, hi) == CLAY_OK);
    return take_all(cache);
}

std::uint64_t gated_of(const clay_document* doc) {
    std::uint64_t g = 0;
    REQUIRE(clay_internal_gated_bricks(doc, &g) == CLAY_OK);
    return g;
}

clay_resume_stats resume_of(const clay_document* doc) {
    clay_resume_stats s;
    std::memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    REQUIRE(clay_document_resume_stats(doc, &s) == CLAY_OK);
    return s;
}

// One refill through the ABI, submitted, with the counters it moved.
struct Fill {
    std::size_t count = 0;
    std::uint64_t gated = 0, resumed = 0, refilled = 0;
};

Fill fill(clay_document* doc, clay_brick_cache* cache, const std::vector<clay_brick_request>& reqs,
          const char* backend, bool colours) {
    Fill f;
    f.count = reqs.size();
    const std::size_t per = 8 * 8 * 8;
    std::vector<float> values(reqs.size() * per);
    std::vector<float> rgb(colours ? reqs.size() * per * 3 : 0);
    const std::uint64_t g0 = gated_of(doc);
    const clay_resume_stats r0 = resume_of(doc);
    REQUIRE(clay_brick_cache_eval_requests(doc, backend, reqs.data(), reqs.size(), values.data(),
                                           values.size(), colours ? rgb.data() : nullptr,
                                           rgb.size()) == CLAY_OK);
    const clay_resume_stats r1 = resume_of(doc);
    f.gated = gated_of(doc) - g0;
    f.resumed = r1.resumed_bricks - r0.resumed_bricks;
    f.refilled = r1.refilled_bricks - r0.refilled_bricks;
    std::vector<int32_t> results(reqs.size());
    std::size_t accepted = 0;
    REQUIRE(clay_brick_cache_submit(cache, reqs.data(), reqs.size(), values.data(),
                                    values.size(), colours ? rgb.data() : nullptr, rgb.size(),
                                    results.data(), &accepted) == CLAY_OK);
    REQUIRE(accepted == reqs.size());
    return f;
}

// Everything the cache stores for these keys, in its stored form.
struct Stored {
    std::vector<int32_t> states;
    std::vector<std::uint16_t> halves;
    std::vector<std::uint8_t> rgba;
    std::size_t surface = 0, inside = 0, outside = 0;
};

Stored stored(const clay_brick_cache* cache, const std::vector<clay_brick_request>& reqs,
              bool colours) {
    Stored s;
    const std::size_t per = 8 * 8 * 8;
    std::vector<int32_t> keys(reqs.size() * 3);
    for (std::size_t i = 0; i < reqs.size(); ++i)
        for (int a = 0; a < 3; ++a) keys[i * 3 + a] = reqs[i].key[a];
    s.states.resize(reqs.size());
    s.halves.resize(reqs.size() * per);
    s.rgba.resize(colours ? reqs.size() * per * 4 : 0);
    REQUIRE(clay_brick_cache_read_bricks(cache, 0, keys.data(), reqs.size(), 0, s.states.data(),
                                         s.halves.data(), s.halves.size(),
                                         colours ? s.rgba.data() : nullptr,
                                         s.rgba.size()) == CLAY_OK);
    for (int32_t st : s.states) {
        if (st == CLAY_BRICK_SURFACE) ++s.surface;
        if (st == CLAY_BRICK_INSIDE) ++s.inside;
        if (st == CLAY_BRICK_OUTSIDE) ++s.outside;
    }
    return s;
}

void check_same_stored(const Stored& a, const Stored& b) {
    CHECK(a.states == b.states);
    CHECK(a.halves == b.halves);
    CHECK(a.rgba == b.rgba);
}

// The window every stroke case below fills: deep inside the worked sphere,
// reaching its shell at the corners, so it mixes uniform bricks with surface
// ones and holds the gate's colour sample to something.
const float kLo[3] = {-0.6f, -0.6f, -0.6f};
const float kHi[3] = {0.6f, 0.6f, 0.6f};

}  // namespace

TEST_CASE("uniform gate: what the cache stores is bit-identical with and without it") {
    for (const bool colours : {false, true}) {
        CAPTURE(colours);
        // Fresh documents per pass: a document filled once holds a seed for
        // every brick, and the second pass would resume from them -- from
        // the proofs, for the gated bricks -- rather than prove them again.
        Doc walked, gated;
        REQUIRE(clay_internal_set_uniform_gate(walked.d, 0) == CLAY_OK);
        worked_sphere(walked.d, walked.layer, 120);
        worked_sphere(gated.d, gated.layer, 120);
        clay_brick_cache* a = make_cache(0.05f, 3, colours);
        clay_brick_cache* b = make_cache(0.05f, 3, colours);
        // The whole model: surface, inside and outside bricks alike.
        REQUIRE(clay_brick_cache_mark_dirty_layer(a, walked.d, walked.layer) == CLAY_OK);
        REQUIRE(clay_brick_cache_mark_dirty_layer(b, gated.d, gated.layer) == CLAY_OK);
        const std::vector<clay_brick_request> ra = take_all(a), rb = take_all(b);
        REQUIRE(ra.size() == rb.size());
        REQUIRE(ra.size() > 100);

        const Fill fa = fill(walked.d, a, ra, "cpu", colours);
        const Fill fb = fill(gated.d, b, rb, "cpu", colours);
        CHECK(fa.gated == 0);
        CHECK(fb.gated > 0);

        const Stored sa = stored(a, ra, colours), sb = stored(b, rb, colours);
        REQUIRE(sa.surface > 0);
        REQUIRE(sa.inside > 0);
        REQUIRE(sa.outside > 0);
        // A proof never lands on a surface brick: the gate answers at most the
        // uniform ones, and on this model most of them.
        CHECK(fb.gated <= sb.inside + sb.outside);
        CHECK(fb.gated >= (sb.inside + sb.outside) / 2);
        check_same_stored(sa, sb);

        // Every brick left a seed of one kind or the other; a gated brick's is
        // its proof, which the byte count can see is not a lattice.
        const clay_resume_stats rs = resume_of(gated.d);
        CHECK(rs.entries == rb.size());
        CHECK(rs.bytes < resume_of(walked.d).bytes);
        clay_brick_cache_destroy(a);
        clay_brick_cache_destroy(b);
    }
}

TEST_CASE("uniform gate: a gated brick's seed is its proof, and the next dab is right") {
    Doc doc;
    worked_sphere(doc.d, doc.layer, 60);
    clay_brick_cache* cache = make_cache(0.05f, 3, true);
    const std::vector<clay_brick_request> window = mark_box(cache, kLo, kHi);
    REQUIRE(window.size() > 8);

    const Fill cold = fill(doc.d, cache, window, "cpu", true);
    CHECK(cold.refilled == window.size());
    REQUIRE(cold.gated > 0);
    CHECK(resume_of(doc.d).entries == window.size());  // a seed per brick, of either kind

    // The same requests again: the proof is at the current revision, so the
    // brick is answered from it without a compile, and counts as resumed.
    REQUIRE(clay_brick_cache_mark_dirty(cache, kLo, kHi) == CLAY_OK);
    const std::vector<clay_brick_request> again = take_all(cache);
    const Fill same = fill(doc.d, cache, again, "cpu", true);
    CHECK(same.resumed == again.size());
    CHECK(same.refilled == 0);
    CHECK(same.gated == 0);

    // A dab INSIDE the clay, within reach of the window's +x bricks: its suffix
    // is not empty for them, so the proof is carried through a fold that
    // changes both the centre value and sample n/2's colour -- and the brick
    // stays uniform, so every brick of the window resumes, none walks.
    const float dab_rgb[3] = {0.1f, 0.9f, 0.3f};
    add_ball(doc.d, doc.layer, 0.1f, 0.75f, 0.0f, 0.0f, CLAY_OP_ADD, 0.05f, dab_rgb);
    REQUIRE(clay_brick_cache_mark_dirty(cache, kLo, kHi) == CLAY_OK);
    const std::vector<clay_brick_request> after = take_all(cache);
    const Fill warm = fill(doc.d, cache, after, "cpu", true);
    CHECK(warm.resumed == after.size());
    CHECK(warm.refilled == 0);
    CHECK(warm.gated == 0);

    // And what it stored is what a document that never resumed stores, with
    // and without the gate.
    for (const bool gate : {false, true}) {
        CAPTURE(gate);
        Doc oracle;
        REQUIRE(clay_internal_set_uniform_gate(oracle.d, gate ? 1 : 0) == CLAY_OK);
        worked_sphere(oracle.d, oracle.layer, 60);
        add_ball(oracle.d, oracle.layer, 0.1f, 0.75f, 0.0f, 0.0f, CLAY_OP_ADD, 0.05f, dab_rgb);
        clay_brick_cache* fresh = make_cache(0.05f, 3, true);
        const std::vector<clay_brick_request> reqs = mark_box(fresh, kLo, kHi);
        fill(oracle.d, fresh, reqs, "cpu", true);
        check_same_stored(stored(fresh, reqs, true), stored(cache, after, true));
        clay_brick_cache_destroy(fresh);
    }

    // A carve that REACHES gated bricks: a subtract at the heart of the window
    // brings the surface into clay that was uniformly inside. Those bricks fail
    // their re-proof and take the full path; the result still equals a fill
    // from scratch.
    add_ball(doc.d, doc.layer, 0.3f, 0.0f, 0.0f, 0.0f, CLAY_OP_SUBTRACT);
    REQUIRE(clay_brick_cache_mark_dirty(cache, kLo, kHi) == CLAY_OK);
    const std::vector<clay_brick_request> carved = take_all(cache);
    const Fill carve = fill(doc.d, cache, carved, "cpu", true);
    CHECK(carve.refilled > 0);
    CHECK(carve.resumed + carve.refilled == carved.size());
    const Stored got = stored(cache, carved, true);
    CHECK(got.surface > 0);
    for (const bool gate : {false, true}) {
        CAPTURE(gate);
        Doc oracle;
        REQUIRE(clay_internal_set_uniform_gate(oracle.d, gate ? 1 : 0) == CLAY_OK);
        worked_sphere(oracle.d, oracle.layer, 60);
        add_ball(oracle.d, oracle.layer, 0.1f, 0.75f, 0.0f, 0.0f, CLAY_OP_ADD, 0.05f, dab_rgb);
        add_ball(oracle.d, oracle.layer, 0.3f, 0.0f, 0.0f, 0.0f, CLAY_OP_SUBTRACT);
        clay_brick_cache* fresh = make_cache(0.05f, 3, true);
        const std::vector<clay_brick_request> reqs = mark_box(fresh, kLo, kHi);
        fill(oracle.d, fresh, reqs, "cpu", true);
        check_same_stored(stored(fresh, reqs, true), got);
        clay_brick_cache_destroy(fresh);
    }
    clay_brick_cache_destroy(cache);
}

TEST_CASE("uniform gate: the proof rate on the sculpted fixture holds its floor") {
    // Measured at 52 of 57 uniform bricks (91%) when the gate was built. The
    // floor sits well under that -- the point is a gate that stopped firing,
    // which reads as 0 of 57 and as a correct document.
    Doc doc;
    helix_sphere(doc.d, doc.layer, 400);
    clay_brick_cache* cache = make_cache(0.01f, 3, false);
    const float reach = 0.05f + 0.06f;
    const float lo[3] = {0.5f - reach, -reach, -reach}, hi[3] = {0.5f + reach, reach, reach};
    const std::vector<clay_brick_request> window = mark_box(cache, lo, hi);
    const Fill f = fill(doc.d, cache, window, "cpu", false);
    const Stored s = stored(cache, window, false);
    const std::size_t uniform = s.inside + s.outside;
    CAPTURE(window.size());
    CAPTURE(uniform);
    CAPTURE(f.gated);
    REQUIRE(uniform >= 40);
    CHECK(f.gated >= uniform * 3 / 4);
    CHECK(f.gated <= uniform);
    clay_brick_cache_destroy(cache);
}

TEST_CASE("uniform gate: a per-layer half is never gated") {
    // With a second visible SDF layer the refill evaluates the active layer
    // and the layers beneath as two fields and unions them itself. Neither
    // half's centre value or bound says anything about the union, so the gate
    // stays out of it -- and the result is the plain walk's.
    Doc doc;
    worked_sphere(doc.d, doc.layer, 40);
    clay_layer_id second = 0;
    REQUIRE(clay_add_sdf_layer(doc.d, "second", &second) == CLAY_OK);
    add_ball(doc.d, second, 0.4f, 1.0f, 0.0f, 0.0f);
    clay_brick_cache* cache = make_cache(0.05f, 3, true);
    const std::vector<clay_brick_request> window = mark_box(cache, kLo, kHi);
    const Fill f = fill(doc.d, cache, window, "cpu", true);
    CHECK(f.gated == 0);
    CHECK(f.refilled == window.size());

    Doc oracle;
    REQUIRE(clay_internal_set_uniform_gate(oracle.d, 0) == CLAY_OK);
    worked_sphere(oracle.d, oracle.layer, 40);
    clay_layer_id second2 = 0;
    REQUIRE(clay_add_sdf_layer(oracle.d, "second", &second2) == CLAY_OK);
    add_ball(oracle.d, second2, 0.4f, 1.0f, 0.0f, 0.0f);
    clay_brick_cache* fresh = make_cache(0.05f, 3, true);
    const std::vector<clay_brick_request> reqs = mark_box(fresh, kLo, kHi);
    fill(oracle.d, fresh, reqs, "cpu", true);
    check_same_stored(stored(fresh, reqs, true), stored(cache, window, true));
    clay_brick_cache_destroy(fresh);
    clay_brick_cache_destroy(cache);
}

TEST_CASE("uniform gate: a device backend classifies a gated brick as the cpu does") {
    // The stub is an ordinary tape in an ordinary batch slot, so a backend
    // that evaluates the batch on a device sees nothing special -- and must
    // classify what it produces exactly as the CPU classifies the same stub.
    Doc cpu_doc, gpu_doc;
    worked_sphere(cpu_doc.d, cpu_doc.layer, 60);
    worked_sphere(gpu_doc.d, gpu_doc.layer, 60);
    clay_brick_cache* cpu_cache = make_cache(0.05f, 3, true);
    clay_brick_cache* gpu_cache = make_cache(0.05f, 3, true);
    const std::vector<clay_brick_request> cw = mark_box(cpu_cache, kLo, kHi);
    const std::vector<clay_brick_request> gw = mark_box(gpu_cache, kLo, kHi);
    REQUIRE(cw.size() == gw.size());

    const std::size_t per = 8 * 8 * 8;
    std::vector<float> values(gw.size() * per), rgb(gw.size() * per * 3);
    const clay_result r = clay_brick_cache_eval_requests(gpu_doc.d, "metal", gw.data(), gw.size(),
                                                         values.data(), values.size(),
                                                         rgb.data(), rgb.size());
    if (r == CLAY_ERROR_NOT_FOUND) {
        MESSAGE("no metal backend in this build; skipping");
    } else {
        REQUIRE(r == CLAY_OK);
        REQUIRE(gated_of(gpu_doc.d) > 0);
        std::vector<int32_t> results(gw.size());
        std::size_t accepted = 0;
        REQUIRE(clay_brick_cache_submit(gpu_cache, gw.data(), gw.size(), values.data(),
                                        values.size(), rgb.data(), rgb.size(), results.data(),
                                        &accepted) == CLAY_OK);
        const Fill f = fill(cpu_doc.d, cpu_cache, cw, "cpu", true);
        REQUIRE(f.gated > 0);
        const Stored c = stored(cpu_cache, cw, true), g = stored(gpu_cache, gw, true);
        // The uniform bricks -- which is where the gate acts -- agree brick for
        // brick, colour included: the stub's colour is set on the host and
        // crosses the device verbatim. Surface bricks are held to the parity
        // suite's cross-backend tolerance elsewhere, not to bit identity here.
        std::size_t compared = 0;
        for (std::size_t i = 0; i < cw.size(); ++i) {
            if (c.states[i] == CLAY_BRICK_SURFACE) continue;
            CAPTURE(i);
            CHECK(g.states[i] == c.states[i]);
            for (std::size_t k = 0; k < per * 4; ++k)
                if (g.rgba[i * per * 4 + k] != c.rgba[i * per * 4 + k]) {
                    CHECK(g.rgba[i * per * 4 + k] == c.rgba[i * per * 4 + k]);
                    break;
                }
            ++compared;
        }
        CHECK(compared > 0);
    }
    clay_brick_cache_destroy(cpu_cache);
    clay_brick_cache_destroy(gpu_cache);
}
