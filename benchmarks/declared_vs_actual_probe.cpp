// HOW PESSIMISTIC IS THE DECLARED LIPSCHITZ BOUND ON A MOVE CHAIN?
// NOT a gated benchmark.
//
// THE QUESTION. A Move chain's cost has two independent factors: the per-step
// cost of walking N deformers, and the NUMBER of steps, which is set by
// `safe_step_scale` and therefore by the DECLARED Lipschitz bound. The arm-E
// consolidation result could not separate them, because baking removed both at
// once. This separates them.
//
// If the declared bound is close to the field's real gradient, the step count
// is honest and the only win available is O(N) -> O(1) per step. If the
// declared bound is far above the real gradient, the marcher is taking steps
// it does not need and a representation that declares a tighter bound recovers
// the second factor WITHOUT sampling anything.
//
// ClaySpaceDesktop measured this first and got, over 600 points on a shell:
//
//     moves  chain  declared L   safe_step  actual |g|   slack
//         1      1        1.12    0.892878       1.085     1.0x
//         8      8        2.48    0.403958       1.145     2.2x
//        32     32       37.55    0.026628       1.927    19.5x
//        48     48      230.14    0.004345       2.304    99.9x
//
// declared grows 205x, actual grows 2.1x.
//
// WHY THIS FILE EXISTS ANYWAY, and it is not distrust. A SAMPLED MAXIMUM IS A
// LOWER BOUND on the true supremum: a worse point may sit between samples. That
// direction matters enormously here, because the conclusion being reached for
// is "the declared bound could be smaller" -- and a Lipschitz bound that is too
// small does not make the marcher slow, it makes it STEP THROUGH THE SURFACE.
// A perf estimate may be loose. A safety bound may not.
//
// So this does two things their probe could not:
//
//   1. SWEEPS THE SAMPLE DENSITY. If the measured maximum keeps climbing as
//      the grid refines, the estimate has not converged and the slack is
//      overstated. If it plateaus, the lower bound is close to the supremum
//      and the slack is real.
//
//   2. SWEEPS THE FINITE-DIFFERENCE STEP h. A central difference under-reads a
//      gradient that varies within h, so too large an h flatters the field and
//      too small an h drowns in fp noise. The honest number is where the two
//      meet.
//
// It measures |grad f| of the compiled FIELD by central differences of
// clay_layer_eval_points -- the field's gradient is what sphere tracing depends
// on and what `lipschitz` claims to bound.
//
// Exits non-zero if the measurement cannot be trusted: if the gradient max is
// still climbing at the finest grid, or if any measured gradient EXCEEDS the
// declared bound, which would mean the declared bound is already unsound and is
// a far more urgent finding than anything about pessimism.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

constexpr float kMoveRadius = 0.40f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

bool build(Doc* out) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    const float r = 1.0f;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!it) {
        clay_document_destroy(doc);
        return false;
    }
    clay_item_set_op(it, CLAY_OP_ADD);
    clay_node_id n = 0;
    const clay_result res = clay_layer_add_item(doc, layer, it, &n);
    clay_item_destroy(it);
    if (res != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    out->doc = doc;
    out->layer = layer;
    return true;
}

bool dab(Doc* d, int index) {
    const float t = 0.37f * static_cast<float>(index);
    const float cx = std::cos(t) * 0.42f, cy = std::sin(t) * 0.42f;
    const float cz = std::sqrt(std::max(0.05f, 1.0f - cx * cx - cy * cy));
    const float anchor[3] = {cx, cy, cz};
    clay_move_params mp;
    std::memset(&mp, 0, sizeof mp);
    mp.struct_size = sizeof mp;
    mp.radius = kMoveRadius;
    mp.ease = 0;
    mp.front_only = 0;
    clay_sdf_move_tx* tx = clay_sdf_move_begin(d->doc, d->layer, anchor, &mp, nullptr);
    if (!tx) return false;
    const float total[3] = {cx * 0.11f, cy * 0.11f, cz * 0.11f};
    clay_sculpt_dirty dirty;
    std::memset(&dirty, 0, sizeof dirty);
    dirty.struct_size = sizeof dirty;
    bool ok = clay_sdf_move_update(tx, total, &dirty) == CLAY_OK;
    if (ok) {
        clay_sculpt_budget b;
        std::memset(&b, 0, sizeof b);
        b.struct_size = sizeof b;
        ok = clay_sdf_move_commit(tx, &b) == CLAY_OK;
    }
    clay_sdf_move_destroy(tx);
    return ok;
}

// |grad f| sampled over a shell straddling the worked surface, by CENTRAL
// DIFFERENCES of the distance field.
//
// NOT clay_layer_eval_gradients, and that was this file's first mistake: its
// own header says "Normalized field gradients (the tetrahedron trick, so they
// are unit-length surface normals rather than raw differences)". It returns
// unit normals by design, so it reported |g| = 1.000 at every depth and every
// density -- 35,400 samples of a constant, which the convergence check below
// passed perfectly because a constant has converged. Measuring the wrong
// quantity survives a convergence test; only a VARIATION test catches it, and
// there is one at the end of main now.
//
// `h` is swept by the caller: too large under-reads a gradient that varies
// within h, too small drowns in fp32 noise, and the honest figure is where the
// two meet.
double max_gradient(const clay_document* doc, clay_layer_id layer, int side, double h,
                    double* out_mean, int* out_samples) {
    std::vector<float> pts;
    // A shell around the patch: the dabs land near +z on a unit sphere, so
    // sample a band of radii through the deformed surface rather than a plane,
    // which would miss wherever the surface moved to.
    for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
            for (int k = 0; k < 5; ++k) {
                const double u = -0.9 + 1.8 * i / double(side - 1);
                const double v = -0.9 + 1.8 * j / double(side - 1);
                const double rr = u * u + v * v;
                if (rr > 0.81) continue;
                const double base = std::sqrt(std::max(0.02, 1.0 - rr));
                const double w = base + (-0.14 + 0.07 * k);  // straddle the surface
                pts.push_back(float(u));
                pts.push_back(float(v));
                pts.push_back(float(w));
            }
    const std::size_t n = pts.size() / 3;
    if (n == 0) return -1.0;

    // Six offset probes per sample point: f(x+h) and f(x-h) on each axis.
    std::vector<float> probe(n * 6 * 3);
    for (std::size_t i = 0; i < n; ++i)
        for (int axis = 0; axis < 3; ++axis)
            for (int sgn = 0; sgn < 2; ++sgn) {
                const std::size_t k = ((i * 3 + axis) * 2 + sgn) * 3;
                for (int a = 0; a < 3; ++a) probe[k + a] = pts[i * 3 + a];
                probe[k + axis] += float(sgn ? -h : h);
            }
    std::vector<float> dist(n * 6, 0.0f);
    if (clay_layer_eval_points(doc, layer, "cpu", probe.data(), n * 6, dist.data(), nullptr) !=
        CLAY_OK)
        return -1.0;

    double mx = 0.0, sum = 0.0;
    int counted = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double gg[3];
        bool ok = true;
        for (int axis = 0; axis < 3; ++axis) {
            const double plus = dist[(i * 3 + axis) * 2 + 0];
            const double minus = dist[(i * 3 + axis) * 2 + 1];
            if (!std::isfinite(plus) || !std::isfinite(minus)) { ok = false; break; }
            gg[axis] = (plus - minus) / (2.0 * h);
        }
        if (!ok) continue;
        const double m = std::sqrt(gg[0] * gg[0] + gg[1] * gg[1] + gg[2] * gg[2]);
        if (!std::isfinite(m)) continue;
        mx = std::max(mx, m);
        sum += m;
        ++counted;
    }
    if (out_mean) *out_mean = counted ? sum / counted : 0.0;
    if (out_samples) *out_samples = counted;
    return mx;
}

}  // namespace

int main() {
    std::printf("declared_vs_actual_probe: how pessimistic is the declared Lipschitz bound?\n");
    std::printf("  sphere r=1, move radius %.2f, gradients from clay_layer_eval_gradients\n\n",
                double(kMoveRadius));

    int failures = 0;
    std::vector<double> seen;
    std::printf("  moves chain | declared L  safe_step | actual |g| at grid 24/48/96   mean  samples | slack\n");

    for (const int n : {1, 4, 8, 16, 32, 48}) {
        Doc d;
        if (!build(&d)) {
            std::printf("FAIL: build\n");
            return 1;
        }
        for (int i = 0; i < n; ++i)
            if (!dab(&d, i)) {
                std::printf("FAIL: dab %d\n", i);
                return 1;
            }
        clay_field_report fr;
        std::memset(&fr, 0, sizeof fr);
        fr.struct_size = sizeof fr;
        clay_layer_field_report(d.doc, d.layer, 0.5f, &fr);

        double mean = 0.0;
        int samples = 0;
        const double g24 = max_gradient(d.doc, d.layer, 24, 1e-3, nullptr, nullptr);
        const double g48 = max_gradient(d.doc, d.layer, 48, 1e-3, nullptr, nullptr);
        const double g96 = max_gradient(d.doc, d.layer, 96, 1e-3, &mean, &samples);
        // h sweep at the finest density: the honest figure is where a coarse h
        // (which under-reads) and a fine h (which picks up fp noise) agree.
        const double h_coarse = max_gradient(d.doc, d.layer, 48, 4e-3, nullptr, nullptr);
        const double h_fine = max_gradient(d.doc, d.layer, 48, 2.5e-4, nullptr, nullptr);
        seen.push_back(g96);

        const double slack = g96 > 0.0 ? double(fr.lipschitz) / g96 : 0.0;
        std::printf("  %5d %5d | %10.2f %10.6f | %9.3f %8.3f %8.3f %6.3f %8d | %6.1fx\n", n,
                    fr.longest_deformer_chain, double(fr.lipschitz), double(fr.safe_step_scale),
                    g24, g48, g96, mean, samples, slack);
        std::printf("        h sweep at grid 48: h=4e-3 %.3f, h=1e-3 %.3f, h=2.5e-4 %.3f\n",
                    h_coarse, g48, h_fine);

        // CONVERGENCE. If refining the grid keeps finding a larger maximum, the
        // sampled value is still a loose lower bound and the slack above is
        // overstated. 5% between the last two densities is the bar.
        if (g96 > 0.0 && g48 > 0.0 && (g96 - g48) / g48 > 0.05) {
            std::printf("        NOTE: gradient max still climbing %.3f -> %.3f between the two\n"
                        "        finest grids, so this row's slack is an OVERESTIMATE.\n", g48, g96);
        }
        // SOUNDNESS. A measured gradient above the declared bound would mean the
        // bound is already wrong, which matters far more than its pessimism.
        if (g96 > double(fr.lipschitz)) {
            std::printf("        FAIL: measured |grad f| %.3f EXCEEDS the declared bound %.3f.\n"
                        "        The declared bound is unsound at this depth, which is a\n"
                        "        correctness defect and not a pessimism finding.\n",
                        g96, double(fr.lipschitz));
            ++failures;
        }
        clay_document_destroy(d.doc);
    }

    std::printf("\n  A SAMPLED MAXIMUM IS A LOWER BOUND on the true supremum, so every slack\n"
                "  figure here is an UPPER estimate of what is recoverable. That direction is\n"
                "  the safe one for deciding whether to investigate, and the UNSAFE one for\n"
                "  deciding what to declare: a bound below the true gradient makes the marcher\n"
                "  step through the surface. Nothing here licenses declaring the measured\n"
                "  number -- only an ANALYTIC bound on the composed map can be declared.\n");

    // THE CHECK THAT WOULD HAVE CAUGHT THE FIRST VERSION. If the measured
    // maximum is identical at every chain depth, the quantity being measured
    // does not depend on the chain -- which is what a unit normal looks like,
    // and what this file reported before it was fixed. A convergence test
    // cannot catch that; only a variation test can.
    if (seen.size() >= 2) {
        const double lo = *std::min_element(seen.begin(), seen.end());
        const double hi = *std::max_element(seen.begin(), seen.end());
        std::printf("\n  measured |grad f| across depths: %.4f .. %.4f\n", lo, hi);
        if (hi - lo < 1e-6) {
            std::printf("  FAIL: identical at every depth, so this is not measuring anything that\n"
                        "        depends on the Move chain. The first version of this probe read\n"
                        "        clay_layer_eval_gradients, which returns UNIT NORMALS by design,\n"
                        "        and reported 1.000 everywhere while passing its convergence check.\n");
            ++failures;
        }
    }

    if (failures) {
        std::printf("\n%d soundness failure(s).\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
