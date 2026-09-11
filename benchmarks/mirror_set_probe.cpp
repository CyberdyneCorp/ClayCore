// What setting a layer's mirror to the value it ALREADY HAS costs.
// NOT a gated benchmark.
//
// WHY. ClaySpaceDesktop's stall ledger puts `begin stroke` at 92.7 ms average
// over 51 presses — the largest single line in it, larger than the stroke
// itself, and not an edit at all. Reading their arming path: BeginStroke ->
// open_live_gesture -> arm_live_move -> point_the_mirror, and on a field layer
// that is `clay_set_layer_mirror` PER PRESS, whether or not the symmetry
// changed.
//
// On this side, that command used to write the axes and k unconditionally and
// return an inverse (`apply_one(SetLayerMirrorCmd)`), so a no-op recorded an
// undo entry and — through `command_influence_bound` ->
// `layer_command_bound(..., changes_layer_set=false)` — took the WHOLE-LAYER
// invalidation a layer transform takes. `SetLayerCompositionCmd` directly below
// it already showed the other answer: `nullopt`, this vocabulary's "no, the
// document is unchanged".
//
// WHAT THE FIRST VERSION OF THIS PROBE GOT WRONG, recorded because it is the
// whole reason the numbers below are shaped as they are. It counted the bricks
// that went dirty after the set, against a control that made no call at all,
// and read "200 against 0" as the command's doing. It is not:
// `clay_brick_cache_mark_dirty_layer` marks the layer's influence box
// UNCONDITIONALLY (bindings/c/clay_c.cpp), so the arm that called it dirtied
// the layer and the arm that did not dirtied nothing, whatever the command did
// in between. Both arms mark here, and the brick counts come out EQUAL —
// which is the point. A count that is equal on both sides of a fix is not the
// measurement.
//
// WHAT ACTUALLY MOVES is the REFILL, because a whole-layer invalidation drops
// the layer's brick SEEDS (clay_document_resume_stats) and breaks its append
// log. Marked bricks with seeds resume and cost almost nothing; the same
// bricks without seeds pay the whole surviving edit list. So the arms below
// report the seed count either side, and the wall clock of refilling exactly
// the same bricks.
//
// MEASURED, 60-item form, 8-voxel bricks at 0.05, cache warmed over the form,
// Apple M2 Max (Release, cpu backend, CLAY_BUILD_BENCHMARKS=ON), median of four
// interleaved runs. `before` is THIS tree with the two short-circuits disabled
// and nothing else changed, so the two columns differ by the fix alone:
//
//   arm                            bricks      refill ms        seeds    undo
//                                before/after before/after before/after b/a
//   control: no set at all         500 / 500   0.31 /  0.33  788 -> 788   0/0
//   mirror -> its CURRENT value    500 / 500  31.00 /  0.32  788 -> 288   1/0
//                                                            788 -> 788
//   radial -> its CURRENT value    500 / 500  30.91 /  0.30  788 -> 288   1/0
//                                                            788 -> 788
//   mirror -> a DIFFERENT value    600 / 600  49.67 / 50.57  788 -> 168   1/1
//
// A no-op press cost 31.00 ms, one undo entry and 500 of the layer's 788 seeds;
// it now costs 0.32 ms and takes nothing, which is the control to within the
// run-to-run spread. The bricks refilled are identical on both sides in every
// row, so nothing here is a count that moved — it is the same work being
// resumed instead of walked.
//
// THE LAST ROW IS THE ONE THAT MATTERS FOR CORRECTNESS, and it is deliberately
// unchanged: 49.67 ms before, 50.57 after, one undo entry either way, the same
// 620 seeds dropped. A short-circuit that is too eager would make it cheap too,
// and that is not a fast frame, it is a symmetry change the artist asked for
// and did not get.
//
// Exits non-zero if the fixture cannot discriminate — if the warm pass refilled
// nothing, or if a real change does not cost visibly more than a no-op, there
// is nothing here to read — and non-zero if either half of the fix has
// regressed.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

constexpr int kDim = 8;
constexpr float kVoxel = 0.05f;

long drain(clay_brick_cache* cache, const clay_document* doc) {
    long n = 0;
    std::vector<clay_brick_request> reqs(512);
    std::vector<float> vals;
    for (;;) {
        std::size_t count = reqs.size(), remaining = 0;
        if (clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) != CLAY_OK) return -1;
        if (count == 0) break;
        vals.assign(count * static_cast<std::size_t>(kDim) * kDim * kDim, 0.0f);
        if (clay_brick_cache_eval_requests(doc, "cpu", reqs.data(), count, vals.data(), vals.size(),
                                           nullptr, 0) != CLAY_OK)
            return -1;
        std::size_t accepted = 0;
        if (clay_brick_cache_submit(cache, reqs.data(), count, vals.data(), vals.size(), nullptr, 0,
                                    nullptr, &accepted) != CLAY_OK)
            return -1;
        n += static_cast<long>(count);
        if (remaining == 0) break;
    }
    return n;
}

// Bricks currently holding a seed. The seed store belongs to the DOCUMENT, and
// a whole-layer invalidation is what empties it — so this is the state the
// refill below either resumes from or has to rebuild.
std::uint64_t seeds(const clay_document* doc) {
    clay_resume_stats st;
    std::memset(&st, 0, sizeof st);
    st.struct_size = static_cast<std::uint32_t>(sizeof st);
    if (clay_document_resume_stats(doc, &st) != CLAY_OK) return 0;
    return st.entries;
}

// One press-shaped arm: make the call, then do exactly what a host does after
// an edit it cannot bound itself — dirty the layer and refill. Every arm marks,
// including the control, so the brick count is a constant of the fixture and
// the only thing left to differ is whether those bricks could resume.
struct Arm {
    long bricks = 0;
    double refill_ms = 0.0;
    std::uint64_t seeds_before = 0;
    std::uint64_t seeds_after_set = 0;
    std::size_t undo = 0;
    clay_result result = CLAY_OK;
};

template <typename Call>
Arm measure(clay_document* doc, clay_brick_cache* cache, clay_layer_id layer, Call call) {
    Arm a;
    std::size_t before = 0, after = 0;
    clay_document_undo_state(doc, nullptr, &before, nullptr);
    a.seeds_before = seeds(doc);
    a.result = call();
    a.seeds_after_set = seeds(doc);
    clay_document_undo_state(doc, nullptr, &after, nullptr);
    a.undo = after - before;
    clay_brick_cache_mark_dirty_layer(cache, doc, layer);
    const Clock::time_point t0 = Clock::now();
    a.bricks = drain(cache, doc);
    a.refill_ms = ms_since(t0);
    return a;
}

void report(const char* name, const Arm& a) {
    std::printf("  %-34s %8ld %10.2f  %6llu -> %-6llu %6zu\n", name, a.bricks, a.refill_ms,
                static_cast<unsigned long long>(a.seeds_before),
                static_cast<unsigned long long>(a.seeds_after_set), a.undo);
}

}  // namespace

int main() {
    std::printf("mirror_set_probe: what a NO-OP clay_set_layer_mirror costs\n\n");

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    if (!doc || clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        std::printf("FAIL: no document\n");
        return 1;
    }
    clay_document_enable_undo(doc);
    // A form with some extent, so a whole-layer invalidation is visibly more
    // than a local one.
    for (int i = 0; i < 60; ++i) {
        const float t = static_cast<float>(i) / 59.0f;
        const float r = 0.22f;
        clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
        const float p[3] = {std::cos(t * 3.14159f) * 1.2f, std::sin(t * 3.14159f) * 0.6f, 0.0f};
        clay_item_set_position(it, p);
        clay_item_set_op(it, CLAY_OP_ADD);
        clay_item_set_blend(it, CLAY_BLEND_QUADRATIC, 0.05f);
        clay_node_id n = 0;
        clay_layer_add_item(doc, layer, it, &n);
        clay_item_destroy(it);
    }

    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.dim = kDim;
    cfg.voxel_size = kVoxel;
    cfg.band_voxels = 2;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);

    const float lo[3] = {-2.0f, -1.5f, -1.0f}, hi[3] = {2.0f, 1.5f, 1.0f};
    clay_brick_cache_mark_dirty(cache, lo, hi);
    const long warm = drain(cache, doc);
    std::printf("  warm: %ld bricks in the cache, %llu of them holding a seed\n\n", warm,
                static_cast<unsigned long long>(seeds(doc)));
    if (warm <= 0 || seeds(doc) == 0) {
        std::printf("FAIL: the warm pass left no seeds, so there is nothing for an\n"
                    "      invalidation to drop and every arm below measures a cold refill.\n");
        return 1;
    }

    // Set the mirror and the radial mode ONCE each, so the layer has values to
    // be set to again — and refill, so the arms start from a warm cache.
    clay_set_layer_mirror(doc, layer, 1, 0, 0, 0.0f);
    clay_set_layer_radial(doc, layer, 1, 4, 0.0f);
    clay_brick_cache_mark_dirty_layer(cache, doc, layer);
    drain(cache, doc);

    std::printf("  %-34s %8s %10s  %-16s %6s\n", "arm", "bricks", "refill ms", "seeds: set",
                "undo");

    // ARM A: the control — no call at all, then the same dirty-and-refill every
    // other arm does. This is what a press costs when the engine is left alone.
    const Arm control = measure(doc, cache, layer, [] { return CLAY_OK; });
    report("control: no set at all", control);

    // ARM B: set the mirror to EXACTLY what it already carries.
    const Arm noop = measure(doc, cache, layer,
                             [&] { return clay_set_layer_mirror(doc, layer, 1, 0, 0, 0.0f); });
    report("mirror -> its CURRENT value", noop);

    // ARM C: the radial mode, which has the identical shape and is set from the
    // same host path.
    const Arm radial_noop =
        measure(doc, cache, layer, [&] { return clay_set_layer_radial(doc, layer, 1, 4, 0.0f); });
    report("radial -> its CURRENT value", radial_noop);

    // ARM D: a REAL symmetry change, which must still cost what it always did.
    // Without this row the probe would pass just as happily against a setter
    // short-circuited unconditionally, which is the failure worth fearing here:
    // not a slow frame but geometry the artist asked for and did not get.
    const Arm real = measure(doc, cache, layer,
                             [&] { return clay_set_layer_mirror(doc, layer, 1, 1, 0, 0.0f); });
    report("mirror -> a DIFFERENT value", real);

    std::printf("\n");
    int bad = 0;
    if (real.refill_ms < 4.0 * control.refill_ms || real.seeds_after_set >= real.seeds_before) {
        std::printf("  FAIL: a REAL mirror change refilled in %.2f ms against %.2f for the\n"
                    "        control and left %llu of %llu seeds standing. This fixture cannot\n"
                    "        tell an invalidation from no invalidation, so nothing below means\n"
                    "        anything -- fix the fixture before reading the rows.\n",
                    real.refill_ms, control.refill_ms,
                    static_cast<unsigned long long>(real.seeds_after_set),
                    static_cast<unsigned long long>(real.seeds_before));
        ++bad;
    }
    if (noop.result != CLAY_OK || radial_noop.result != CLAY_OK || real.result != CLAY_OK) {
        std::printf("  FAIL: a set returned %d / %d / %d, and a no-op must still be CLAY_OK --\n"
                    "        apply()'s nullopt must not reach a caller as NOT_FOUND.\n",
                    static_cast<int>(noop.result), static_cast<int>(radial_noop.result),
                    static_cast<int>(real.result));
        ++bad;
    }
    if (noop.undo != 0 || noop.seeds_after_set != noop.seeds_before) {
        std::printf("  FAIL: a no-op MIRROR set recorded %zu undo entr%s and took %llu of %llu\n"
                    "        seeds with it. This is #536, back again: the refill cost %.2f ms\n"
                    "        against %.2f for doing nothing.\n",
                    noop.undo, noop.undo == 1 ? "y" : "ies",
                    static_cast<unsigned long long>(noop.seeds_before - noop.seeds_after_set),
                    static_cast<unsigned long long>(noop.seeds_before), noop.refill_ms,
                    control.refill_ms);
        ++bad;
    }
    if (radial_noop.undo != 0 || radial_noop.seeds_after_set != radial_noop.seeds_before) {
        std::printf("  FAIL: a no-op RADIAL set recorded %zu undo entr%s and took %llu of %llu\n"
                    "        seeds with it; the refill cost %.2f ms against %.2f.\n",
                    radial_noop.undo, radial_noop.undo == 1 ? "y" : "ies",
                    static_cast<unsigned long long>(radial_noop.seeds_before -
                                                    radial_noop.seeds_after_set),
                    static_cast<unsigned long long>(radial_noop.seeds_before),
                    radial_noop.refill_ms, control.refill_ms);
        ++bad;
    }
    if (real.undo != 1) {
        std::printf("  FAIL: a REAL mirror change recorded %zu undo entr%s. The short-circuit is\n"
                    "        too eager and has swallowed an edit -- wrong geometry, not a slow\n"
                    "        frame.\n",
                    real.undo, real.undo == 1 ? "y" : "ies");
        ++bad;
    }
    if (bad == 0)
        std::printf("  -> A NO-OP SYMMETRY SET IS FREE. It refills in %.2f ms against %.2f for\n"
                    "     making no call at all, keeps every one of its %llu seeds and records\n"
                    "     no undo entry. A REAL change still drops them and still costs %.2f ms,\n"
                    "     so what a press pays now is what the press actually changed.\n"
                    "     The brick counts (%ld / %ld / %ld) are equal by construction: every\n"
                    "     arm marks the layer, so they are the fixture and not the finding.\n",
                    noop.refill_ms, control.refill_ms,
                    static_cast<unsigned long long>(noop.seeds_after_set), real.refill_ms,
                    control.bricks, noop.bricks, radial_noop.bricks);

    clay_brick_cache_destroy(cache);
    clay_document_destroy(doc);
    if (bad != 0) return 1;
    std::printf("\nok\n");
    return 0;
}
