#include "clay/mesh/dual_contouring.h"

#include "clay/kernel/field.h"
#include "dual_grid.h"

namespace clay {
namespace mesh {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// QEF vertex: minimize sum(dot(n_i, x - p_i)^2) over the cell's Hermite
// crossings via the 3x3 normal equations (A^T A + lambda*I) y = A^T r,
// solved around the crossing centroid (mass point) so the regularization
// biases toward it. No SVD: lambda keeps the system SPD even when all
// normals are coplanar (flat faces), and the solve is Cramer's rule.
cfloat3 qef_vertex(const detail::DualCrossing* crossings, int count, float lambda, cfloat3 lo,
                   cfloat3 hi) {
    if (count == 0) return (lo + hi) * 0.5f;  // unreachable for sign-changing cells
    cfloat3 mass = cf3(0, 0, 0);
    for (int i = 0; i < count; ++i) mass = mass + crossings[i].pos;
    mass = mass / static_cast<float>(count);

    double a00 = lambda, a11 = lambda, a22 = lambda;
    double a01 = 0, a02 = 0, a12 = 0;
    double b0 = 0, b1 = 0, b2 = 0;
    for (int i = 0; i < count; ++i) {
        cfloat3 n = crossings[i].normal;
        double d = kernel::cdot(n, crossings[i].pos - mass);
        a00 += (double)n.x * n.x;
        a01 += (double)n.x * n.y;
        a02 += (double)n.x * n.z;
        a11 += (double)n.y * n.y;
        a12 += (double)n.y * n.z;
        a22 += (double)n.z * n.z;
        b0 += n.x * d;
        b1 += n.y * d;
        b2 += n.z * d;
    }
    double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) +
                 a02 * (a01 * a12 - a11 * a02);  // SPD -> det >= lambda^3 > 0
    double y0 = (b0 * (a11 * a22 - a12 * a12) - a01 * (b1 * a22 - a12 * b2) +
                 a02 * (b1 * a12 - a11 * b2)) /
                det;
    double y1 = (a00 * (b1 * a22 - a12 * b2) - b0 * (a01 * a22 - a12 * a02) +
                 a02 * (a01 * b2 - b1 * a02)) /
                det;
    double y2 = (a00 * (a11 * b2 - b1 * a12) - a01 * (a01 * b2 - b1 * a02) +
                 b0 * (a01 * a12 - a11 * a02)) /
                det;
    cfloat3 x = mass + cf3((float)y0, (float)y1, (float)y2);
    return kernel::cclamp(x, lo, hi);  // keep the dual vertex inside its cell
}

}  // namespace

Mesh mesh_lattice_dc(const std::function<float(int, int, int)>& sample, const int cell_min[3],
                     const int cell_max[3], kernel::cfloat3 origin, float spacing,
                     float qef_lambda) {
    auto normal_at = [&](cfloat3, const int p0[3], const int p1[3], float t) {
        auto grad = [&](const int p[3]) {
            return cf3(sample(p[0] + 1, p[1], p[2]) - sample(p[0] - 1, p[1], p[2]),
                       sample(p[0], p[1] + 1, p[2]) - sample(p[0], p[1] - 1, p[2]),
                       sample(p[0], p[1], p[2] + 1) - sample(p[0], p[1], p[2] - 1));
        };
        cfloat3 g = grad(p0) * (1.0f - t) + grad(p1) * t;
        float len = kernel::clength(g);
        return len > 1e-20f ? g / len : cf3(0, 1, 0);
    };
    auto place = [&](const detail::DualCrossing* crossings, int count, cfloat3 lo, cfloat3 hi) {
        return qef_vertex(crossings, count, qef_lambda, lo, hi);
    };
    return detail::dual_grid_mesh(sample, cell_min, cell_max, origin, spacing, normal_at, place);
}

Mesh mesh_tape_dc(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
                  const DualContouringOptions& options) {
    if (!options.enable_experimental) return {};  // meshing spec: flagged until hardened
    detail::TapeGrid grid = detail::eval_tape_grid(tape, region, voxel_size);
    if (!grid.ok) return {};
    auto sample = [&](int i, int j, int k) { return grid.at(i, j, k); };
    auto field = [&](cfloat3 p) { return tape.eval(p).d; };
    auto normal_at = [&](cfloat3 p, const int*, const int*, float) {
        return kernel::cnormal(field, p, options.attributes.gradient_eps);
    };
    auto place = [&](const detail::DualCrossing* crossings, int count, cfloat3 lo, cfloat3 hi) {
        return qef_vertex(crossings, count, options.qef_lambda, lo, hi);
    };
    // one extra cell ring: out-of-range samples are positive, so geometry
    // crossing the region boundary is closed (same ring as mesh_tape)
    int cmin[3] = {-1, -1, -1};
    int cmax[3] = {grid.nx, grid.ny, grid.nz};
    Mesh m = detail::dual_grid_mesh(sample, cmin, cmax, region.min, voxel_size, normal_at, place);
    apply_tape_attributes(m, tape, options.attributes);
    return m;
}

}  // namespace mesh
}  // namespace clay
