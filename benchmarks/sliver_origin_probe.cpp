// WHERE DO THE BRICK MESHER'S SLIVERS COME FROM? NOT a gated benchmark.
//
// THE QUESTION, and why it has to be answered before anything is built.
// #549's option A is "clamp the marcher's interpolation parameter away from
// the endpoints". It is provably SAFE -- the marching case index comes from
// sign tests alone, so `t` cannot change which triangles exist, only where
// their vertices sit -- and it is now justified on performance, because the
// slivers force a host onto clay_document_mesh and cost it 4.15x on settle.
//
// What has never been checked is whether the clamp would WORK. The reasoning
// is that a sliver arises when `t = f0/(f0-f1)` lands at 0 or 1, which happens
// when fp16 quantisation or band clamping makes two lattice samples equal or
// nearly so. Plausible, and untested. If slivers come from somewhere else, the
// clamp costs a goldens refresh, ~10 device baseline cases, 618 host PNGs and
// a device gate, and fixes nothing.
//
// HOW THIS ANSWERS IT WITHOUT INSTRUMENTING THE ENGINE. A vertex at t = 0 or
// t = 1 sits exactly ON a lattice point. So for every vertex, measure how far
// it is from the lattice, in units of the voxel: a pinned vertex reads ~0, a
// vertex interpolated properly along its edge reads anywhere in (0, 0.5].
//
// Then compare the vertices OF SLIVERS against the population. If the clamp is
// the right fix, sliver vertices are pinned and ordinary ones are not. If both
// distributions look alike, the clamp is not the fix and #549 needs a different
// answer.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

constexpr int kDim = 8;
constexpr float kVoxel = 0.05f;
constexpr float kGrabRadius = 0.40f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

bool build(Doc* out, int dabs) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) return false;
    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!item) return false;
    clay_item_set_op(item, CLAY_OP_ADD);
    clay_node_id node = 0;
    const clay_result res = clay_layer_add_item(doc, layer, item, &node);
    clay_item_destroy(item);
    if (res != CLAY_OK) return false;
    for (int i = 0; i < dabs; ++i) {
        const float a = 0.37f * static_cast<float>(i);
        const float centre[3] = {std::cos(a) * 0.55f, std::sin(a) * 0.55f, 0.80f};
        const float disp[3] = {0.0f, 0.0f, 0.05f};
        clay_move_params mp{};
        mp.struct_size = sizeof(mp);
        mp.radius = kGrabRadius;
        std::size_t applied = 0;
        if (clay_layer_move_surface(doc, layer, centre, disp, &mp, &applied) != CLAY_OK) break;
    }
    out->doc = doc;
    out->layer = layer;
    return true;
}

long drain(clay_brick_cache* cache, const clay_document* against) {
    long refilled = 0;
    std::vector<clay_brick_request> reqs(8192);
    std::vector<float> values;
    for (;;) {
        std::size_t count = reqs.size(), remaining = 0;
        if (clay_brick_cache_take_dirty(cache, reqs.data(), &count, &remaining) != CLAY_OK)
            return -1;
        if (count == 0) break;
        values.assign(count * static_cast<std::size_t>(kDim) * kDim * kDim, 0.0f);
        if (clay_brick_cache_eval_requests(against, "cpu", reqs.data(), count, values.data(),
                                           values.size(), nullptr, 0) != CLAY_OK)
            return -1;
        std::size_t accepted = 0;
        if (clay_brick_cache_submit(cache, reqs.data(), count, values.data(), values.size(),
                                    nullptr, 0, nullptr, &accepted) != CLAY_OK)
            return -1;
        refilled += static_cast<long>(count);
        if (remaining == 0) break;
    }
    return refilled;
}

// How far a coordinate sits from the lattice, in voxels: 0 means exactly on a
// lattice plane, 0.5 means exactly between two.
double off_lattice(float x) {
    const double u = static_cast<double>(x) / kVoxel;
    return std::fabs(u - std::round(u));
}

// THE INTERPOLATED AXIS IS THE MAX, NOT THE MIN, and getting that wrong makes
// this probe measure nothing.
//
// A marching-cubes vertex sits on a lattice EDGE, so two of its three
// coordinates are exactly lattice values by construction and only the third
// carries `t`. Taking the min over axes therefore reads ~0 for every vertex in
// the mesh -- which is what the first version of this did, and both
// populations came back with median 0.0000 and ~80% under 0.01. Two identical
// distributions is the tell that the measure is not measuring.
//
// The max picks out the axis the crossing actually ran along: 0 there means t
// landed at an endpoint, 0.5 means it landed mid-edge.
double vertex_pinning(const float* p) {
    return std::max(std::max(off_lattice(p[0]), off_lattice(p[1])), off_lattice(p[2]));
}

void report(const char* label, std::vector<double> v) {
    if (v.empty()) {
        std::printf("  %-22s (none)\n", label);
        return;
    }
    std::sort(v.begin(), v.end());
    auto q = [&](double f) { return v[static_cast<std::size_t>(f * (v.size() - 1))]; };
    std::size_t pinned = 0;
    for (double x : v)
        if (x < 0.01) ++pinned;
    std::printf("  %-22s n=%-7zu p10=%.4f  median=%.4f  p90=%.4f   under 0.01: %.1f%%\n", label,
                v.size(), q(0.10), q(0.50), q(0.90),
                100.0 * static_cast<double>(pinned) / static_cast<double>(v.size()));
}

}  // namespace

int main() {
    std::printf("sliver_origin_probe: are the brick mesher's slivers pinned to the lattice?\n");
    std::printf("  If they are, clamping the marcher's t is the right fix for #549.\n");
    std::printf("  If they are not, it costs a goldens refresh and fixes nothing.\n\n");

    Doc d;
    if (!build(&d, 48)) { std::printf("FAIL: fixture\n"); return 1; }
    clay_brick_config bc{};
    bc.struct_size = sizeof(bc);
    bc.dim = kDim;
    bc.voxel_size = kVoxel;
    bc.band_voxels = 2;
    clay_brick_cache* cache = clay_brick_cache_create(&bc);
    if (!cache) { std::printf("FAIL: cache\n"); return 1; }
    const float wmin[3] = {-2.0f, -2.0f, -2.0f};
    const float wmax[3] = {2.0f, 2.0f, 2.0f};
    clay_brick_cache_mark_dirty(cache, wmin, wmax);
    if (drain(cache, d.doc) <= 0) { std::printf("FAIL: refill\n"); return 1; }

    clay_brick_mesh_params p{};
    p.struct_size = sizeof(p);
    p.normals = CLAY_NORMAL_GRADIENT;
    p.colors = 0;
    clay_mesh* m = nullptr;
    if (clay_brick_cache_mesh(cache, d.doc, &p, nullptr, 0, nullptr, &m) != CLAY_OK || !m) {
        std::printf("FAIL: mesh\n");
        return 1;
    }
    const std::size_t nv = clay_mesh_vertex_count(m);
    const std::size_t ni = clay_mesh_index_count(m);
    struct P { float x, y, z; };
    std::vector<P> pos(nv);
    std::vector<std::uint32_t> idx(ni);
    clay_vertex_layout lay{};
    lay.struct_size = sizeof(lay);
    lay.stride = sizeof(P);
    lay.position_offset = 0;
    lay.normal_offset = -1;
    lay.color_offset = -1;
    lay.uv_offset = -1;
    const bool got = clay_mesh_copy_vertices(m, &lay, pos.data(), nv * sizeof(P)) == CLAY_OK &&
                     clay_mesh_copy_indices(m, idx.data(), ni) == CLAY_OK;
    clay_mesh_destroy(m);
    if (!got || nv == 0 || ni == 0) { std::printf("FAIL: read back\n"); return 1; }

    // Twice the area is the cross product's length; a sliver is one whose is a
    // tiny fraction of the mesh's largest.
    const std::size_t tris = ni / 3;
    std::vector<double> area2(tris, 0.0);
    double longest = 0.0;
    for (std::size_t t = 0; t < tris; ++t) {
        const P& a = pos[idx[t * 3]];
        const P& b = pos[idx[t * 3 + 1]];
        const P& c = pos[idx[t * 3 + 2]];
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        area2[t] = std::sqrt(cx * cx + cy * cy + cz * cz);
        longest = std::max(longest, area2[t]);
    }
    const double cut = longest * 1e-3;

    std::vector<double> sliver_v, ordinary_v;
    std::size_t slivers = 0;
    for (std::size_t t = 0; t < tris; ++t) {
        const bool thin = area2[t] < cut;
        if (thin) ++slivers;
        for (int k = 0; k < 3; ++k) {
            const double pin = vertex_pinning(&pos[idx[t * 3 + k]].x);
            (thin ? sliver_v : ordinary_v).push_back(pin);
        }
    }

    std::printf("  %zu triangles, %zu slivers (under %.3g of the largest area)\n\n", tris,
                slivers, 1e-3);
    std::printf("  distance from the lattice, in voxels (0 = pinned ON a lattice plane):\n");
    report("sliver vertices", sliver_v);
    report("all other vertices", ordinary_v);

    // A comparison with no slivers, or with no ordinary triangles, says nothing.
    if (slivers == 0 || ordinary_v.empty()) {
        std::printf("\n  MEASURED NOTHING: the fixture produced no slivers to compare.\n");
        clay_brick_cache_destroy(cache);
        clay_document_destroy(d.doc);
        return 1;
    }

    std::sort(sliver_v.begin(), sliver_v.end());
    std::sort(ordinary_v.begin(), ordinary_v.end());
    const double sm = sliver_v[sliver_v.size() / 2];
    const double om = ordinary_v[ordinary_v.size() / 2];
    std::printf("\n  VERDICT\n");
    if (sm < om * 0.25)
        std::printf("    Sliver vertices ARE pinned to the lattice (median %.4f against %.4f).\n"
                    "    Clamping the marcher's t would move them off it, so option A is the\n"
                    "    right fix and the goldens refresh buys something.\n", sm, om);
    else
        std::printf("    Sliver vertices are NOT especially pinned (median %.4f against %.4f).\n"
                    "    They do not come from t landing at an endpoint, so clamping t would\n"
                    "    cost a goldens refresh, ~10 device cases and 618 host PNGs and fix\n"
                    "    NOTHING. Option A is refuted; #549 needs a different answer.\n", sm, om);

    clay_brick_cache_destroy(cache);
    clay_document_destroy(d.doc);
    return 0;
}
