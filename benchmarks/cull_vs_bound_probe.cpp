// DOES A PER-BRICK TAPE DECLARE A BOUND FOR DEFORMERS IT DID NOT EMIT?
// NOT a gated benchmark. Probe for issue #541.
//
// THE CLAIM UNDER TEST. `Compiler::cull_deformers` (src/scene/tape_build.cpp)
// drops every grab whose finite support cannot reach the cull region -- issue
// #452 -- so a small brick's tape carries only the few grabs that touch it.
// `Compiler::fold_info` is a SEPARATE call that folds
// `deformer_lipschitz(item)`, which walks `item.deformers`: the WHOLE chain,
// including the grabs the cull just dropped.
//
// If that is what happens, a brick keeping 3 of 48 grabs still declares the
// product of all 48 -- about 1.34^48 against 1.34^3 -- and the marcher pays
// ~10^5 steps it does not need. #541 measured exactly that gap by a different
// route (declared 1,443,068 against a field gradient of 4.958).
//
// WHAT WOULD REFUTE IT. If the survivors come back at or near 48, the cull is
// not culling -- its region is dilated by each kept grab's whole displacement,
// so deep in a chain the test box may have grown to swallow everything. Then
// the bound is honest about the tape and the pad is the thing to fix. That
// outcome is the reason this probe exists rather than a patch.
//
// The fixture uses ease_linear because that is what the Move brush actually
// sets (include/clay/brush/move.h: `std::uint8_t ease = 0`).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "clay/kernel/ease.h"
#include "clay/kernel/field.h"
#include "clay/kernel/tape.h"
#include "clay/mesh/marching.h"
#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;

namespace {

constexpr float kGrabRadius = 0.40f;
constexpr float kDrag = 0.05f;

// Dabs walking the +z cap of a unit sphere, the way a stroke does.
scene::Document fixture(int moves) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node n;
    n.prim = scene::Prim::sphere(1.0f);
    for (int i = 0; i < moves; ++i) {
        const float a = 0.37f * static_cast<float>(i);
        const kernel::cfloat3 c = cf3(std::sin(a) * 0.6f, std::cos(a) * 0.6f, 0.8f);
        n.deformers.push_back(
            scene::Deformer::grab(c, kGrabRadius, cf3(0.0f, 0.0f, kDrag), kernel::ease_linear,
                                  false));
    }
    l.sdf->insert(std::move(n));
    return doc;
}

// How many grabs the emitted tape actually carries. Read back out of the param
// block rather than asked of the builder, because what this probe is about is
// exactly the gap between what the builder EMITS and what it DECLARES -- so
// the emitted side has to come from the bytes.
//
// Layout (kernel/tape.h): [prim header 17][prim params 7][repeat 7][count].
int emitted_deformers(const scene::Tape& t) {
    const std::size_t kCountAt =
        CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS + CLAY_TAPE_REPEAT_FLOATS;
    for (const kernel::CTapeInstr& in : t.instrs) {
        if (in.op != kernel::ctape_sphere) continue;
        const std::size_t at = in.param_offset + kCountAt;
        if (at >= t.params.size()) return -1;  // layout drifted; say so, do not guess
        return static_cast<int>(t.params[at]);
    }
    return -1;  // the cull dropped the prim entirely
}

}  // namespace

int main() {
    std::printf("cull vs bound: does the tape declare grabs it did not emit?\n");
    std::printf("  sphere r=1, grab radius %.2f, drag %.3f, ease_linear (Move's default)\n\n",
                static_cast<double>(kGrabRadius), static_cast<double>(kDrag));

    std::printf("  %6s %10s %10s %14s %14s\n", "moves", "chain", "emitted", "declared L",
                "safe_step");
    for (int moves : {1, 4, 8, 16, 32, 48}) {
        const scene::Document doc = fixture(moves);

        // One brick, voxel-sized, sitting on the cap where the dabs land.
        const math::Aabb brick{cf3(0.55f, 0.55f, 0.75f), cf3(0.60f, 0.60f, 0.80f)};
        const scene::CullRegion cull{brick};
        const scene::Tape culled = scene::compile_document(doc, &cull);

        std::printf("  %6d %10d %10d %14.2f %14.8f\n", moves, moves, emitted_deformers(culled),
                    static_cast<double>(culled.info.lipschitz),
                    static_cast<double>(culled.safe_step_scale()));
    }

    // What the bound COSTS. The step scale sets how far a SPHERE TRACE may
    // step, so the honest end of this probe is a timed march.
    //
    // MESHING WAS TRIED HERE FIRST AND MEASURES NOTHING -- recorded so it is
    // not tried again. `mesh_tape` evaluates a DENSE GRID; it never marches,
    // so the step scale cannot reach it. A/B over this same fixture: 7.666 ms
    // against 7.213 ms at 48 dabs while safe_step moved 0.00394 -> 0.17089, a
    // 43x change in the bound and none at all in the time.
    //
    // A raycast is the path the bound actually drives, and it is what a host
    // spends on every pick and every cursor move.
    std::printf("\n  %6s %10s %14s %14s %12s %8s %7s %7s\n", "moves", "emitted", "declared L",
                "safe_step", "ray ms", "steps", "hits", "truth");
    for (int moves : {1, 4, 8, 16, 32, 48}) {
        const scene::Document doc = fixture(moves);
        const math::Aabb brick{cf3(0.40f, 0.40f, 0.60f), cf3(0.75f, 0.75f, 0.95f)};
        const scene::CullRegion cull{brick};
        const scene::Tape culled = scene::compile_document(doc, &cull);

        // A 64x64 fan aimed into the brick, marched exactly the way
        // cpu_backend's trace_one marches: the tape's own step scale, the
        // same 1.4 relaxation, the same step cap.
        const float step_scale = culled.safe_step_scale();
        auto field = [&](kernel::cfloat3 p) { return culled.eval(p).d; };
        int hits = 0;
        long long steps = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int iy = 0; iy < 64; ++iy) {
            for (int ix = 0; ix < 64; ++ix) {
                const float u = static_cast<float>(ix) / 63.0f;
                const float v = static_cast<float>(iy) / 63.0f;
                const kernel::cfloat3 ro =
                    cf3(0.40f + 0.35f * u, 0.40f + 0.35f * v, 2.0f);
                const kernel::cfloat3 rd = cf3(0.0f, 0.0f, -1.0f);
                const kernel::CRayHit r =
                    kernel::craycast(field, ro, rd, 0.0f, 4.0f, 1e-4f, step_scale, 1.4f, 4096);
                if (r.hit) ++hits;
                steps += r.steps;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Rays that all MISS take one step each and time nothing, which would
        // read as a win. The fan is aimed at the cap, so it must hit.
        if (hits == 0) {
            std::printf("  %6d %10d %14s %14s %12s   NO HITS -- measures nothing\n", moves,
                        emitted_deformers(culled), "-", "-", "-");
            continue;
        }
        // GROUND TRUTH for the hit count, and NOT through craycast.
        //
        // A first attempt marched craycast at a deliberately tiny step scale
        // and reported 4096 hits -- every ray, including corner rays 1.06 from
        // the axis that cannot touch a unit sphere. That is craycast's own
        // acceptance test, not the geometry: it computes
        //
        //     h = f(p) * step_scale;  accept when |h| < eps * t
        //
        // so step_scale scales the DISTANCE THE HIT TEST SEES. Shrink it and
        // the marcher accepts almost anywhere. Which is also what the degraded
        // bound does in production, and is why the `before` column reports
        // MORE hits than the truth rather than fewer -- a step scale of
        // 0.00394 manufactures hits.
        //
        // So the truth is taken by walking the ray in fixed world increments
        // and finding the first sign change. Slow, independent of the bound,
        // and it cannot be fooled by either.
        int truth_hits = 0;
        for (int iy = 0; iy < 64; ++iy) {
            for (int ix = 0; ix < 64; ++ix) {
                const float u = static_cast<float>(ix) / 63.0f;
                const float v = static_cast<float>(iy) / 63.0f;
                const kernel::cfloat3 ro = cf3(0.40f + 0.35f * u, 0.40f + 0.35f * v, 2.0f);
                const kernel::cfloat3 rd = cf3(0.0f, 0.0f, -1.0f);
                bool crossed = false;
                float prev = field(ro);
                for (int k = 1; k <= 40000 && !crossed; ++k) {
                    const float t = static_cast<float>(k) * 1e-4f;
                    const float cur = field(ro + rd * t);
                    if (prev > 0.0f && cur <= 0.0f) crossed = true;
                    prev = cur;
                }
                if (crossed) ++truth_hits;
            }
        }
        std::printf("  %6d %10d %14.2f %14.8f %12.3f %8lld %7d %7d%s\n", moves,
                    emitted_deformers(culled), static_cast<double>(culled.info.lipschitz),
                    static_cast<double>(culled.safe_step_scale()), ms, steps, hits, truth_hits,
                    hits == truth_hits ? "" : "  <-- DISAGREES WITH TRUTH");
    }

    std::printf("\n  A tape whose `emitted` is far below `chain` while `declared L` still grows\n"
                "  like the whole chain is the #541 defect. Equal columns refute it.\n");
    return 0;
}
