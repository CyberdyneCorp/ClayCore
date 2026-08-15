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

// Bernstein basis of degree n at t, written into `out` (n + 1 values).
//
// De Casteljau's recurrence rather than binomial coefficients times powers:
// it needs no factorials, cannot overflow at the degrees a cage reaches, and
// is numerically the better-behaved of the two. Each row is built from the one
// above by b[i] = (1 - t) * b[i] + t * b[i - 1].
void bernstein(int degree, float t, float* out) {
    out[0] = 1.0f;
    for (int d = 1; d <= degree; ++d) {
        out[d] = t * out[d - 1];
        for (int i = d - 1; i > 0; --i) out[i] = (1.0f - t) * out[i] + t * out[i - 1];
        out[0] = (1.0f - t) * out[0];
    }
}

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
    offsets_[index(i, j, k)] = v;
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

bool Lattice::is_identity() const {
    for (const cfloat3& o : offsets_)
        if (o.x != 0.0f || o.y != 0.0f || o.z != 0.0f) return false;
    return true;
}

cfloat3 Lattice::displacement(cfloat3 p) const {
    if (box_.empty()) return cf3(0, 0, 0);
    const float s = axis_parameter(p.x, box_.min.x, box_.max.x);
    const float t = axis_parameter(p.y, box_.min.y, box_.max.y);
    const float u = axis_parameter(p.z, box_.min.z, box_.max.z);

    // Degree is one less than the point count, so a two-point axis is degree
    // one — exactly linear — and no separate trilinear path is needed.
    float bx[kMaxLatticeDivisions], by[kMaxLatticeDivisions], bz[kMaxLatticeDivisions];
    bernstein(nx_ - 1, s, bx);
    bernstein(ny_ - 1, t, by);
    bernstein(nz_ - 1, u, bz);

    cfloat3 sum = cf3(0, 0, 0);
    for (int k = 0; k < nz_; ++k) {
        if (bz[k] == 0.0f) continue;
        for (int j = 0; j < ny_; ++j) {
            const float wjk = by[j] * bz[k];
            if (wjk == 0.0f) continue;
            for (int i = 0; i < nx_; ++i) {
                const float w = bx[i] * wjk;
                if (w == 0.0f) continue;
                sum = sum + offsets_[index(i, j, k)] * w;
            }
        }
    }
    return sum;
}

}  // namespace mesh
}  // namespace clay
