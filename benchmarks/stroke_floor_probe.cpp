// WHAT DOES THE ENGINE CHARGE FOR ONE WHOLE STROKE ON A CLEAN SPHERE?
// NOT a gated benchmark. Probe for issue #531.
//
// THE NUMBER TO BEAT. #531 measured fourteen SDF brushes in a live
// ClaySpaceDesktop session, each on a clean one-item sphere, each isolated by
// undo. Every one reported `stalled: true`:
//
//     mask 63.6   crease 69.1   clay 72.6 ... move 203.4 ... snake-hook 244.6
//
// 16 ms is the 60 fps budget, so the FASTEST brush is 4x over it on the
// cheapest document that exists. The application's own phase split put
// `engine edit` at a median of 2.88 ms and `engine mesh` at 9.45 ms, which is
// about 12 ms of a 63 ms floor. The other 51 ms has never been attributed.
//
// THIS PROBE TAKES THE ENGINE'S HALF. It drives one complete Move stroke
// through the C ABI the way a host does -- begin, drag, commit, mark the
// dirty region, refill the bricks, mesh them -- and times each phase. If the
// engine's total lands near 12 ms against the application's 203.4 ms for the
// same brush on the same fixture, then the remaining time is host-side and
// #531's fix is not ours. If it lands near 203 ms, it is ours and this says
// which phase.
//
// WHY THE MESH PARAMS ARE SWEPT. `clay_brick_cache_mesh` evaluates gradient
// normals and per-vertex colours through PER-BRICK CULLED TAPES (its own
// header says so), so both are attributes of the FIELD rather than of the
// lattice and both cost a tape evaluation per vertex. A host that asks for
// them without needing them is buying that, and the difference between face
// normals and gradient normals is a knob a host owns. Measured rather than
// assumed, because assuming is how the last three of these went wrong.
//
// WHAT WOULD REFUTE THE "IT IS HOST-SIDE" READING: an engine total in the
// tens of milliseconds. The probe prints the total next to the application's
// figure for the same brush so the comparison cannot be dodged.

#include <chrono>
#include <cmath>
#include <cstddef>
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

constexpr int kDim = 8;
constexpr float kVoxel = 0.05f;
constexpr float kRadius = 0.40f;
constexpr int kSegments = 6;

// The application's own figure for a Move stroke on this exact fixture.
constexpr double kAppMoveMs = 203.4;
constexpr double kAppFastestMs = 63.6;  // mask, the cheapest of the fourteen
constexpr double kFrameBudgetMs = 16.0;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

// A PLAIN SPHERE -- #531's fixture, deliberately not a blockout. The whole
// point is that this is the case every density explanation says is free.
bool build(Doc* out) {
    clay_document* doc = clay_document_create();
    if (!doc) return false;
    clay_layer_id layer = 0;
    if (clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK) {
        clay_document_destroy(doc);
        return false;
    }
    const float r = 1.0f;
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!item) { clay_document_destroy(doc); return false; }
    clay_item_set_op(item, CLAY_OP_ADD);
    clay_node_id node = 0;
    const clay_result res = clay_layer_add_item(doc, layer, item, &node);
    clay_item_destroy(item);
    if (res != CLAY_OK) { clay_document_destroy(doc); return false; }
    out->doc = doc;
    out->layer = layer;
    return true;
}

long drain(clay_brick_cache* cache, const clay_document* against,
           std::vector<int32_t>* out_keys = nullptr) {
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
        if (out_keys)
            for (std::size_t i = 0; i < count; ++i) {
                out_keys->push_back(reqs[i].key[0]);
                out_keys->push_back(reqs[i].key[1]);
                out_keys->push_back(reqs[i].key[2]);
            }
        refilled += static_cast<long>(count);
        if (remaining == 0) break;
    }
    return refilled;
}

struct MeshResult {
    double ms = 0.0;
    uint64_t verts = 0, tris = 0;
};

MeshResult mesh_all(clay_brick_cache* cache, const clay_document* doc, int normals, int colors,
                    const int32_t* keys = nullptr, std::size_t key_count = 0) {
    MeshResult r;
    clay_brick_mesh_params p{};
    p.struct_size = sizeof(p);
    p.normals = normals;
    p.colors = colors;
    p.gradient_eps = 0.0f;
    clay_mesh* m = nullptr;
    const Clock::time_point t = Clock::now();
    const clay_result res =
        clay_brick_cache_mesh(cache, doc, &p, keys, key_count, nullptr, &m);
    r.ms = ms_since(t);
    if (res != CLAY_OK || !m) return r;
    r.verts = clay_mesh_vertex_count(m);
    r.tris = clay_mesh_index_count(m) / 3;
    clay_mesh_destroy(m);
    return r;
}

}  // namespace

int main() {
    std::printf("stroke_floor_probe: what the ENGINE charges for one whole stroke\n");
    std::printf("  clean one-item sphere r=1, voxel %.3f, move radius %.2f, %d segments\n",
                static_cast<double>(kVoxel), static_cast<double>(kRadius), kSegments);
    std::printf("  the application reports %.1f ms for this brush on this fixture (#531)\n\n",
                kAppMoveMs);

    Doc d;
    if (!build(&d)) { std::printf("FAIL: could not build the sphere\n"); return 1; }

    clay_brick_config bc{};
    bc.struct_size = sizeof(bc);
    bc.dim = kDim;
    bc.voxel_size = kVoxel;
    bc.band_voxels = 2;
    bc.memory_budget = 0;
    clay_brick_cache* cache = clay_brick_cache_create(&bc);
    if (!cache) { std::printf("FAIL: no cache\n"); clay_document_destroy(d.doc); return 1; }

    // Warm exactly as a host does before the first stroke: the surface is
    // already on screen when the artist presses.
    const float wmin[3] = {-2.0f, -2.0f, -2.0f};
    const float wmax[3] = {2.0f, 2.0f, 2.0f};
    clay_brick_cache_mark_dirty(cache, wmin, wmax);
    const long warm = drain(cache, d.doc);
    if (warm < 0) { std::printf("FAIL: warm refill\n"); return 1; }
    const MeshResult warm_mesh = mesh_all(cache, d.doc, CLAY_NORMAL_GRADIENT, 1);
    std::printf("  warm: %ld bricks, %llu verts, %llu tris\n\n", warm,
                (unsigned long long)warm_mesh.verts, (unsigned long long)warm_mesh.tris);

    // A mesh of nothing times nothing, and a table of zeroes reads as a win.
    if (warm_mesh.verts == 0) {
        std::printf("FAIL: the warm mesh is EMPTY -- this probe would measure nothing\n");
        clay_brick_cache_destroy(cache);
        clay_document_destroy(d.doc);
        return 1;
    }

    // ---- one whole stroke, phase by phase -------------------------------
    const float centre[3] = {0.0f, 0.0f, 1.0f};
    clay_move_params mp{};
    mp.struct_size = sizeof(mp);
    mp.radius = kRadius;

    const Clock::time_point t_begin = Clock::now();
    clay_sdf_move_tx* tx = clay_sdf_move_begin(d.doc, d.layer, centre, &mp, nullptr);
    const double begin_ms = ms_since(t_begin);
    if (!tx) { std::printf("FAIL: move_begin returned NULL\n"); return 1; }

    double update_ms = 0.0;
    clay_sculpt_dirty dirty{};
    dirty.struct_size = sizeof(dirty);
    const Clock::time_point t_up = Clock::now();
    for (int s = 1; s <= kSegments; ++s) {
        const float f = static_cast<float>(s) / static_cast<float>(kSegments);
        const float disp[3] = {0.0f, 0.0f, 0.12f * f};
        if (clay_sdf_move_update(tx, disp, &dirty) != CLAY_OK) {
            std::printf("FAIL: move_update at segment %d\n", s);
            return 1;
        }
    }
    update_ms = ms_since(t_up);

    const Clock::time_point t_commit = Clock::now();
    const clay_result cres = clay_sdf_move_commit(tx, nullptr);
    const double commit_ms = ms_since(t_commit);
    clay_sdf_move_destroy(tx);
    if (cres != CLAY_OK) { std::printf("FAIL: move_commit\n"); return 1; }

    const Clock::time_point t_refill = Clock::now();
    if (dirty.has_bounds)
        clay_brick_cache_mark_dirty(cache, dirty.bounds_min, dirty.bounds_max);
    std::vector<int32_t> dirty_keys;
    const long bricks = drain(cache, d.doc, &dirty_keys);
    const double refill_ms = ms_since(t_refill);
    if (bricks < 0) { std::printf("FAIL: refill\n"); return 1; }

    // A stroke that dirtied nothing has refilled nothing and meshed nothing,
    // and would report a very fast engine for the wrong reason.
    if (bricks == 0) {
        std::printf("FAIL: the stroke dirtied ZERO bricks -- it measured nothing\n");
        return 1;
    }

    // Over the stroke's OWN bricks. Meshing the whole cache here would compare
    // a whole-cache mesh against a whole-document one and call it a per-brick
    // cost, which is not the trade the host takes.
    mesh_all(cache, d.doc, CLAY_NORMAL_GRADIENT, 1, dirty_keys.data(),
             dirty_keys.size() / 3);  // warm; the first call pays for what the
                                      // allocator and caches have not seen
    const MeshResult m_grad = mesh_all(cache, d.doc, CLAY_NORMAL_GRADIENT, 1,
                                       dirty_keys.data(), dirty_keys.size() / 3);

    const double engine_total = begin_ms + update_ms + commit_ms + refill_ms + m_grad.ms;

    std::printf("  phase                         ms\n");
    std::printf("  move_begin               %7.3f   (\"THE ONLY traversal of the edit list\")\n",
                begin_ms);
    std::printf("  move_update x%-2d          %7.3f\n", kSegments, update_ms);
    std::printf("  move_commit              %7.3f\n", commit_ms);
    std::printf("  refill (%4ld bricks)     %7.3f\n", bricks, refill_ms);
    std::printf("  mesh  grad normals+cols  %7.3f   (%llu verts)\n", m_grad.ms,
                (unsigned long long)m_grad.verts);
    std::printf("  %-24s %7.3f  <== the engine's whole stroke\n", "TOTAL", engine_total);

    // ---- what the mesh params cost --------------------------------------
    // Both attributes are evaluated through per-brick culled tapes, so they
    // are a tape evaluation per vertex rather than a lattice walk. A host
    // that asks for them without needing them is buying that.
    std::printf("\n  mesh params, same bricks:\n");
    struct { const char* name; int normals; int colors; } sweep[] = {
        {"face normals, no colours", CLAY_NORMAL_FACE, 0},
        {"face normals, colours",    CLAY_NORMAL_FACE, 1},
        {"gradient normals, no col", CLAY_NORMAL_GRADIENT, 0},
        {"gradient normals+colours", CLAY_NORMAL_GRADIENT, 1},
    };
    for (const auto& s : sweep) {
        // Twice, second taken. The first version of this table reported
        // gradient normals as FASTER than face normals, which is impossible
        // and was the allocator warming up inside the first row.
        mesh_all(cache, d.doc, s.normals, s.colors, dirty_keys.data(), dirty_keys.size() / 3);
        const MeshResult r = mesh_all(cache, d.doc, s.normals, s.colors,
                                      dirty_keys.data(), dirty_keys.size() / 3);
        std::printf("    %-26s %7.3f ms  (%llu verts)\n", s.name, r.ms,
                    (unsigned long long)r.verts);
    }

    // THE CALL THE HOST IS FORCED ONTO. ClaySpaceDesktop re-meshes the whole
    // field through clay_document_mesh after every completed SDF stroke, and
    // its own comment says why: "The brick mesher can leave isolated dark pits
    // even in a fresh rebuild. The completed SDF uses the document mesher so
    // that the artifact cannot remain after a stroke."
    //
    // So this is not an overhead around our per-brick call, it is a DIFFERENT
    // call with a whole-document scope, and the host takes it knowingly as a
    // correctness-for-latency trade. That trade is only necessary while the
    // pits exist, which makes the cost ours rather than theirs.
    {
        clay_mesh_params dp{};
        dp.struct_size = sizeof(dp);
        dp.voxel_size = kVoxel;
        clay_mesh* warm_dm = nullptr;
        clay_document_mesh(d.doc, &dp, &warm_dm);
        if (warm_dm) clay_mesh_destroy(warm_dm);
        clay_mesh* dm = nullptr;
        const Clock::time_point t = Clock::now();
        const clay_result res = clay_document_mesh(d.doc, &dp, &dm);
        const double doc_ms = ms_since(t);
        const uint64_t dv = (res == CLAY_OK && dm) ? clay_mesh_vertex_count(dm) : 0;
        if (dm) clay_mesh_destroy(dm);
        std::printf("\n  the call the host is FORCED onto after every stroke:\n");
        std::printf("    clay_brick_cache_mesh   %7.3f ms  (%llu verts)  the stroke's %zu bricks\n",
                    m_grad.ms, (unsigned long long)m_grad.verts, dirty_keys.size() / 3);
        std::printf("    clay_document_mesh      %7.3f ms  (%llu verts)  WHOLE FIELD\n",
                    doc_ms, (unsigned long long)dv);
        if (m_grad.ms > 0.0)
            std::printf("    the host pays           %7.1fx  for a stroke's final re-mesh\n",
                        doc_ms / m_grad.ms);
        std::printf("    (app ledger: re-malha final 59.4 ms vs engine mesh median 9.45 ms)\n");
    }

    // A LIVE DRAG DOES THIS PER POINTER EVENT, not once at the end. The phase
    // table above is one refill-and-mesh cycle; an artist dragging across the
    // surface pays one per frame the pointer moves. Modelling that is the
    // difference between an honest comparison and a flattering one.
    {
        Doc d2;
        if (build(&d2)) {
            clay_brick_config bc2{};
            bc2.struct_size = sizeof(bc2);
            bc2.dim = kDim;
            bc2.voxel_size = kVoxel;
            bc2.band_voxels = 2;
            clay_brick_cache* c2 = clay_brick_cache_create(&bc2);
            if (c2) {
                clay_brick_cache_mark_dirty(c2, wmin, wmax);
                drain(c2, d2.doc);
                clay_move_params mp2{};
                mp2.struct_size = sizeof(mp2);
                mp2.radius = kRadius;
                const Clock::time_point t_all = Clock::now();
                clay_sdf_move_tx* tx2 =
                    clay_sdf_move_begin(d2.doc, d2.layer, centre, &mp2, nullptr);
                long frames = 0, total_bricks = 0;
                if (tx2) {
                    for (int s2 = 1; s2 <= kSegments; ++s2) {
                        const float f2 = static_cast<float>(s2) / static_cast<float>(kSegments);
                        const float dsp[3] = {0.0f, 0.0f, 0.12f * f2};
                        clay_sculpt_dirty dd{};
                        dd.struct_size = sizeof(dd);
                        if (clay_sdf_move_update(tx2, dsp, &dd) != CLAY_OK) break;
                        // what the viewport does every frame the pointer moves
                        if (dd.has_bounds)
                            clay_brick_cache_mark_dirty(c2, dd.bounds_min, dd.bounds_max);
                        std::vector<int32_t> k2;
                        const long b2 = drain(c2, d2.doc, &k2);
                        if (b2 > 0) {
                            total_bricks += b2;
                            mesh_all(c2, d2.doc, CLAY_NORMAL_GRADIENT, 1, k2.data(),
                                     k2.size() / 3);
                        }
                        ++frames;
                    }
                    clay_sdf_move_commit(tx2, nullptr);
                    clay_sdf_move_destroy(tx2);
                }
                const double live_ms = ms_since(t_all);
                std::printf("\n  a LIVE drag, refilling and meshing every frame:\n");
                std::printf("    %ld frames, %ld bricks refilled in total\n", frames,
                            total_bricks);
                std::printf("    engine, whole drag      %7.3f ms   (%.3f ms/frame)\n",
                            live_ms, frames ? live_ms / (double)frames : 0.0);
                clay_brick_cache_destroy(c2);
            }
            clay_document_destroy(d2.doc);
        }
    }

    std::printf("\n  against the application on the SAME fixture:\n");
    std::printf("    engine, this probe        %7.3f ms\n", engine_total);
    std::printf("    app, move                 %7.1f ms   (x%.1f this)\n", kAppMoveMs,
                kAppMoveMs / (engine_total > 0 ? engine_total : 1.0));
    std::printf("    app, fastest of fourteen  %7.1f ms   (x%.1f this)\n", kAppFastestMs,
                kAppFastestMs / (engine_total > 0 ? engine_total : 1.0));
    std::printf("    60 fps budget             %7.1f ms\n", kFrameBudgetMs);
    // WHAT A DEEP CHAIN COSTS EACH MESHER (their #110 landed and split it).
    //
    // ClaySpaceDesktop settles every completed SDF stroke through
    // clay_document_mesh, because the brick mesher leaves slivers (#549). With
    // per-call timing they measured that settle at voxel 0.02:
    //
    //     dabs   verts   engine mesh
    //        0   47024        14.59 ms
    //        8   48411        21.17
    //       24   48988        35.40
    //       48   49250        57.48
    //
    // 3.9x the time for 4.7% more vertices. So the document mesher's cost is
    // not its OUTPUT, it is what each sample costs -- and a sample costs more
    // when more grabs reach it.
    //
    // THE HYPOTHESIS THIS TESTS: the two meshers pay the chain differently
    // because only one of them evaluates the field. clay_brick_cache_mesh
    // marches the CACHED lattice, which was evaluated once at refill and is
    // just numbers by the time it is meshed. clay_document_mesh evaluates the
    // field densely at every grid point, uncalled, so every sample walks every
    // deformer. If that is right, the gap between them should OPEN with chain
    // depth -- and the pits are then costing the host the cull, not just a
    // second pass.
    //
    // Same dab counts as theirs so the two tables can be read together.
    std::printf("\n  what a deep chain costs each mesher (their dab counts):\n");
    std::printf("  %6s %10s %14s %14s %8s\n", "dabs", "bricks", "brick mesh ms",
                "document ms", "ratio");
    for (int dabs : {0, 8, 24, 48}) {
        Doc dd;
        if (!build(&dd)) break;
        clay_brick_config bc2{};
        bc2.struct_size = sizeof(bc2);
        bc2.dim = kDim;
        bc2.voxel_size = kVoxel;
        bc2.band_voxels = 2;
        clay_brick_cache* c3 = clay_brick_cache_create(&bc2);
        if (!c3) { clay_document_destroy(dd.doc); break; }

        // Dabs at fresh anchors around the cap, each its own gesture, so the
        // chain genuinely grows rather than coalescing into one warp.
        for (int i = 0; i < dabs; ++i) {
            const float a = 0.37f * static_cast<float>(i);
            const float ctr[3] = {std::cos(a) * 0.55f, std::sin(a) * 0.55f, 0.80f};
            const float dsp[3] = {0.0f, 0.0f, 0.05f};
            clay_move_params mp3{};
            mp3.struct_size = sizeof(mp3);
            mp3.radius = 0.40f;
            std::size_t applied = 0;
            if (clay_layer_move_surface(dd.doc, dd.layer, ctr, dsp, &mp3, &applied) != CLAY_OK)
                break;
        }
        clay_brick_cache_mark_dirty(c3, wmin, wmax);
        const long nb = drain(c3, dd.doc);

        const MeshResult warm_b = mesh_all(c3, dd.doc, CLAY_NORMAL_GRADIENT, 0);
        const MeshResult mb = mesh_all(c3, dd.doc, CLAY_NORMAL_GRADIENT, 0);

        clay_mesh_params dp2{};
        dp2.struct_size = sizeof(dp2);
        dp2.voxel_size = kVoxel;
        clay_mesh* warm_d = nullptr;
        clay_document_mesh(dd.doc, &dp2, &warm_d);
        if (warm_d) clay_mesh_destroy(warm_d);
        clay_mesh* dm2 = nullptr;
        const Clock::time_point t2 = Clock::now();
        const clay_result r2 = clay_document_mesh(dd.doc, &dp2, &dm2);
        const double dms = ms_since(t2);
        if (dm2) clay_mesh_destroy(dm2);

        // A mesher that produced nothing times nothing.
        if (r2 != CLAY_OK || mb.verts == 0 || nb <= 0) {
            std::printf("  %6d %10ld %14s %14s %8s  MEASURED NOTHING\n", dabs, nb, "-", "-", "-");
        } else {
            std::printf("  %6d %10ld %14.3f %14.3f %8.2fx\n", dabs, nb, mb.ms, dms,
                        mb.ms > 0.0 ? dms / mb.ms : 0.0);
        }
        (void)warm_b;
        clay_brick_cache_destroy(c3);
        clay_document_destroy(dd.doc);
    }
    std::printf("  A ratio that OPENS with depth says the document mesher pays the chain\n"
                "  per sample and the brick mesher does not -- so the pits (#549) cost a\n"
                "  host the cull, not merely a second pass.\n");

    std::printf("\n  An engine total well under the application's figure puts the\n"
                "  remainder host-side. An engine total near it makes #531 ours.\n");

    clay_brick_cache_destroy(cache);
    clay_document_destroy(d.doc);
    return 0;
}
