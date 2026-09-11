// WHERE, IF ANYWHERE, BAKING A BRUSH CHAIN STOPS BEING A LOSS.
// NOT a gated benchmark.
//
// THE CLAIM UNDER TEST is one this repository states in its own enum, and acts
// on. `clay.h`, CLAY_DEGRADATION_DEFORMERS:
//
//     A chain of brushes on a layer with nothing to absorb. Consolidation is
//     NOT the cure and measured 6x WORSE on a real gesture: it swaps a cheap
//     analytic item for a dense volume, and the marching win -- a 29x better
//     step scale -- is swamped by what the volume costs per sample. Leave the
//     layer parametric.
//
// `src/scene/consolidate.cpp:214` enforces it: `advises_consolidation =
// degraded && volumes`, so a single drawable carrying a brush chain is never
// advised however far its step scale has fallen.
//
// WHY IT IS WORTH RE-ASKING. "29x better step scale" names the regime the
// measurement was taken in. A live ClaySpaceDesktop session, driven through
// its own MCP, reaches far past that on four to eight ordinary Move dabs:
//
//     dab   chain   safe_step_scale        vs dab 1
//       1       2          0.412423           1.0x
//       4       8          0.028932            14x
//       8      16          0.000837           490x
//
// (Symmetry is the reason the chain grows by two a dab: the starting document
// has X on, and the engine emits one grab per mirror image.)
//
// 6x more per sample against 490x fewer steps is a different arithmetic from
// 6x against 29x. Nobody has measured where the two curves cross, and the
// advisory has no escape hatch for "degraded so badly that even a bad cure
// beats the disease". If a crossover exists, `advises_consolidation` needs a
// step-scale floor and the fix is a few lines. If it does not, the answer is a
// chain collapse that keeps the layer parametric, which is a research problem
// and needs justifying with exactly this table.
//
// THE DESIGN. One sphere. N dabs, each at a NEW centre (the same centre
// coalesces -- `continues_gesture` matches bit-exact centre and radius, and a
// commit in between makes no difference, which cost two people a run each).
// Then, at every N, the same work measured two ways:
//
//     PARAMETRIC   raycast and mesh the layer as it stands
//     BAKED        consolidate a COPY at the advised-style cell size, then
//                  raycast and mesh that
//
// Both arms answer the same question a host asks per frame, so the ratio is
// what a sculptor would feel. Reported per N so the crossover is visible
// rather than asserted.
//
// Exits non-zero if the fixture stops discriminating: if the chain does not
// grow with N, or if a bake silently fails and the "baked" arm is secretly the
// parametric one.

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

constexpr float kRadius = 0.40f;
constexpr int kRaySide = 64;   // 4096 rays; a viewport casts one per pixel
constexpr float kCell = 0.02f; // consolidation cell size

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

bool build_sphere(Doc* out) {
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

// One Move dab at a fresh place on the sphere, applied through the live
// transaction exactly as a host drives it.
bool dab(Doc* d, int index) {
    const float t = 0.35f * static_cast<float>(index);
    const float cx = std::cos(t) * 0.55f;
    const float cy = std::sin(t) * 0.55f;
    const float cz = std::sqrt(std::max(0.05f, 1.0f - cx * cx - cy * cy));
    const float anchor[3] = {cx, cy, cz};

    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = kRadius;
    mp.ease = 0;
    mp.front_only = 0;

    clay_sdf_move_tx* tx = clay_sdf_move_begin(d->doc, d->layer, anchor, &mp, nullptr);
    if (!tx) return false;
    const float total[3] = {cx * 0.12f, cy * 0.12f, cz * 0.12f};
    clay_sculpt_dirty dirty;
    std::memset(&dirty, 0, sizeof dirty);
    dirty.struct_size = sizeof dirty;
    bool ok = clay_sdf_move_update(tx, total, &dirty) == CLAY_OK;
    if (ok) {
        clay_sculpt_budget budget;
        std::memset(&budget, 0, sizeof budget);
        budget.struct_size = sizeof budget;
        ok = clay_sdf_move_commit(tx, &budget) == CLAY_OK;
    }
    clay_sdf_move_destroy(tx);
    return ok;
}

// WARMED, because the first version was not. Its n=1 row reported 13421 ms at a
// step scale of 0.727 -- a healthy field -- which is first-touch cost (tape
// compile, thread-pool spin-up) charged to whichever raycast ran first, and it
// produced a "crossover at 1 dab, 1765x" that was pure warm-up.
double raycast_ms(const clay_document* doc, int* out_hits) {
    {   // one untimed pass
        int32_t hit = 0;
        float t = 0.0f, pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
        const float o[3] = {0.0f, 0.0f, 4.0f}, dd[3] = {0.0f, 0.0f, -1.0f};
        for (int w = 0; w < 8; ++w) clay_raycast(doc, o, dd, &hit, &t, pos, nrm);
    }
    const Clock::time_point t0 = Clock::now();
    int hits = 0;
    for (int i = 0; i < kRaySide; ++i)
        for (int j = 0; j < kRaySide; ++j) {
            const float u =
                -1.3f + 2.6f * static_cast<float>(i) / static_cast<float>(kRaySide - 1);
            const float v =
                -1.3f + 2.6f * static_cast<float>(j) / static_cast<float>(kRaySide - 1);
            const float origin[3] = {u, v, 4.0f};
            const float dir[3] = {0.0f, 0.0f, -1.0f};
            int32_t hit = 0;
            float t = 0.0f, pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
            if (clay_raycast(doc, origin, dir, &hit, &t, pos, nrm) == CLAY_OK) hits += hit ? 1 : 0;
        }
    if (out_hits) *out_hits = hits;
    return ms_since(t0);
}

struct Row {
    int dabs = 0;
    int chain = 0;
    float step_parametric = 0.0f;
    float step_baked = 0.0f;
    double ray_parametric = 0.0;
    double ray_baked = 0.0;
    int hits_parametric = 0;
    int hits_baked = 0;
    double bake_ms = 0.0;
    bool baked = false;
};

}  // namespace

int main() {
    std::printf("move_collapse_crossover_probe: does baking a brush chain ever stop being a loss?\n"
                "  sphere r=1, drag radius %.2f, %d rays, consolidation cell %.3f\n\n",
                static_cast<double>(kRadius), kRaySide * kRaySide, static_cast<double>(kCell));

    const int sweep[] = {1, 2, 4, 6, 8, 12, 16};
    std::vector<Row> rows;
    int failures = 0;

    for (const int n : sweep) {
        Row row;
        row.dabs = n;

        // PARAMETRIC arm: a fresh document with n dabs on it.
        Doc p;
        if (!build_sphere(&p)) {
            std::printf("FAIL: could not build the sphere\n");
            return 1;
        }
        for (int i = 0; i < n; ++i)
            if (!dab(&p, i)) {
                std::printf("FAIL: dab %d failed at n=%d\n", i, n);
                return 1;
            }
        clay_field_report fr;
        std::memset(&fr, 0, sizeof fr);
        fr.struct_size = sizeof fr;
        clay_layer_field_report(p.doc, p.layer, 0.5f, &fr);
        row.chain = fr.longest_deformer_chain;
        row.step_parametric = fr.safe_step_scale;
        row.ray_parametric = raycast_ms(p.doc, &row.hits_parametric);

        // BAKED arm: the same document, consolidated.
        Doc b;
        if (!build_sphere(&b)) {
            std::printf("FAIL: could not build the baked-arm sphere\n");
            return 1;
        }
        for (int i = 0; i < n; ++i) dab(&b, i);
        clay_consolidation_params cp;
        std::memset(&cp, 0, sizeof cp);
        cp.struct_size = sizeof cp;
        cp.cell_size = kCell;
        cp.band = 0.0f;
        cp.padding = 0.0f;
        cp.skip_redistance = 0;  // redistancing is what bounds the Lipschitz
        clay_consolidation_cost cost;
        std::memset(&cost, 0, sizeof cost);
        cost.struct_size = sizeof cost;
        const Clock::time_point tb = Clock::now();
        const clay_result cr =
            clay_layer_consolidate(b.doc, b.layer, &cp, nullptr, nullptr, &cost);
        row.bake_ms = ms_since(tb);
        row.baked = (cr == CLAY_OK);
        if (row.baked) {
            clay_field_report fb;
            std::memset(&fb, 0, sizeof fb);
            fb.struct_size = sizeof fb;
            clay_layer_field_report(b.doc, b.layer, 0.5f, &fb);
            row.step_baked = fb.safe_step_scale;
            row.ray_baked = raycast_ms(b.doc, &row.hits_baked);
        }

        clay_document_destroy(p.doc);
        clay_document_destroy(b.doc);
        rows.push_back(row);
        std::printf("  n=%-3d chain=%-3d done\n", n, row.chain);
    }

    std::printf("\n  dabs chain | step(param)  step(baked) | ray(param)  ray(baked)   ratio | "
                "bake ms | hits p/b\n");
    for (const Row& r : rows) {
        std::printf("  %4d %5d | %11.6f  %11.6f | %9.2f  %10.2f  %6.2fx | %7.1f | %d/%d%s\n",
                    r.dabs, r.chain, static_cast<double>(r.step_parametric),
                    static_cast<double>(r.step_baked), r.ray_parametric, r.ray_baked,
                    r.ray_baked > 0.0 ? r.ray_parametric / r.ray_baked : 0.0, r.bake_ms,
                    r.hits_parametric, r.hits_baked, r.baked ? "" : "  <- BAKE FAILED");
    }

    // -- the invariants -----------------------------------------------------
    if (rows.size() >= 2 && rows.back().chain <= rows.front().chain) {
        std::printf("\nFAIL: the chain did not grow with the dab count (%d -> %d). The dabs\n"
                    "      COALESCED, so every row is the same layer and this table means\n"
                    "      nothing. Move the dab centres further apart.\n",
                    rows.front().chain, rows.back().chain);
        ++failures;
    }
    for (const Row& r : rows) {
        if (!r.baked) {
            std::printf("\nFAIL: the bake failed at n=%d, so its 'baked' row is the parametric\n"
                        "      layer under another name.\n", r.dabs);
            ++failures;
            break;
        }
        // A BAKED arm that misses is a broken fixture. A PARAMETRIC arm that
        // misses is a finding: at a low enough step scale the sphere trace
        // exhausts its iteration budget before reaching the surface, so the
        // render does not merely get slow, it stops finding the form. Reported
        // rather than failed -- but its TIMING is then the cost of missing and
        // must not be quoted as a march.
        if (r.hits_baked == 0) {
            std::printf("\nFAIL: at n=%d the BAKED arm hit nothing, so the fixture is wrong.\n",
                        r.dabs);
            ++failures;
            break;
        }
    }

    // The verdict, stated as the question it was written to answer.
    for (const Row& r : rows)
        if (r.hits_parametric == 0)
            std::printf("\n  NOTE: at %d dabs (step scale %.6f) the parametric arm's rays found\n"
                        "  NO SURFACE. The march runs out of iterations before reaching it, so\n"
                        "  the field is not just dear to render, it renders WRONG. That row's\n"
                        "  time is the cost of missing and is excluded from the crossover.\n",
                        r.dabs, static_cast<double>(r.step_parametric));

    const Row* crossover = nullptr;
    for (const Row& r : rows)
        if (r.baked && r.hits_parametric > 0 && r.ray_baked > 0.0 &&
            r.ray_parametric > r.ray_baked) {
            crossover = &r;
            break;
        }
    if (crossover)
        std::printf("\n  -> A CROSSOVER EXISTS at %d dabs (chain %d, step scale %.6f): marching\n"
                    "     the baked volume is %.2fx cheaper than marching the chain. Past this\n"
                    "     point 'consolidation is not the cure' stops being true, and\n"
                    "     advises_consolidation needs a step-scale floor.\n",
                    crossover->dabs, crossover->chain,
                    static_cast<double>(crossover->step_parametric),
                    crossover->ray_parametric / crossover->ray_baked);
    else
        std::printf("\n  -> NO CROSSOVER in this sweep. The enum's claim holds at every depth\n"
                    "     measured, advises_consolidation is right to withhold, and the only\n"
                    "     remaining cure is a collapse that keeps the layer PARAMETRIC.\n");

    // -- IS 0.577 A LAW, OR THIS SPHERE'S NUMBER? ---------------------------
    //
    // Every baked row above reports the same step scale, so the crossover looks
    // like a property of consolidation. ClaySpaceDesktop pointed out that it is
    // not: 0.577 is what a REDISTANCED SPHERE AT CELL 0.02 produces, and a
    // denser form or a finer cell may bake to something lower — in which case
    // the crossover moves and a fixed floor does not follow it. The floor is
    // only meaningful if what the bake produces is stable, so that is measured
    // here rather than assumed.
    std::printf("\n  BAKE QUALITY vs CELL SIZE, at 8 dabs (does the baked step scale hold?)\n");
    std::printf("  cell     step(baked)   ray(baked)    bake ms       bytes\n");
    float baked_lo = 1e9f, baked_hi = 0.0f;
    for (const float cell : {0.04f, 0.02f, 0.01f, 0.005f}) {
        Doc b;
        if (!build_sphere(&b)) break;
        for (int i = 0; i < 8; ++i) dab(&b, i);
        clay_consolidation_params cp;
        std::memset(&cp, 0, sizeof cp);
        cp.struct_size = sizeof cp;
        cp.cell_size = cell;
        cp.skip_redistance = 0;
        clay_consolidation_cost cost;
        std::memset(&cost, 0, sizeof cost);
        cost.struct_size = sizeof cost;
        const Clock::time_point tb = Clock::now();
        const clay_result cr = clay_layer_consolidate(b.doc, b.layer, &cp, nullptr, nullptr, &cost);
        const double bake_ms = ms_since(tb);
        if (cr != CLAY_OK) {
            std::printf("  %.4f   BAKE FAILED\n", static_cast<double>(cell));
            clay_document_destroy(b.doc);
            continue;
        }
        clay_field_report fb;
        std::memset(&fb, 0, sizeof fb);
        fb.struct_size = sizeof fb;
        clay_layer_field_report(b.doc, b.layer, 0.5f, &fb);
        int h = 0;
        const double ray = raycast_ms(b.doc, &h);
        baked_lo = std::min(baked_lo, fb.safe_step_scale);
        baked_hi = std::max(baked_hi, fb.safe_step_scale);
        std::printf("  %.4f   %11.6f  %10.2f  %9.1f  %10llu\n", static_cast<double>(cell),
                    static_cast<double>(fb.safe_step_scale), ray, bake_ms,
                    static_cast<unsigned long long>(cost.bytes));
        clay_document_destroy(b.doc);
    }
    if (baked_hi > 0.0f) {
        const float spread = baked_hi / std::max(baked_lo, 1e-9f);
        std::printf("\n  baked step scale across cell sizes: %.6f .. %.6f  (%.2fx spread)\n",
                    static_cast<double>(baked_lo), static_cast<double>(baked_hi),
                    static_cast<double>(spread));
        if (spread > 1.5f)
            std::printf("  -> THE BAKE'S OWN STEP SCALE MOVES WITH THE CELL. The crossover is a\n"
                        "     function of what the bake produces, so a FIXED floor cannot be\n"
                        "     right everywhere and the proposal needs the floor derived from\n"
                        "     the projected bake rather than from a constant.\n");
        else
            std::printf("  -> the baked step scale is stable across cell sizes here, so a fixed\n"
                        "     floor is defensible on this fixture. Still one fixture.\n");
    }

    if (failures) {
        std::printf("\n%d invariant(s) failed; do not quote the table above.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
