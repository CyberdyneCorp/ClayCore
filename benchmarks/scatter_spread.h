#pragma once

// A deterministic spread through the working volume, for every case that needs
// SCATTERED material rather than one block.
//
// The multipliers are the fractional parts of sqrt(2), sqrt(3) and sqrt(5), and
// the choice is load-bearing rather than decorative.
//
// These cases used to walk `g`, `g*g`, `g*g*g` for the golden ratio, described
// as a low-discrepancy walk. It was not one. `g` satisfies `g^2 = 1-g`, so
// `frac(i*g^2) == 1 - frac(i*g)` and the second coordinate was exactly the
// negation of the first: every point landed on the plane x+y=0. `g^3 = 2g-1`
// tied the third to the first as well, leaving a curve in a plane. The
// "scattered" material was a diagonal sheet — the one shape these benchmarks
// exist to avoid, as their own comments say.
//
// It cost more than tidiness. On a 40-cell lattice the collapse piled 400
// blobs of 64 cells into 3,492 distinct cells instead of ~25,600, so the case
// measured a seventh of the work it claimed and the ceiling was calibrated
// against that.
//
// The powers of any low-degree algebraic number carry a relation like this one.
// The plastic number's own fail identically — a^2 + a^3 == 1 exactly, so z
// would have been -y — which is worth knowing, because it is the obvious
// replacement. Distinct square-free radicands cannot fail this way: 1, sqrt(2),
// sqrt(3) and sqrt(5) are linearly independent over the rationals, so no
// relation of that shape exists to be tripped over.
//
// tests/unit/test_bench_spread.cpp holds the property. The device harness
// carries its own copy in Swift (tests/device/Tests/LatencyCases.swift,
// `stampMultipliers`) with a matching test, because it cannot include this.

namespace clay_bench {

inline constexpr double kScatterX = 0.4142135624;  // frac(sqrt 2)
inline constexpr double kScatterY = 0.7320508076;  // frac(sqrt 3)
inline constexpr double kScatterZ = 0.2360679775;  // frac(sqrt 5)

// The i-th point of the spread, each coordinate in [0, 1).
inline void scatter_unit(int i, double* x, double* y, double* z) {
    const double n = static_cast<double>(i);
    auto fr = [](double v) { return v - static_cast<double>(static_cast<long long>(v)); };
    *x = fr(n * kScatterX);
    *y = fr(n * kScatterY);
    *z = fr(n * kScatterZ);
}

// The same spread on an integer cell lattice: the [-0.8, 0.8] working volume
// scaled to `scale` cells per unit.
inline void scatter_cell(int i, int scale, int* x, int* y, int* z) {
    double u = 0.0, v = 0.0, w = 0.0;
    scatter_unit(i, &u, &v, &w);
    *x = static_cast<int>((u * 1.6 - 0.8) * scale);
    *y = static_cast<int>((v * 1.6 - 0.8) * scale);
    *z = static_cast<int>((w * 1.6 - 0.8) * scale);
}

}  // namespace clay_bench
