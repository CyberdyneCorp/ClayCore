// COULD A GRADIENT NORMAL COME FROM THE STORED LATTICE INSTEAD OF THE FIELD?
// NOT a gated benchmark.
//
// #589 measured that the ENTIRE depth-growth of brick meshing is the gradient
// normals: the march is flat at 0.99x from chain depth 0 to 48 while the
// attribute pass grows 10.8x, because a gradient is a tetrahedron tap -- about
// four field evaluations per vertex -- and each walks the surviving deformer
// chain.
//
// The brick cache ALREADY holds dim^3 samples of that field. Central-differencing
// them costs ZERO field evaluations, so the whole 10.8 ms would go. This probe
// exists to find the price, and the price is accuracy.
//
// WHY IT MIGHT BE UNACCEPTABLE, stated before measuring so the number is not
// read charitably. Three known sources of error, none of them small a priori:
//
//   band clamping   a sample is clamped to +-band, so any vertex whose
//                   neighbourhood reaches the clamp has a differenced gradient
//                   pointing partly along the clamp plane rather than the surface
//   fp16 storage    about 3 decimal digits; a central difference DIVIDES by the
//                   spacing, so quantisation is amplified by 1/(2h)
//   lattice spacing the difference is over a whole voxel, so it answers for the
//                   voxel and not the point
//
// And there is a measured precedent for refusing a cheaper normal: #550 found
// CLAY_NORMAL_FACE on a coarse lattice up to 84.78 degrees off the field where
// the gradient read 0.00. "Cheaper normals" is not automatically acceptable on a
// sculpting engine.
//
// SO THE VERDICT IS AN ANGLE, not a speedup. Reported as a distribution rather
// than a mean, because a normal that is right on average and wrong on a rim is
// what a sculptor sees.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/eval/backend.h"
#include "clay/kernel/shim.h"
#include "clay/mesh/marching.h"
#include "clay/scene/bounds.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using kernel::cfloat3;

namespace {

constexpr float kVoxel = 0.02f;
constexpr int kDim = 8;
constexpr int kBand = 3;

scene::Node item(scene::Prim prim, cfloat3 pos) {
    scene::Node n;
    n.prim = prim;
    n.xform.position = pos;
    n.op = scene::Op::Add;
    return n;
}

// The tetrahedron tap the mesher actually uses, through the tape.
cfloat3 tape_gradient(const scene::Tape& tape, cfloat3 p, float eps) {
    const cfloat3 k0 = cf3(1, -1, -1), k1 = cf3(-1, -1, 1);
    const cfloat3 k2 = cf3(-1, 1, -1), k3 = cf3(1, 1, 1);
    cfloat3 g = k0 * tape.eval(p + k0 * eps).d + k1 * tape.eval(p + k1 * eps).d +
                k2 * tape.eval(p + k2 * eps).d + k3 * tape.eval(p + k3 * eps).d;
    const float len = kernel::clength(g);
    return len > 0.0f ? g / len : cf3(0, 0, 1);
}

// TRILINEAR read of the stored lattice, then a central difference on it.
//
// Deliberately not the nearest-sample form. A first version of this probe took
// the nearest lower sample and reported a p90 of 18.3 degrees, which would have
// refuted the idea on a weak implementation -- the cheap-normal question
// deserves the good version or no verdict at all.
float lattice_at(const brick::BrickCache& cache, cfloat3 q, bool* clamped) {
    const float w = static_cast<float>(kDim) * kVoxel;
    const float band = static_cast<float>(kBand) * kVoxel;
    // Sample centres sit at (i + 0.5) * voxel inside the brick, so shift into
    // that frame before flooring or every read is half a voxel out.
    auto one = [&](int gi, int gj, int gk) -> float {
        const float wx = (static_cast<float>(gi) + 0.5f) * kVoxel;
        const float wy = (static_cast<float>(gj) + 0.5f) * kVoxel;
        const float wz = (static_cast<float>(gk) + 0.5f) * kVoxel;
        const brick::BrickKey key{static_cast<int>(std::floor(wx / w)),
                                  static_cast<int>(std::floor(wy / w)),
                                  static_cast<int>(std::floor(wz / w))};
        const brick::Brick* b = cache.find(key);
        if (!b) return band;
        const int i = gi - key.x * kDim, j = gj - key.y * kDim, k = gk - key.z * kDim;
        if (i < 0 || j < 0 || k < 0 || i >= kDim || j >= kDim || k >= kDim) return band;
        const float v = cache.sample(key, i, j, k);
        if (std::fabs(v) >= band * 0.999f) *clamped = true;
        return v;
    };
    const float gx = q.x / kVoxel - 0.5f, gy = q.y / kVoxel - 0.5f, gz = q.z / kVoxel - 0.5f;
    const int i0 = static_cast<int>(std::floor(gx));
    const int j0 = static_cast<int>(std::floor(gy));
    const int k0 = static_cast<int>(std::floor(gz));
    const float fx = gx - static_cast<float>(i0);
    const float fy = gy - static_cast<float>(j0);
    const float fz = gz - static_cast<float>(k0);
    float c[8];
    int n = 0;
    for (int dk = 0; dk <= 1; ++dk)
        for (int dj = 0; dj <= 1; ++dj)
            for (int di = 0; di <= 1; ++di) c[n++] = one(i0 + di, j0 + dj, k0 + dk);
    const float x00 = c[0] + (c[1] - c[0]) * fx, x10 = c[2] + (c[3] - c[2]) * fx;
    const float x01 = c[4] + (c[5] - c[4]) * fx, x11 = c[6] + (c[7] - c[6]) * fx;
    const float y0 = x00 + (x10 - x00) * fy, y1 = x01 + (x11 - x01) * fy;
    return y0 + (y1 - y0) * fz;
}

bool lattice_gradient(const brick::BrickCache& cache, cfloat3 p, cfloat3* out,
                      bool* touched_clamp) {
    const float h = kVoxel;
    *touched_clamp = false;
    const float xp = lattice_at(cache, p + cf3(h, 0, 0), touched_clamp);
    const float xm = lattice_at(cache, p - cf3(h, 0, 0), touched_clamp);
    const float yp = lattice_at(cache, p + cf3(0, h, 0), touched_clamp);
    const float ym = lattice_at(cache, p - cf3(0, h, 0), touched_clamp);
    const float zp = lattice_at(cache, p + cf3(0, 0, h), touched_clamp);
    const float zm = lattice_at(cache, p - cf3(0, 0, h), touched_clamp);
    cfloat3 g = cf3(xp - xm, yp - ym, zp - zm);
    const float len = kernel::clength(g);
    if (!(len > 0.0f)) return false;
    *out = g / len;
    return true;
}

}  // namespace

int main() {
    std::printf("Gradient normals: the tape's tetrahedron tap against a central\n");
    std::printf("difference on the brick cache's own stored lattice.\n");
    std::printf("unit sphere, voxel %.3f, dim %d, band %d.\n\n",
                static_cast<double>(kVoxel), kDim, kBand);

    const int depths[3] = {0, 12, 48};
    for (int depth : depths) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("form");
    scene::Node n = item(scene::Prim::sphere(1.0f), cf3(0, 0, 0));
    // A Move chain is exactly a stack of grabs. Each at its own centre so they
    // stack rather than coalesce, and shallow so the form survives 48 of them.
    for (int i = 0; i < depth; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(depth + 1);
        const float ang = 2.4f * static_cast<float>(i);
        const cfloat3 c = cf3(0.35f * std::cos(ang) * t, 0.35f * std::sin(ang) * t,
                              std::sqrt(std::max(0.05f, 1.0f - 0.1225f * t * t)));
        n.deformers.push_back(scene::Deformer::grab(c, 0.35f, cf3(0, 0, 0.012f)));
    }
    l.sdf->insert(n);

    brick::BrickCache cache(brick::BrickConfig{kDim, kVoxel, kBand, 0});
    cache.mark_dirty(scene::layer_influence_bound(doc.layers[0]));
    eval::Backend* cpu = eval::Registry::instance().find("cpu");
    for (const brick::BrickRequest& req : cache.take_dirty()) {
        scene::CullRegion cull{cache.cull_region(req.key)};
        scene::Tape t = scene::compile_document(doc, &cull);
        std::vector<float> values(static_cast<std::size_t>(req.grid.nx) * req.grid.ny *
                                  req.grid.nz);
        if (cpu->eval_grid(t, req.grid, values.data()) != eval::Status::Ok) {
            std::printf("FAIL: eval_grid\n");
            return 1;
        }
        cache.submit(req, values.data());
    }

    mesh::Mesh m = mesh::mesh_bricks(cache, &doc);
    if (m.positions.empty()) {
        std::printf("FAIL: meshed nothing\n");
        return 1;
    }
    const scene::Tape whole = scene::compile_document(doc);

    std::vector<double> deg, deg_clamped, deg_clean;
    std::size_t unavailable = 0;
    for (std::size_t i = 0; i < m.positions.size(); i += 7) {  // a seventh, for time
        cfloat3 lat;
        bool clamped = false;
        if (!lattice_gradient(cache, m.positions[i], &lat, &clamped)) {
            ++unavailable;
            continue;
        }
        const cfloat3 ref = tape_gradient(whole, m.positions[i], 1e-4f);
        const double d = std::acos(std::clamp(
                             static_cast<double>(kernel::cdot(ref, lat)), -1.0, 1.0)) *
                         57.29578;
        deg.push_back(d);
        (clamped ? deg_clamped : deg_clean).push_back(d);
    }
    if (deg.empty()) {
        std::printf("FAIL: no vertex could be differenced, so nothing was compared.\n");
        return 1;
    }

    auto pct = [](std::vector<double>& v, double p) {
        std::sort(v.begin(), v.end());
        return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
    };
    std::printf("\n=== CHAIN DEPTH %d ===\n", depth);
    std::printf("  vertices compared   %zu of %zu   (%zu had no full 6-neighbourhood)\n",
                deg.size(), m.positions.size(), unavailable);
    std::printf("\n  angle between the two normals, degrees:\n");
    std::printf("    median   %7.3f\n", pct(deg, 0.50));
    std::printf("    p90      %7.3f\n", pct(deg, 0.90));
    std::printf("    p99      %7.3f\n", pct(deg, 0.99));
    std::printf("    worst    %7.3f\n", pct(deg, 1.00));

    if (!deg_clean.empty())
        std::printf("\n  away from the band clamp (%zu):  median %6.3f   p99 %6.3f   worst %6.3f\n",
                    deg_clean.size(), pct(deg_clean, 0.5), pct(deg_clean, 0.99),
                    pct(deg_clean, 1.0));
    if (!deg_clamped.empty())
        std::printf("  touching the band clamp (%zu):   median %6.3f   p99 %6.3f   worst %6.3f\n",
                    deg_clamped.size(), pct(deg_clamped, 0.5), pct(deg_clamped, 0.99),
                    pct(deg_clamped, 1.0));

    }  // depths

    std::printf("\n  For scale: #550 measured CLAY_NORMAL_FACE on a coarse lattice at up\n");
    std::printf("  to 84.78 degrees from the field, and that was judged unusable.\n");
    return 0;
}
