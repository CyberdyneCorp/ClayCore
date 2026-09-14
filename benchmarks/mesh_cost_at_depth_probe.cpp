// WHERE BRICK MESHING SPENDS ITS TIME, AS A DEFORMER CHAIN ACCUMULATES.
// NOT a gated benchmark.
//
// #531 asks where a stroke's cost goes and establishes that meshing is the
// answer. What it does NOT establish is where inside meshing, and the four
// hypotheses it already refutes are all about the EDIT path rather than this
// one. So this splits the mesh call itself.
//
// THE REASON TO EXPECT A SPLIT. clay_brick_cache_mesh does two different kinds
// of field work:
//
//   the grid fill   one evaluation per LATTICE SAMPLE -- dim^3 per brick, 512
//                   at dim 8 -- and it is already paid by the refill, not here
//   attributes      gradient normals and colours go through eval_points_batch
//                   on ONE CULLED TAPE PER BRICK GROUP, and a gradient is a
//                   tetrahedron tap: about four evaluations per VERTEX
//
// A brick with a surface through it holds a few hundred vertices, so at four
// taps each the attribute pass can rival or exceed the 512-sample fill. And
// EVERY ONE of those evaluations walks the surviving deformer chain -- the
// mechanism measured on the other side of this investigation, where the engine
// went from 11.6% of a stroke at chain depth 0 to 34.2% at depth 48.
//
// At depth 0 this split does not show: a previous run of stroke_floor_probe put
// all four normal/colour combinations within 1.93-2.41 ms of each other, which
// reads as "mesh params are free". THAT READING IS ONLY TRUE AT DEPTH 0, and
// this probe exists to find out where it stops being true.
//
// WHAT WOULD MAKE IT ACTIONABLE. The brick cache already holds dim^3 samples of
// the field. If gradients dominate at depth, they could be finite-differenced
// from the STORED lattice at zero additional field evaluations, at some cost in
// accuracy near the band edge. That is a real option with a real price, and it
// is only worth designing if the number says so.
//
// Exits non-zero if the fixture stops discriminating -- if the chain does not
// grow, or if no combination separates from another at the deepest chain, which
// would mean this measures nothing.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

constexpr float kVoxel = 0.02f;   // ClaySpaceDesktop's own, not this probe's convenience
constexpr int kDim = 8;           // their measured optimum; 16 meshes more cells overall
constexpr int kBand = 3;
constexpr float kRadius = 0.35f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

bool build(Doc* d) {
    d->doc = clay_document_create();
    if (!d->doc) return false;
    if (clay_add_sdf_layer(d->doc, "form", &d->layer) != CLAY_OK) return false;
    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!item) return false;
    clay_item_set_op(item, CLAY_OP_ADD);
    clay_node_id node = 0;
    const clay_result res = clay_layer_add_item(d->doc, d->layer, item, &node);
    clay_item_destroy(item);
    return res == CLAY_OK;
}

// Stamp n grabs, each at its own centre so they STACK rather than coalesce:
// continues_gesture folds a repeat at a bit-identical centre, and a probe that
// dabbed one spot would report a chain of 1 and a beautifully flat table.
bool prestamp(const Doc& d, int n) {
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(n + 1);
        const float ang = 2.4f * static_cast<float>(i);
        const float ctr[3] = {0.35f * std::cos(ang) * t, 0.35f * std::sin(ang) * t,
                              std::sqrt(std::max(0.05f, 1.0f - 0.1225f * t * t))};
        const float dsp[3] = {0.0f, 0.0f, 0.012f};
        clay_move_params mp{};
        mp.struct_size = sizeof mp;
        mp.radius = kRadius;
        mp.front_only = 1;
        size_t applied = 0;
        if (clay_layer_move_surface(d.doc, d.layer, ctr, dsp, &mp, &applied) != CLAY_OK)
            return false;
    }
    return true;
}

long drain(clay_brick_cache* cache, const clay_document* against) {
    long refilled = 0;
    std::vector<clay_brick_request> reqs(4096);
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
        refilled += static_cast<long>(accepted);
        if (remaining == 0) break;
    }
    return refilled;
}

struct Row {
    double none_ms = 0, face_ms = 0, grad_ms = 0, grad_col_ms = 0;
    unsigned long long verts = 0;
    long bricks = 0;
};

double mesh_ms(clay_brick_cache* cache, const clay_document* doc, int normals, int colors,
               unsigned long long* out_verts) {
    clay_brick_mesh_params p{};
    p.struct_size = sizeof p;
    p.normals = normals;
    p.colors = colors;
    clay_mesh* m = nullptr;
    const Clock::time_point t = Clock::now();
    const clay_result r = clay_brick_cache_mesh(cache, doc, &p, nullptr, 0, nullptr, &m);
    const double ms = ms_since(t);
    if (r != CLAY_OK || !m) return -1.0;
    if (out_verts) *out_verts = clay_mesh_vertex_count(m);
    clay_mesh_destroy(m);
    return ms;
}

bool row_at(int depth, Row* out) {
    Doc d;
    if (!build(&d)) return false;
    if (depth > 0 && !prestamp(d, depth)) return false;

    clay_brick_config bc{};
    bc.struct_size = sizeof bc;
    bc.dim = kDim;
    bc.voxel_size = kVoxel;
    bc.band_voxels = kBand;
    clay_brick_cache* cache = clay_brick_cache_create(&bc);
    if (!cache) return false;
    const float lo[3] = {-1.3f, -1.3f, -1.3f}, hi[3] = {1.3f, 1.3f, 1.3f};
    clay_brick_cache_mark_dirty(cache, lo, hi);
    out->bricks = drain(cache, d.doc);
    if (out->bricks < 0) return false;

    // Warm once: the first mesh of a cache pays allocations the rest do not.
    mesh_ms(cache, d.doc, CLAY_NORMAL_GRADIENT, 1, nullptr);

    const int kReps = 3;
    for (int i = 0; i < kReps; ++i) {
        out->none_ms += mesh_ms(cache, d.doc, CLAY_NORMAL_NONE, 0, nullptr);
        out->face_ms += mesh_ms(cache, d.doc, CLAY_NORMAL_FACE, 0, nullptr);
        out->grad_ms += mesh_ms(cache, d.doc, CLAY_NORMAL_GRADIENT, 0, &out->verts);
        out->grad_col_ms += mesh_ms(cache, d.doc, CLAY_NORMAL_GRADIENT, 1, nullptr);
    }
    out->none_ms /= kReps; out->face_ms /= kReps;
    out->grad_ms /= kReps; out->grad_col_ms /= kReps;

    clay_brick_cache_destroy(cache);
    clay_document_destroy(d.doc);
    return true;
}

}  // namespace

int main() {
    std::printf("clay_brick_cache_mesh split by attribute work, as the chain grows.\n");
    std::printf("unit sphere, voxel %.3f, dim %d, band %d -- ClaySpaceDesktop's own config.\n\n",
                static_cast<double>(kVoxel), kDim, kBand);
    std::printf("  chain  bricks    verts      none     face  gradient  grad+col"
                "   grad/none\n");

    const int depths[5] = {0, 4, 12, 32, 48};
    Row rows[5];
    for (int i = 0; i < 5; ++i) {
        if (!row_at(depths[i], &rows[i])) {
            std::printf("FAIL: could not build the row at depth %d\n", depths[i]);
            return 1;
        }
        const Row& r = rows[i];
        std::printf("  %5d  %6ld  %7llu  %7.3f  %7.3f  %8.3f  %8.3f   %8.2fx\n",
                    depths[i], r.bricks, r.verts, r.none_ms, r.face_ms, r.grad_ms,
                    r.grad_col_ms, r.none_ms > 0 ? r.grad_ms / r.none_ms : 0.0);
    }

    const Row& first = rows[0];
    const Row& last = rows[4];
    std::printf("\n  depth 0 -> 48:\n");
    std::printf("    marching only (no attributes)   %7.3f -> %7.3f ms   %.2fx\n",
                first.none_ms, last.none_ms, first.none_ms > 0 ? last.none_ms / first.none_ms : 0.0);
    std::printf("    gradient normals               %7.3f -> %7.3f ms   %.2fx\n",
                first.grad_ms, last.grad_ms, first.grad_ms > 0 ? last.grad_ms / first.grad_ms : 0.0);
    std::printf("    the attribute pass alone       %7.3f -> %7.3f ms\n",
                first.grad_ms - first.none_ms, last.grad_ms - last.none_ms);

    // -- the invariants -----------------------------------------------------
    if (last.verts == 0 || last.bricks <= 0) {
        std::printf("\nFAIL: the deepest row meshed nothing, so every timing above is of nothing.\n");
        return 1;
    }
    if (std::fabs(last.grad_ms - last.none_ms) < 0.05) {
        std::printf("\nFAIL: at chain 48 the attribute pass is indistinguishable from the\n"
                    "      march, so this probe cannot separate what it exists to separate.\n");
        return 1;
    }
    std::printf("\n  The attribute pass is field work and the march is not, so the two\n"
                "  columns diverge exactly as far as the chain makes a field sample cost.\n");
    return 0;
}
