// Making a sampled field a distance field again (sdf-kernels spec,
// add-consolidation-policy). See include/clay/field/redistance.h for why a
// bake alone does not bound a field's Lipschitz, and why the solve is unsigned
// with the sign put back afterwards.

#include "clay/field/redistance.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace clay {
namespace field {

namespace {

// "No distance yet". Finite rather than infinity because it goes through the
// quadratic below, where an infinity would produce a NaN that then spreads.
// Ten orders of magnitude above any distance a scene has is unambiguous and
// squares without overflowing a float.
constexpr float kUnknown = 1e10f;

// The sample lattice, dense, for the duration of the solve. Dense because a
// sweep needs a total order across brick boundaries; see the header for the
// cost and the alternative that was rejected.
struct Lattice {
    int n[3] = {0, 0, 0};
    std::vector<float> value;         // the input sample, where there is one
    std::vector<std::uint8_t> stored; // whether there is one
    std::vector<float> d;             // unsigned distance under construction
    std::vector<std::uint8_t> fixed;  // seeded at the interface; never updated

    std::size_t at(int x, int y, int z) const {
        return static_cast<std::size_t>((z * n[1] + y) * n[0] + x);
    }
    std::size_t count() const {
        return static_cast<std::size_t>(n[0]) * static_cast<std::size_t>(n[1]) *
               static_cast<std::size_t>(n[2]);
    }
};

Lattice read_lattice(const FieldVolume& v) {
    Lattice l;
    for (int a = 0; a < 3; ++a) l.n[a] = v.sample_extent(a);
    l.value.assign(l.count(), 0.0f);
    l.stored.assign(l.count(), 0);
    l.d.assign(l.count(), kUnknown);
    l.fixed.assign(l.count(), 0);
    for (int z = 0; z < l.n[2]; ++z)
        for (int y = 0; y < l.n[1]; ++y)
            for (int x = 0; x < l.n[0]; ++x) {
                const std::optional<float> s = v.sample_at(x, y, z);
                if (!s) continue;
                const std::size_t i = l.at(x, y, z);
                l.value[i] = *s;
                l.stored[i] = 1;
            }
    return l;
}

// Whether any of the six neighbours is stored and on the other side of the
// surface — the test for "this sample is one of the ones bracketing it".
bool brackets_surface(const Lattice& l, int x, int y, int z) {
    constexpr int kNeighbours[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const bool inside = l.value[l.at(x, y, z)] < 0.0f;
    for (const auto& step : kNeighbours) {
        const int nx = x + step[0], ny = y + step[1], nz = z + step[2];
        if (nx < 0 || ny < 0 || nz < 0 || nx >= l.n[0] || ny >= l.n[1] || nz >= l.n[2]) continue;
        const std::size_t j = l.at(nx, ny, nz);
        if (l.stored[j] && (l.value[j] < 0.0f) != inside) return true;
    }
    return false;
}

// One component of the input field's slope: the STEEPER of the two one-sided
// differences, not the central one.
//
// A central difference cancels across a kink — at a facet edge the field falls
// on one side and rises on the other, so the average reads near zero and the
// seed below divides by it. The steeper one-sided difference never cancels,
// and taking the steeper of the pair errs towards a seed nearer the surface,
// which is the safe direction: a seed too near is corrected by the sweep, a
// seed too far is fixed and never revisited.
//
// A neighbour that is not stored contributes nothing rather than a fabricated
// value, which is what `here` on both sides amounts to.
float slope(const Lattice& l, int x, int y, int z, int axis, float cell) {
    int lo[3] = {x, y, z}, hi[3] = {x, y, z};
    lo[axis] -= 1;
    hi[axis] += 1;
    const bool has_lo = lo[axis] >= 0 && l.stored[l.at(lo[0], lo[1], lo[2])];
    const bool has_hi = hi[axis] < l.n[axis] && l.stored[l.at(hi[0], hi[1], hi[2])];
    const float here = l.value[l.at(x, y, z)];
    const float a = has_lo ? l.value[l.at(lo[0], lo[1], lo[2])] : here;
    const float b = has_hi ? l.value[l.at(hi[0], hi[1], hi[2])] : here;
    return std::max(std::abs(here - a), std::abs(b - here)) / cell;
}

// Seed every stored sample that brackets the surface with its distance to it.
//
// WHY |f| / |grad f| RATHER THAN THE CROSSING ALONG AN EDGE. Interpolating
// along one axis gives the distance to the surface ALONG THAT AXIS, which
// overestimates the real one by 1/cos(angle between the axis and the surface
// normal) — so the seeded shell bulges wherever the surface runs oblique to
// the lattice, and every axis disagrees about where the surface is. Dividing
// by the gradient's LENGTH takes the shortest direction instead, which is the
// one first-order estimate all three axes agree on.
//
// The ratio is scale-free, which is what makes it usable here at all: the
// input is a steep field by assumption — that is why it is being redistanced —
// and |f| / |grad f| is unchanged by multiplying the whole field by fourteen.
bool seed_interface(Lattice& l, float cell) {
    bool any = false;
    for (int z = 0; z < l.n[2]; ++z)
        for (int y = 0; y < l.n[1]; ++y)
            for (int x = 0; x < l.n[0]; ++x) {
                const std::size_t i = l.at(x, y, z);
                if (!l.stored[i] || !brackets_surface(l, x, y, z)) continue;
                const float gx = slope(l, x, y, z, 0, cell);
                const float gy = slope(l, x, y, z, 1, cell);
                const float gz = slope(l, x, y, z, 2, cell);
                const float g = std::sqrt(gx * gx + gy * gy + gz * gz);
                // A vanishing gradient says nothing about where the surface
                // is; the sample sits next to a crossing, so half a cell is
                // the honest fallback rather than a division by nothing.
                float d = g > 1e-12f ? std::abs(l.value[i]) / g : 0.5f * cell;
                // A seed is a LOCAL estimate. Past a cell it is extrapolation,
                // and the sweep computes that better than this does.
                l.d[i] = std::min(l.d[i], std::min(d, cell));
                l.fixed[i] = 1;
                any = true;
            }
    return any;
}

// The smaller of the two neighbours along one axis; out of range counts as
// unknown, so the boundary constrains nothing rather than pinning a value.
float upwind(const Lattice& l, int x, int y, int z, int axis) {
    int lo[3] = {x, y, z}, hi[3] = {x, y, z};
    lo[axis] -= 1;
    hi[axis] += 1;
    const float a = lo[axis] >= 0 ? l.d[l.at(lo[0], lo[1], lo[2])] : kUnknown;
    const float b = hi[axis] < l.n[axis] ? l.d[l.at(hi[0], hi[1], hi[2])] : kUnknown;
    return std::min(a, b);
}

// Godunov's upwind solution of |grad d| = 1 at one sample, from its three
// upwind neighbours sorted a <= b <= c. One-dimensional first; the two- and
// three-dimensional roots are only taken when the cheaper one would overtake
// the neighbour it ignored, which is what makes the scheme monotone.
float solve_eikonal(float a, float b, float c, float h) {
    float x = a + h;
    if (x <= b) return x;
    const float two = 2.0f * h * h - (a - b) * (a - b);
    if (two < 0.0f) return x;
    x = 0.5f * (a + b + std::sqrt(two));
    if (x <= c) return x;
    const float sum = a + b + c;
    const float three = sum * sum - 3.0f * (a * a + b * b + c * c - h * h);
    if (three < 0.0f) return x;
    return (sum + std::sqrt(three)) / 3.0f;
}

void relax_sample(Lattice& l, int x, int y, int z, float cell) {
    const std::size_t i = l.at(x, y, z);
    if (l.fixed[i]) return;
    float nb[3] = {upwind(l, x, y, z, 0), upwind(l, x, y, z, 1), upwind(l, x, y, z, 2)};
    std::sort(nb, nb + 3);
    if (nb[0] >= kUnknown) return;  // nothing known nearby yet
    l.d[i] = std::min(l.d[i], solve_eikonal(nb[0], nb[1], nb[2], cell));
}

// One sweep, walking each axis in the direction its sign says. The eight of
// them together cover every characteristic direction, which is why a single
// round resolves a fixed source.
void sweep(Lattice& l, float cell, int sx, int sy, int sz) {
    for (int iz = 0; iz < l.n[2]; ++iz) {
        const int z = sz > 0 ? iz : l.n[2] - 1 - iz;
        for (int iy = 0; iy < l.n[1]; ++iy) {
            const int y = sy > 0 ? iy : l.n[1] - 1 - iy;
            for (int ix = 0; ix < l.n[0]; ++ix) {
                const int x = sx > 0 ? ix : l.n[0] - 1 - ix;
                relax_sample(l, x, y, z, cell);
            }
        }
    }
}

}  // namespace

bool redistance(FieldVolume& v, int rounds) {
    if (v.empty()) return false;
    const float cell = v.cell_size();
    Lattice l = read_lattice(v);
    // No sign change means no zero set on this lattice. Solving anyway would
    // hand back a distance to a surface that is not there, which is worse than
    // the bound the bake already produced.
    if (!seed_interface(l, cell)) return false;

    for (int round = 0; round < std::max(rounds, 1); ++round)
        for (int s = 0; s < 8; ++s)
            sweep(l, cell, (s & 1) ? -1 : 1, (s & 2) ? -1 : 1, (s & 4) ? -1 : 1);

    // The sign is the one piece of the input that a steep field still has
    // right: it crosses zero where a gentle one would.
    v.rewrite([&l](int x, int y, int z, float old) {
        const float d = l.d[l.at(x, y, z)];
        return old < 0.0f ? -d : d;
    });
    v.set_sample_lipschitz(v.measure_sample_lipschitz());
    return true;
}

}  // namespace field
}  // namespace clay
