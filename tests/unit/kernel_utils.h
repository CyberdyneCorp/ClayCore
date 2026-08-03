#pragma once

// Shared helpers for kernel tests: deterministic RNG and the
// conservative-step property (the load-bearing SDF safety invariant:
// stepping by |f(p)| * step_scale from p never crosses the surface).

#include <doctest/doctest.h>

#include <cstdint>

#include "clay/kernel/shim.h"

namespace clay_test {

using namespace clay::kernel;

struct Lcg {
    std::uint64_t state;
    explicit Lcg(std::uint64_t seed) : state(seed) {}
    float next01() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((state >> 40) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
    cfloat2 vec2(float lo, float hi) { return cf2(range(lo, hi), range(lo, hi)); }
    cfloat3 vec3(float lo, float hi) { return cf3(range(lo, hi), range(lo, hi), range(lo, hi)); }
    cfloat3 unit3() {
        // rejection-sample a unit vector
        for (;;) {
            cfloat3 v = vec3(-1.0f, 1.0f);
            float l2 = cdot2(v);
            if (l2 > 1e-4f && l2 <= 1.0f) return v / csqrt(l2);
        }
    }
};

// Property: for random p, stepping from p by 0.99 * |f(p)| * step_scale in
// ANY direction never changes the field's sign (checked with dense samples
// along the segment). Holds for exact fields and safe bounds.
template <typename F>
inline void check_conservative_steps(F f, float step_scale, float extent, int trials,
                                     std::uint64_t seed) {
    Lcg rng(seed);
    int checked = 0;
    for (int i = 0; i < trials; ++i) {
        cfloat3 p = rng.vec3(-extent, extent);
        float d = f(p);
        if (cabs(d) < 1e-3f) continue;  // already on the surface
        cfloat3 dir = rng.unit3();
        float step = 0.99f * cabs(d) * step_scale;
        for (int s = 1; s <= 8; ++s) {
            float t = step * static_cast<float>(s) / 8.0f;
            float ft = f(p + dir * t);
            if (csign(ft) != csign(d) && cabs(ft) > 1e-5f) {
                CAPTURE(p.x);
                CAPTURE(p.y);
                CAPTURE(p.z);
                CAPTURE(d);
                CAPTURE(t);
                CAPTURE(ft);
                FAIL_CHECK("conservative-step violation");
                return;
            }
        }
        ++checked;
    }
    // Make sure the test exercised enough non-surface samples.
    CHECK(checked > trials / 2);
}

// Property: |f(p) - f(q)| <= L * |p - q| (Lipschitz bound) over random pairs.
template <typename F>
inline void check_lipschitz(F f, float lipschitz, float extent, int trials, std::uint64_t seed) {
    Lcg rng(seed);
    for (int i = 0; i < trials; ++i) {
        cfloat3 p = rng.vec3(-extent, extent);
        cfloat3 q = rng.vec3(-extent, extent);
        float lhs = cabs(f(p) - f(q));
        float rhs = lipschitz * clength(p - q) + 1e-4f;
        CAPTURE(p.x);
        CAPTURE(q.x);
        REQUIRE(lhs <= rhs);
    }
}

}  // namespace clay_test
