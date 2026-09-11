// What FOUR MOVE DABS ON A PLAIN SPHERE actually cost. NOT a gated benchmark.
//
// WHY THIS EXISTS, and it is a correction rather than a continuation. A day of
// measurement on both sides of this project converged on "a Move drag is
// expensive because the material under the pointer is expensive" — 385 ms per
// pointer event with 400 stamps under the brush, 1.86 ms with the same 400 a
// hemisphere away. Then the actual report arrived:
//
//     "it takes almost a second after I do 3 or 4 move dabs in a simple sphere"
//
// A SPHERE IS ONE ITEM. It is the low-density case, the one every number we
// took says should be the fast one (0.41 ms/event on an empty document here).
// So the converged explanation does not cover the reported symptom, and
// something nobody measured is doing this.
//
// TWO THINGS NOBODY MEASURED, both visible in that one sentence:
//
//   "DABS", plural, discrete. Every measurement either side ran was inside ONE
//   gesture — segments of a single drag. Nothing looked at what accumulates
//   ACROSS gestures.
//
//   THE COMMIT. My earlier probes cancelled the transaction; the host's drove
//   segments. clay_sdf_move_commit — which writes the chains and then runs the
//   complexity policy inside the same undo step — was never on either clock.
//
// SO THIS TIMES EVERY PHASE OF EVERY DAB SEPARATELY, and after each one asks
// the field what it now costs: warps carried, the Lipschitz bound, the safe
// step scale, and an actual raycast. The hypothesis worth killing first is
// that the stall is not in the EDIT at all but in what the edit does to
// RAYCASTING: dabs in one spot overlap by definition, overlapping grabs
// legitimately compound the declared bound, and a collapsed safe_step_scale
// makes every ray burn its whole iteration budget. That would land AFTER the
// edit returns, which is exactly what "almost a second after 3 or 4 dabs"
// describes.
//
// A NEGATIVE RESULT IS THE POINT AS MUCH AS A POSITIVE ONE. If four dabs on a
// sphere are microseconds here, the second the user sees is host-side and this
// file says so with numbers, which is worth as much as finding it.

#include <algorithm>
#include <array>
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

constexpr int kDabs = 8;       // more than the reported 3-4, to see the trend
constexpr int kSegments = 6;   // pointer events within one dab
constexpr float kRadius = 0.40f;
constexpr int kDim = 8;
constexpr float kVoxel = 0.05f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

// A PLAIN SPHERE. Deliberately the user's fixture and not a blockout: the whole
// point is that this is the case our density explanation says is free.
bool build(Doc* out) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!item) {
        clay_document_destroy(doc);
        return false;
    }
    clay_item_set_op(item, CLAY_OP_ADD);
    clay_node_id node = 0;
    const clay_result res = clay_layer_add_item(doc, layer, item, &node);
    clay_item_destroy(item);
    if (res != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    out->doc = doc;
    out->layer = layer;
    return true;
}

long drain(clay_brick_cache* cache, const clay_document* against) {
    long refilled = 0;
    std::vector<clay_brick_request> reqs(512);
    std::vector<float> values;
    for (;;) {
        std::size_t count = reqs.size(), remaining = 0;
        if (clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) != CLAY_OK)
            return -1;
        if (count == 0) break;
        values.assign(count * static_cast<std::size_t>(kDim) * kDim * kDim, 0.0f);
        if (clay_brick_cache_eval_requests(against, "cpu", reqs.data(), count, values.data(),
                                           values.size(), nullptr, 0) != CLAY_OK)
            return -1;
        std::size_t accepted = 0;
        if (clay_brick_cache_submit(cache, reqs.data(), count, values.data(), values.size(),
                                    nullptr, 0, nullptr, &accepted) != CLAY_OK)
            return -1;
        refilled += static_cast<long>(count);
        if (remaining == 0) break;
    }
    return refilled;
}

// A render's worth of rays at the form, which is what a collapsed safe step
// scale would punish. Timed apart from every edit.
double raycast_ms(const clay_brick_cache* cache, int side) {
    const Clock::time_point t0 = Clock::now();
    int hits = 0;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j) {
            const float u = -1.5f + 3.0f * static_cast<float>(i) / static_cast<float>(side - 1);
            const float v = -1.5f + 3.0f * static_cast<float>(j) / static_cast<float>(side - 1);
            const float origin[3] = {u, v, 4.0f};
            const float dir[3] = {0.0f, 0.0f, -1.0f};
            int32_t hit = 0;
            float t = 0.0f, pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
            if (clay_brick_cache_raycast(cache, origin, dir, &hit, &t, pos, nrm) == CLAY_OK)
                hits += hit ? 1 : 0;
        }
    const double ms = ms_since(t0);
    if (hits == 0) std::printf("      (warning: no ray hit the form — the raycast timed nothing)\n");
    return ms;
}

// The OTHER raycast, and the one that matters. clay_brick_cache_raycast walks
// the cached fp16 lattice and is immune to what the tape's declared bound does;
// clay_raycast SPHERE-TRACES THE TAPE and steps by f(p)/L, so a collapsed
// safe_step_scale multiplies the number of steps every ray takes.
//
// This is the call a host picks with — ClaySpaceDesktop's pick.rs uses
// clay_raycast on every cursor move — so if the bound collapses, the CURSOR
// gets slow, after the edit has already returned. That is the shape of "it
// takes almost a second after I do 3 or 4 move dabs".
double tape_raycast_ms(const clay_document* doc, int side) {
    const Clock::time_point t0 = Clock::now();
    int hits = 0;
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j) {
            const float u = -1.2f + 2.4f * static_cast<float>(i) / static_cast<float>(side - 1);
            const float v = -1.2f + 2.4f * static_cast<float>(j) / static_cast<float>(side - 1);
            const float origin[3] = {u, v, 4.0f};
            const float dir[3] = {0.0f, 0.0f, -1.0f};
            int32_t hit = 0;
            float t = 0.0f, pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
            if (clay_raycast(doc, origin, dir, &hit, &t, pos, nrm) == CLAY_OK)
                hits += hit ? 1 : 0;
        }
    const double ms = ms_since(t0);
    if (hits == 0) std::printf("      (warning: no tape ray hit — timing nothing)\n");
    return ms;
}

}  // namespace

int main() {
    std::printf("move_dabs_probe: what %d discrete Move dabs cost on a PLAIN SPHERE\n"
                "  (%d segments per dab, radius %.2f, voxel %.3f)\n\n",
                kDabs, kSegments, static_cast<double>(kRadius), static_cast<double>(kVoxel));

    Doc d;
    if (!build(&d)) {
        std::printf("FAIL: could not build the sphere\n");
        return 1;
    }
    clay_document_enable_undo(d.doc);

    clay_brick_config cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.dim = kDim;
    cfg.voxel_size = kVoxel;
    cfg.band_voxels = 2;
    clay_brick_cache* cache = clay_brick_cache_create(&cfg);
    if (!cache) {
        std::printf("FAIL: no brick cache\n");
        return 1;
    }
    const float wmin[3] = {-2.0f, -2.0f, -2.0f};
    const float wmax[3] = {2.0f, 2.0f, 2.0f};
    clay_brick_cache_mark_dirty(cache, wmin, wmax);
    const long warm = drain(cache, d.doc);
    std::printf("  warm: %ld bricks\n", warm);
    std::printf("  baseline raycast (128x128): %.3f ms\n\n", raycast_ms(cache, 128));

    std::printf("  dab   begin    update    commit    refill   bricks |  warps  chain  "
                "lipschitz  step_scale   cache-ray   TAPE-RAY\n");

    int failures = 0;
    double first_raycast = 0.0, last_raycast = 0.0;
    for (int dab = 0; dab < kDabs; ++dab) {
        // DISTINCT anchors, a little apart, which is what a sculptor actually
        // does — and what the first version of this got wrong. Dabbing at one
        // bit-identical anchor makes continues_gesture COALESCE every dab into
        // the previous grab: the probe ran eight dabs, reported `warps 1` on
        // every row, and printed a beautifully flat table of a single warp
        // being rewritten. Flat because nothing accumulated, not because
        // accumulation is cheap. They still OVERLAP, which is the case where
        // Lipschitz factors legitimately compound.
        const float step = 0.06f * static_cast<float>(dab);
        const float anchor[3] = {step, 0.0f, std::sqrt(std::max(0.01f, 1.0f - step * step))};

        clay_move_params mp;
        std::memset(&mp, 0, sizeof mp);
        mp.struct_size = sizeof mp;
        mp.radius = kRadius;
        mp.ease = 0;
        mp.front_only = 0;

        const Clock::time_point tb = Clock::now();
        clay_sdf_move_tx* tx = clay_sdf_move_begin(d.doc, d.layer, anchor, &mp, nullptr);
        const double begin_ms = ms_since(tb);
        if (!tx) {
            std::printf("FAIL: begin returned NULL on dab %d\n", dab);
            return 1;
        }

        double update_ms = 0.0;
        clay_sculpt_dirty dirty;
        for (int seg = 0; seg < kSegments; ++seg) {
            const float f = static_cast<float>(seg + 1) / static_cast<float>(kSegments);
            const float total[3] = {0.0f, 0.10f * f, 0.18f * f};
            std::memset(&dirty, 0, sizeof dirty);
            dirty.struct_size = sizeof dirty;
            const Clock::time_point tu = Clock::now();
            const clay_result r = clay_sdf_move_update(tx, total, &dirty);
            update_ms += ms_since(tu);
            if (r != CLAY_OK) {
                std::printf("FAIL: update failed on dab %d segment %d\n", dab, seg);
                return 1;
            }
        }

        clay_sculpt_budget budget;
        std::memset(&budget, 0, sizeof budget);
        budget.struct_size = sizeof budget;
        const Clock::time_point tc = Clock::now();
        const clay_result cr = clay_sdf_move_commit(tx, &budget);
        const double commit_ms = ms_since(tc);
        clay_sdf_move_destroy(tx);
        if (cr != CLAY_OK) {
            std::printf("FAIL: commit failed on dab %d\n", dab);
            return 1;
        }

        // What a host pays next: refill what the dab dirtied.
        const Clock::time_point tr = Clock::now();
        if (dirty.has_bounds) clay_brick_cache_mark_dirty(cache, dirty.bounds_min, dirty.bounds_max);
        const long bricks = drain(cache, d.doc);
        const double refill_ms = ms_since(tr);
        if (bricks < 0) {
            std::printf("FAIL: refill failed on dab %d\n", dab);
            return 1;
        }

        clay_layer_warp_cost wc;
        std::memset(&wc, 0, sizeof wc);
        wc.struct_size = sizeof wc;
        clay_layer_warp_cost_get(d.doc, d.layer, &wc);

        clay_field_report fr;
        std::memset(&fr, 0, sizeof fr);
        fr.struct_size = sizeof fr;
        clay_layer_field_report(d.doc, d.layer, 0.5f, &fr);

        const double ray = raycast_ms(cache, 128);
        // 64x64 rather than 128x128: this one is expected to blow up, and a
        // probe that takes a minute per row is one nobody runs twice.
        const double tape_ray = tape_raycast_ms(d.doc, 64);
        if (dab == 0) first_raycast = tape_ray;
        last_raycast = tape_ray;

        std::printf("  %3d  %7.3f  %8.3f  %8.3f  %8.3f  %7ld | %6llu  %5d  %9.2f  %10.6f  %9.3f  %9.3f\n",
                    dab + 1, begin_ms, update_ms, commit_ms, refill_ms, bricks,
                    static_cast<unsigned long long>(wc.warps), fr.longest_deformer_chain,
                    static_cast<double>(fr.lipschitz), static_cast<double>(fr.safe_step_scale),
                    ray, tape_ray);
    }

    // -- the invariants -----------------------------------------------------
    //
    // This probe exists to find a per-dab degradation. If nothing degrades it
    // must say so loudly rather than print a flat table and let a reader infer
    // whatever they came in believing.
    std::printf("\n  TAPE raycast (what a host PICKS with): dab 1 %.3f ms -> dab %d %.3f ms  (%.2fx)\n", first_raycast, kDabs,
                last_raycast, first_raycast > 0.0 ? last_raycast / first_raycast : 0.0);
    if (first_raycast > 0.0 && last_raycast > first_raycast * 2.0)
        std::printf("  -> TAPE RAYCAST DEGRADES ACROSS DABS. The stall is not in the edit: it\n"
                    "     is in every PICK and every tape trace afterwards, and the\n"
                    "     safe_step_scale column is why. A host that picks per cursor move\n"
                    "     pays this on every mouse event until the layer is consolidated.\n");
    else
        std::printf("  -> raycast does NOT degrade materially across dabs. Whatever the user\n"
                    "     sees is not this, and on this evidence not in the engine at all.\n");

    // THE FIXTURE MUST ACTUALLY ACCUMULATE. A run where every dab coalesced
    // into the last measures one warp eight times and reports it as eight
    // cheap dabs — which is exactly what the first version of this did.
    clay_layer_warp_cost final_wc;
    std::memset(&final_wc, 0, sizeof final_wc);
    final_wc.struct_size = sizeof final_wc;
    clay_layer_warp_cost_get(d.doc, d.layer, &final_wc);
    std::printf("  warps after %d dabs: %llu\n", kDabs,
                static_cast<unsigned long long>(final_wc.warps));
    if (final_wc.warps < static_cast<std::uint64_t>(kDabs)) {
        std::printf("FAIL: %d dabs left only %llu warps, so they COALESCED and this table is\n"
                    "      one warp measured %d times. Move the anchors further apart.\n",
                    kDabs, static_cast<unsigned long long>(final_wc.warps), kDabs);
        ++failures;
    }

    if (warm <= 0) {
        std::printf("\nFAIL: the warm pass refilled nothing, so every number above is the cost\n"
                    "      of an empty cache and this probe measured nothing.\n");
        ++failures;
    }

    clay_brick_cache_destroy(cache);
    clay_document_destroy(d.doc);
    if (failures) return 1;
    std::printf("\nok\n");
    return 0;
}
