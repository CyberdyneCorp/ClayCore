// WHAT gesture_id DOES TO A DRAG THE HOST HAS ALREADY CHOPPED UP.
// NOT a gated benchmark.
//
// THE CASE. ClaySpaceDesktop does not animate a Move parameter. Its radius is
// the brush's, constant for the gesture, and its ease and front_only are
// literals. By the taxonomy in #533 that is the ANCHORED case, which should
// already coalesce.
//
// It does not, and the reason is above the call rather than in it. The
// application hands the engine `pending()` slices starting at `applied - 1`, so
// `move_surface` is re-entered once per slice with the centre taken from
// `samples[applied - 1]` -- the CENTRE ADVANCES, and the displacement is
// re-derived per slice rather than accumulated. Six slices, six grabs. That is
// what drives their safe_step_scale collapse: 0.412423 at one dab, 0.000837 at
// eight, and past sixteen the form stops rendering at 33 ray hits of 512.
//
// THE QUESTION THIS ANSWERS is whether naming the gesture is enough on its own,
// because that is the one-line change and it is the one a reader of #533 would
// make first. It is NOT enough, and the way it fails is silent.
//
// `moved_chain` REPLACES every leading grab that continues the gesture rather
// than stacking on it, and `resolve_prepared_move` takes a TOTAL displacement
// from the anchor. Those two together are the contract: a continued gesture
// restates where the drag has reached, it does not add another step to it. A
// host that names the gesture while still passing per-slice displacements is
// telling the engine to replace the whole drag with its last slice.
//
// THREE ARMS, one drag of six slices, identical inputs otherwise:
//
//   A  id 0, advancing centre, per-slice displacement   what they run today
//   B  id 1, advancing centre, per-slice displacement   the one-line change
//   C  id 1, anchored centre,  cumulative displacement  the contract
//
// Reported per arm: the warp count and chain the layer ends with, the declared
// bound and step scale that follow from it, and HOW FAR THE SURFACE ACTUALLY
// MOVED -- because the last of those is what separates a speedup from a
// silently different sculpt, and the first two cannot tell them apart.
//
// Exits non-zero if the fixture stops discriminating: if arm A does not
// accumulate a chain, or if B and C end up indistinguishable, either of which
// would mean this is measuring nothing.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

constexpr int kSlices = 6;
constexpr float kRadius = 0.35f;
// The drag: a straight pull along +y, sampled into kSlices pieces.
constexpr float kTotalY = 0.30f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

bool build(Doc* d) {
    d->doc = clay_document_create();
    if (!d->doc) return false;
    if (clay_add_sdf_layer(d->doc, "form", &d->layer) != CLAY_OK) return false;
    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!item) return false;
    clay_item_set_op(item, CLAY_OP_ADD);
    clay_node_id node = 0;
    const clay_result res = clay_layer_add_item(d->doc, d->layer, item, &node);
    clay_item_destroy(item);
    return res == CLAY_OK;
}

// Where the surface sits along +y, found by bisection on the field. This is the
// measurement that separates arm B from arm C: the chain can look perfect while
// the sculpt is wrong.
float surface_y(const Doc& d) {
    float lo = 0.5f, hi = 2.5f;
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float p[3] = {0.0f, mid, 0.0f};
        float dist = 0.0f;
        if (clay_layer_eval_points(d.doc, d.layer, "cpu", p, 1, &dist, nullptr) != CLAY_OK)
            return -1.0f;
        if (dist > 0.0f) hi = mid; else lo = mid;
    }
    return 0.5f * (lo + hi);
}

struct Arm {
    const char* name;
    unsigned long long gesture_id;
    bool advancing_centre;   // their segmentation, vs a fixed anchor
    bool per_slice;          // per-slice displacement, vs cumulative from the anchor
};

bool run_arm(const Arm& arm, double* out_y, unsigned long long* out_warps, int* out_chain,
             double* out_lip, double* out_step) {
    Doc d;
    if (!build(&d)) return false;

    for (int s = 0; s < kSlices; ++s) {
        const float done = static_cast<float>(s) / static_cast<float>(kSlices);
        const float next = static_cast<float>(s + 1) / static_cast<float>(kSlices);
        // The anchor is the top of the sphere; a slice's centre is where the
        // previous slice left the cursor, which is what samples[applied-1] is.
        const float centre[3] = {0.0f, arm.advancing_centre ? 1.0f + kTotalY * done : 1.0f, 0.0f};
        const float dy = arm.per_slice ? kTotalY * (next - done) : kTotalY * next;
        const float disp[3] = {0.0f, dy, 0.0f};

        clay_move_params mp;
        std::memset(&mp, 0, sizeof mp);
        mp.struct_size = sizeof mp;
        mp.radius = kRadius;
        mp.ease = 0;
        mp.front_only = 1;
        mp.gesture_id = arm.gesture_id;

        size_t applied = 0;
        if (clay_layer_move_surface(d.doc, d.layer, centre, disp, &mp, &applied) != CLAY_OK) {
            std::printf("FAIL: move_surface failed in arm %s slice %d\n", arm.name, s);
            return false;
        }
    }

    clay_layer_warp_cost wc;
    std::memset(&wc, 0, sizeof wc);
    wc.struct_size = sizeof wc;
    clay_layer_warp_cost_get(d.doc, d.layer, &wc);

    clay_field_report fr;
    std::memset(&fr, 0, sizeof fr);
    fr.struct_size = sizeof fr;
    clay_layer_field_report(d.doc, d.layer, 0.5f, &fr);

    *out_y = static_cast<double>(surface_y(d));
    *out_warps = wc.warps;
    *out_chain = fr.longest_deformer_chain;
    *out_lip = static_cast<double>(fr.lipschitz);
    *out_step = static_cast<double>(fr.safe_step_scale);
    clay_document_destroy(d.doc);
    return true;
}

}  // namespace

int main() {
    std::printf("A drag of %d slices, pulled +%.2f in y, radius %.2f.\n", kSlices,
                static_cast<double>(kTotalY), static_cast<double>(kRadius));
    std::printf("An undragged sphere's surface sits at y = 1.000; a drag that lands\n");
    std::printf("everything it was given reaches about %.3f.\n\n",
                static_cast<double>(1.0f + kTotalY));
    std::printf("  arm                                        warps  chain   lipschitz  step_scale"
                "   surface_y\n");

    const Arm arms[3] = {
        {"A  id 0, advancing centre, per-slice", 0, true, true},
        {"B  id 1, advancing centre, per-slice", 1, true, true},
        {"C  id 1, anchored centre, cumulative", 1, false, false},
    };
    double y[3] = {0, 0, 0}, lip[3] = {0, 0, 0}, step[3] = {0, 0, 0};
    unsigned long long warps[3] = {0, 0, 0};
    int chain[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!run_arm(arms[i], &y[i], &warps[i], &chain[i], &lip[i], &step[i])) return 1;
        std::printf("  %-40s %6llu  %5d  %10.4f  %10.6f  %10.4f\n", arms[i].name, warps[i],
                    chain[i], lip[i], step[i], y[i]);
    }

    std::printf("\n");
    std::printf("  chain A -> C   %d -> %d\n", chain[0], chain[2]);
    if (step[0] > 0.0) std::printf("  step scale     %.6f -> %.6f  (%.2fx)\n", step[0], step[2],
                                   step[2] / step[0]);
    std::printf("  surface        A %.4f   B %.4f   C %.4f\n", y[0], y[1], y[2]);
    std::printf("  B lost         %.4f of the %.4f it was asked to pull\n",
                y[0] - y[1], y[0] - 1.0);

    // -- the invariants -----------------------------------------------------
    if (chain[0] <= 1) {
        std::printf("\nFAIL: arm A did not accumulate a chain, so this measures nothing.\n");
        return 1;
    }
    if (std::fabs(y[1] - y[2]) < 1e-3) {
        std::printf("\nFAIL: arms B and C agree, so the contract this probe exists to\n"
                    "      demonstrate is not being exercised.\n");
        return 1;
    }
    std::printf("\nNaming the gesture is necessary and NOT sufficient: arm B keeps the chain\n"
                "short and loses the drag, because a continued gesture RESTATES the total\n"
                "rather than adding to it. A host segmenting its stroke has to anchor the\n"
                "centre and accumulate the displacement as well -- arm C.\n");
    return 0;
}
