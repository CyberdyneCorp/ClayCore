// DOES THE APPLICATION'S DEFAULT EXPORT WRITE A NON-MANIFOLD MESH?
// NOT a gated benchmark.
//
// Driven through the C ABI, deliberately, because that is what a host calls
// and because a C++-level reproduction of "the same thing" already disagreed
// with the host by 384 triangles at the input to decimation -- which is exactly
// the kind of gap that makes two correct measurements incomparable.
//
// The reported case: one layer, one node, a unit sphere, nothing sculpted;
// clay_document_mesh at voxel 0.02 with CLAY_MESHER_MARCHING and decimate on.
//
// Swept at 0.01 rather than the 0.05 the report used, because the coarse grid
// badly understated it. 31 of 76 ratios from 0.20 to 0.95 come back
// non-manifold, in exactly TWO CONTIGUOUS BANDS:
//
//     0.44 .. 0.59   16 ratios
//     0.60           CLEAN -- a single isolated ratio
//     0.61 .. 0.75   15 ratios
//
// Everything below 0.44 and above 0.75 is clean. A coarse sweep landing on
// 0.60 reads as "non-monotone noise"; it is one lucky point in a 32-wide
// region, and the default export ratio of 0.5 sits inside the first band.
//
// WITHIN A BAND IT IS THE SAME EDGE. Printing the incident positions shows
// 0.65/0.70/0.75 all failing at (-0.379, -0.920, +0.100) and 0.50/0.55 both at
// (+0.417, -0.143, +0.897). Every offending vertex lies exactly on the sphere
// (|p| = 1.0000) and every bad edge is SHORTER THAN ONE VOXEL -- 0.0037 to
// 0.036 against a voxel of 0.02.
//
// Since a higher ratio means FEWER collapses, the sequence reads: clean, a
// pinch appears at 0.75 and survives down to 0.61, is resolved at 0.60, a
// different one appears at 0.59 and survives to 0.44, resolved by 0.43. A
// pinch is CREATED by one collapse and REMOVED by a later one -- it is a
// transient state of the simplification, not a property of the target size.
//
// AND THEY ARE FLAT, which is why this prints the radial layout. The four
// triangles at each bad edge sit within about two degrees of each other, with
// the fourth about 180 degrees away:
//
//     0.50, 0.55     +0.0   +179.9    -0.0    -1.7
//     0.65           +0.0   -179.0  -179.4  -179.4
//     0.70, 0.75     +0.0   +178.9    -0.6    -0.6
//
// That is the same configuration that killed the repair pass on #567's crossed
// tori -- which pair of triangles belongs to which sheet is a rounding
// tie-break, and the two answers leave different genus. So repair is refuted
// on a SECOND, independent fixture, which is what it needed: the first
// refutation generalised from one.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
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

        // WHERE the bad edge sits, not just that there is one. If it lands in
        // the same place across ratios it is a feature of the input mesh that
        // survives to certain collapse depths, not a random unlucky collapse.
        if (v.non_manifold_edges) {
            const float* pos = clay_mesh_positions(m);
            const std::uint32_t* idx = clay_mesh_indices(m);
            std::map<std::uint64_t, int> inc;
            for (size_t t = 0; t < tris; ++t)
                for (int e = 0; e < 3; ++e) {
                    const std::uint32_t a = idx[t * 3 + e], b = idx[t * 3 + (e + 1) % 3];
                    const std::uint64_t k = a < b ? (static_cast<std::uint64_t>(a) << 32) | b
                                                  : (static_cast<std::uint64_t>(b) << 32) | a;
                    ++inc[k];
                }
            for (const auto& kv : inc) {
                if (kv.second <= 2) continue;
                const std::uint32_t a = static_cast<std::uint32_t>(kv.first >> 32);
                const std::uint32_t b = static_cast<std::uint32_t>(kv.first & 0xffffffffu);
                const float* pa = pos + a * 3;
                const float* pb = pos + b * 3;
                const double len = std::sqrt((pa[0]-pb[0])*(pa[0]-pb[0]) +
                                             (pa[1]-pb[1])*(pa[1]-pb[1]) +
                                             (pa[2]-pb[2])*(pa[2]-pb[2]));
                const double from_origin =
                    std::sqrt(pa[0]*pa[0] + pa[1]*pa[1] + pa[2]*pa[2]);
                std::printf("        edge x%d  a=(%+.4f %+.4f %+.4f)  |a|=%.4f  len=%.5f\n",
                            kv.second, static_cast<double>(pa[0]), static_cast<double>(pa[1]),
                            static_cast<double>(pa[2]), from_origin, len);
                // ARE THESE PINCHES FLAT? That is what killed the repair pass on
                // #567's fixture -- four triangles at 0, 178.7, 178.7 and -177.3
                // degrees, where which pair belongs to which sheet is a rounding
                // tie-break. If these are well separated, repair is viable here
                // even though it was not there.
                double ax[3] = {(pb[0]-pa[0])/len, (pb[1]-pa[1])/len, (pb[2]-pa[2])/len};
                double ref[3] = {0,0,0}; bool have_ref = false;
                std::printf("          radial:");
                for (size_t ft = 0; ft < tris; ++ft) {
                    std::uint32_t tri[3] = {idx[ft*3], idx[ft*3+1], idx[ft*3+2]};
                    bool ha=false, hb=false; std::uint32_t apex=0;
                    for (int k=0;k<3;++k){ if(tri[k]==a) ha=true; else if(tri[k]==b) hb=true; else apex=tri[k]; }
                    if (!ha || !hb) continue;
                    const float* pc = pos + apex*3;
                    double d[3] = {pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2]};
                    const double dot = d[0]*ax[0]+d[1]*ax[1]+d[2]*ax[2];
                    for (int k=0;k<3;++k) d[k] -= dot*ax[k];
                    const double dl = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
                    if (dl < 1e-12) { std::printf("  [apex on axis]"); continue; }
                    for (int k=0;k<3;++k) d[k] /= dl;
                    if (!have_ref) { for(int k=0;k<3;++k) ref[k]=d[k]; have_ref=true; }
                    double bino[3] = {ax[1]*ref[2]-ax[2]*ref[1], ax[2]*ref[0]-ax[0]*ref[2],
                                      ax[0]*ref[1]-ax[1]*ref[0]};
                    const double cx = d[0]*ref[0]+d[1]*ref[1]+d[2]*ref[2];
                    const double cy = d[0]*bino[0]+d[1]*bino[1]+d[2]*bino[2];
                    std::printf("  %+7.1fdeg", std::atan2(cy, cx) * 57.29578);
                }
                std::printf("\n");
            }
        }
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
    int pinched = 0, total = 0;
    for (int i = 20; i <= 95; ++i) {
        pinched += report(static_cast<float>(i) / 100.0f, 1);
        ++total;
    }
    std::printf("\n  %d of %d pinched\n", pinched, total);
    clay_document_destroy(doc);
    return 0;
}
