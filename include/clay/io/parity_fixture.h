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
struct FixtureTolerance {
    float distance_abs = 1e-5f;
    float distance_rel = 1e-4f;
    float color_abs = 1e-4f;
};

struct FixtureCase {
    std::string name;
    std::string note;
    scene::Tape tape;
    std::vector<kernel::cfloat3> points;
    std::vector<float> distances;         // CPU scalar reference, per point
    std::vector<kernel::cfloat3> colors;  // ditto
};

// The probe set every case shares: a ring straddling the seam of the
// two-operand cases, plus deterministic pseudo-random samples around it.
std::vector<kernel::cfloat3> kernel_parity_probe_points();

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
