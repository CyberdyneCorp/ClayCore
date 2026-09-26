#include "clay/mesh/lattice.h"

#include <algorithm>

namespace clay {
namespace mesh {

using kernel::cf3;
using kernel::cfloat3;

namespace {

int clamp_div(int n) {
    return std::max(kMinLatticeDivisions, std::min(kMaxLatticeDivisions, n));
}

// Each C(count - 1, i), the coefficients of the degree-(count - 1) Bernstein
// basis. Exact in a double for every count up to kMaxLatticeDivisions: the
// largest, C(31, 15), is about 3e8.
std::vector<double> binomials(int count) {
    std::vector<double> c(static_cast<std::size_t>(count), 1.0);
    for (int i = 1; i < count; ++i) c[i] = c[i - 1] * static_cast<double>(count - i) / i;
    return c;
}

// Bernstein basis of degree count - 1 at t, written into `out` (count values).
//
// Built from the powers of t and 1 - t, so an axis costs O(n) rather than the
// O(n^2) of de Casteljau's recurrence — which was cheap beside the n^3 sum it
// fed, and is not beside the sum over dragged points that replaced it. In
// double, so the partition of unity holds to far below a float's resolution
// and a uniformly dragged cage still translates exactly once rounded back.
void bernstein(const std::vector<double>& binomial, double t, double* out) {
    const int count = static_cast<int>(binomial.size());
    double rest[kMaxLatticeDivisions];
    rest[0] = 1.0;
    for (int m = 1; m < count; ++m) rest[m] = rest[m - 1] * (1.0 - t);
    double power = 1.0;
    for (int i = 0; i < count; ++i) {
        out[i] = binomial[i] * power * rest[count - 1 - i];
        power *= t;
    }
}

bool is_zero(cfloat3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

// Where p sits in the box, on one axis, clamped to [0, 1].
//
// Clamped because the offset field is only DEFINED over the cage; a vertex
// beyond it takes the offset of the nearest point of the cage and travels
// rigidly, which is what "outside the box is carried along" means.
//
// A DEGENERATE axis — the box is flat there, which is exactly what a cage over
// a plane's own bounds gives — reads as the MIDDLE rather than as either end.
// There is genuinely no information distinguishing the control points on such
// an axis, so any answer is a convention; this one is the convention that
// leaves none of them dead. Reading it as 0 would make every control point
// above the first unreachable, so dragging the "top middle" handle of a cage
// over a plane would do nothing at all — surprising, and the reason this is
// spelled out rather than defaulted.
float axis_parameter(float v, float lo, float hi) {
    const float span = hi - lo;
    if (!(span > 1e-9f)) return 0.5f;
    return kernel::cclamp((v - lo) / span, 0.0f, 1.0f);
}

}  // namespace

Lattice::Lattice(const math::Aabb& box, int nx, int ny, int nz)
    : box_(box), nx_(clamp_div(nx)), ny_(clamp_div(ny)), nz_(clamp_div(nz)) {
    offsets_.assign(static_cast<std::size_t>(nx_) * ny_ * nz_, cf3(0, 0, 0));
    binomial_x_ = binomials(nx_);
    binomial_y_ = binomials(ny_);
    binomial_z_ = binomials(nz_);
}

std::size_t Lattice::index(int i, int j, int k) const {
    return (static_cast<std::size_t>(k) * ny_ + j) * nx_ + i;
}

cfloat3 Lattice::offset(int i, int j, int k) const {
    if (i < 0 || j < 0 || k < 0 || i >= nx_ || j >= ny_ || k >= nz_) return cf3(0, 0, 0);
    return offsets_[index(i, j, k)];
}

void Lattice::set_offset(int i, int j, int k, cfloat3 v) {
    if (i < 0 || j < 0 || k < 0 || i >= nx_ || j >= ny_ || k >= nz_) return;
    const std::size_t at = index(i, j, k);
    const bool was = !is_zero(offsets_[at]);
    const bool now = !is_zero(v);
    offsets_[at] = v;
    if (now && !was) {
        dragged_.push_back(static_cast<std::uint32_t>(at));
    } else if (was && !now) {
        dragged_.erase(std::find(dragged_.begin(), dragged_.end(), static_cast<std::uint32_t>(at)));
    }
}

cfloat3 Lattice::rest(int i, int j, int k) const {
    if (box_.empty()) return cf3(0, 0, 0);
    auto along = [](float lo, float hi, int idx, int count) {
        return count < 2 ? lo
                         : lo + (hi - lo) * (static_cast<float>(idx) /
                                             static_cast<float>(count - 1));
    };
    return cf3(along(box_.min.x, box_.max.x, i, nx_), along(box_.min.y, box_.max.y, j, ny_),
               along(box_.min.z, box_.max.z, k, nz_));
}

cfloat3 Lattice::displacement(cfloat3 p) const {
    if (box_.empty() || dragged_.empty()) return cf3(0, 0, 0);
    const double s = axis_parameter(p.x, box_.min.x, box_.max.x);
    const double t = axis_parameter(p.y, box_.min.y, box_.max.y);
    const double u = axis_parameter(p.z, box_.min.z, box_.max.z);

    // Degree is one less than the point count, so a two-point axis is degree
    // one — exactly linear — and no separate trilinear path is needed.
    double bx[kMaxLatticeDivisions], by[kMaxLatticeDivisions], bz[kMaxLatticeDivisions];
    bernstein(binomial_x_, s, bx);
    bernstein(binomial_y_, t, by);
    bernstein(binomial_z_, u, bz);

    // Only the points somebody dragged: every other offset is zero and would
    // add exactly nothing. See the header for what the full sum cost.
    const std::size_t row = static_cast<std::size_t>(nx_);
    const std::size_t plane = row * static_cast<std::size_t>(ny_);
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (const std::uint32_t at : dragged_) {
        const double w = bx[at % row] * by[(at / row) % ny_] * bz[at / plane];
        const cfloat3& o = offsets_[at];
        sx += w * o.x;
        sy += w * o.y;
        sz += w * o.z;
    }
    return cf3(static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz));
}

}  // namespace mesh
}  // namespace clay
