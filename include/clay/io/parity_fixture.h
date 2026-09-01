#pragma once

// Host parity fixture (build-packaging spec: Host parity fixture).
//
// A host that previews ClayCore documents on its own GPU has to evaluate the
// field itself. The kernel headers are published so that it can do so with
// OUR math (docs/06-host-gpu-previews.md) — this is how it proves it did.
//
// Each case is a composed tape, a fixed set of probe points, and the CPU
// scalar reference distance and color at each probe. A host evaluates the
// same tapes with kernels compiled from the same headers and asserts
// agreement within the stated tolerances, exactly as tests/unit/test_parity.cpp
// does across our own backends.
//
// The case table is chosen for what a hand-written preview gets wrong rather
// than for coverage of the primitive set: every blend profile against every
// smooth boolean, every extended combine mode, the material-mix weights, a
// deformer chain, repetition, the out-of-line blob, and a composed document.
// The blend cases are probed across the seam, so a support-`k` quadratic smin
// where the engine uses `4k` — the drift that cost ClaySpace a debugging
// cycle — fails here rather than at bake time.

#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/kernel/shim.h"
#include "clay/scene/tape.h"

namespace clay {
namespace io {

// Tolerances a consumer should apply: |a - b| <= abs + rel * |expected|.
// Same shape as the backend parity suite's, and deliberately no tighter —
// a host GPU is no more bit-exact than ours.
//
// The two MARCH tolerances are deliberately lopsided, and that is the whole
// design of the march half rather than a fudge. See FixtureRay.
struct FixtureTolerance {
    float distance_abs = 1e-5f;
    float distance_rel = 1e-4f;
    float color_abs = 1e-4f;
    // How far PAST our hit a host's may land. Tight: overstepping is the
    // failure this catches, and no correct marcher does it.
    float hit_t_late_abs = 1e-3f;
    // How far SHORT of it. Loose: a host that steps more conservatively than
    // required accepts further out and lands early, which is safe.
    float hit_t_early_abs = 1e-2f;
};

// A ray the fixture asserts about, and where the CPU scalar reference lands.
//
// EVERY LISTED RAY HITS. A consumer must hit it too, at a `t` no more than
// `hit_t_late_abs` past ours and no more than `hit_t_early_abs` short of it.
//
// The asymmetry is the point. The acceptance test in a sphere trace is
// |f| * step_scale < eps * t, so the step scale appears in the stopping rule
// as well as in the step length: a host that marches more conservatively than
// the tape asks accepts further out and lands EARLY, which is safe and must
// pass. A host that oversteps — the classic being to step by the reported
// distance when the tape says to scale it — lands LATE or misses the surface
// entirely. Late is therefore the only direction that means a defect, and it
// is the only direction held tight.
//
// The listed rays are filtered to ones a DIFFERENTLY WRITTEN marcher can also
// pass: each survives a march with over-relaxation off, with a finer and a
// coarser epsilon, with half the step budget, and with a quarter of the step
// scale. A ray where those disagree is a grazing or unresolvable one and is no
// basis for an expectation, so it is not exported. That is why a case may
// carry few rays or none: the steepest fields in the table (a swept guide at
// step scale 0.015, a loft at 0.117) cannot support a stable one at all.
struct FixtureRay {
    kernel::cfloat3 origin;
    kernel::cfloat3 direction;  // unit
    float t;                    // where the CPU scalar reference lands
};

// The march every case's rays were traced with, and which a consumer should
// reproduce. Exported because `t` depends on `eps`: the acceptance threshold
// is proportional to it.
struct FixtureMarch {
    float tmin = 0.0f;
    float tmax = 6.0f;
    float eps = 1e-4f;
    int max_steps = 512;
};

struct FixtureCase {
    std::string name;
    std::string note;
    scene::Tape tape;
    std::vector<kernel::cfloat3> points;
    std::vector<float> distances;         // CPU scalar reference, per point
    std::vector<kernel::cfloat3> colors;  // ditto
    // The MARCH half: rays that hit, and where. May be empty — see FixtureRay.
    std::vector<FixtureRay> rays;
};

// The probe set every case shares: a ring straddling the seam of the
// two-operand cases, plus deterministic pseudo-random samples around it.
std::vector<kernel::cfloat3> kernel_parity_probe_points();

// The march parameters every case's rays were traced with.
FixtureMarch kernel_parity_march();

// Build the case table. Pure and deterministic: no I/O, no clock, no RNG
// beyond a fixed-seed sequence, so two calls in any build produce the same
// tapes and the same expectations.
std::vector<FixtureCase> kernel_parity_cases();

// Serialize the table as JSON. Floats use the shortest round-tripping form,
// so a consumer reading them back gets the same bits ClayCore evaluated.
std::string kernel_parity_fixture_json(const std::vector<FixtureCase>& cases,
                                       const FixtureTolerance& tol = {});

// Convenience: build the table and write it out.
IoStatus save_kernel_parity_fixture(const std::string& path);

}  // namespace io
}  // namespace clay
