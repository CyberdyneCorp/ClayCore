// DOES THE APPLICATION'S DEFAULT EXPORT WRITE A NON-MANIFOLD MESH?
// NOT a gated benchmark.
//
// Driven through the C ABI, deliberately, because that is what a host calls
// and because a C++-level reproduction of "the same thing" already disagreed
// with the host by 384 triangles at the input to decimation -- which is exactly
// the kind of gap that makes two correct measurements incomparable.
//
// The reported case: one layer, one node, a unit sphere, nothing sculpted;
// clay_document_mesh at voxel 0.02 with CLAY_MESHER_MARCHING and decimate
// on. Six of fifteen ratios came back with a non-manifold edge, non-monotone
// in the ratio -- 0.60 clean between a failing 0.55 and 0.65.

#include <cstdio>
#include <cstring>

#include "clay.h"

int main() {
    clay_document* doc = clay_document_create();
    if (!doc) { std::printf("FAIL: no document\n"); return 1; }
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) { std::printf("FAIL: layer\n"); return 1; }
    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!item) { std::printf("FAIL: item\n"); return 1; }
    clay_item_set_op(item, CLAY_OP_ADD);
    clay_node_id node = 0;
    const clay_result ar = clay_layer_add_item(doc, layer, item, &node);
    clay_item_destroy(item);
    if (ar != CLAY_OK) { std::printf("FAIL: add\n"); return 1; }

    auto report = [&](float ratio, int decimate) {
        clay_mesh_params p;
        std::memset(&p, 0, sizeof p);
        p.struct_size = sizeof p;
        p.voxel_size = 0.02f;
        p.mesher = CLAY_MESHER_MARCHING;
        p.decimate = decimate;
        p.decimate_ratio = ratio;
        clay_mesh* m = nullptr;
        if (clay_document_mesh(doc, &p, &m) != CLAY_OK || !m) {
            std::printf("  ratio %.2f  FAILED to mesh\n", static_cast<double>(ratio));
            return 0;
        }
        clay_validation_report v;
        std::memset(&v, 0, sizeof v);
        v.struct_size = sizeof v;
        clay_mesh_validation_report(m, 0, &v);
        const size_t tris = clay_mesh_index_count(m) / 3;
        clay_mesh_destroy(m);
        std::printf("  %-6s %.2f   tris %7zu   nm %3zu   bnd %3zu   euler %4lld   %s\n",
                    decimate ? "decim" : "raw", static_cast<double>(ratio), tris,
                    v.non_manifold_edges, v.boundary_edges,
                    static_cast<long long>(v.euler_characteristic),
                    v.non_manifold_edges ? "PINCHED" : "clean");
        return v.non_manifold_edges ? 1 : 0;
    };

    std::printf("unit sphere, one layer, voxel 0.02, CLAY_MESHER_MARCHING, through the C ABI\n\n");
    report(0.0f, 0);
    std::printf("\n");
    const float ratios[15] = {0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f, 0.55f, 0.60f,
                              0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f};
    int pinched = 0;
    for (float x : ratios) pinched += report(x, 1);
    std::printf("\n  %d of 15 pinched\n", pinched);
    clay_document_destroy(doc);
    return 0;
}
