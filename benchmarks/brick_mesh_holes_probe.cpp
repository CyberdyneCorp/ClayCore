// DOES clay_brick_cache_mesh LEAVE HOLES THAT clay_document_mesh DOES NOT?
// NOT a gated benchmark. Reproduction attempt for the pits behind issue #531.
//
// WHERE THIS COMES FROM. ClaySpaceDesktop re-meshes the WHOLE field through
// `clay_document_mesh` after every completed SDF stroke, and its own comment
// says why: "The brick mesher can leave isolated dark pits even in a fresh
// rebuild. The completed SDF uses the document mesher so that the artifact
// cannot remain after a stroke." They carry an ignored test for it --
// `a_long_mixed_session_leaves_no_holes_or_specks` -- whose oracle renders the
// form and counts background pixels locally surrounded by surface. They cannot
// run it: offscreen rendering panics inside wgpu in that environment.
//
// SO THIS USES A SHARPER ORACLE THAT NEEDS NO RENDERER. A pit is a missing
// cell, and a missing cell in a closed surface leaves BOUNDARY EDGES -- edges
// incident to exactly one triangle. `clay_mesh_validation_report` already
// counts them, along with the Euler characteristic, which a hole moves whether
// or not it is visible from the camera that happened to be pointed at it.
//
// A pixel oracle can miss a pit facing away from the camera. This cannot.
//
// WHAT THE ANSWER MEANS EITHER WAY:
//   brick boundary_edges > 0 while document is 0  -> the defect is ours, here
//   both 0                                        -> not reproduced this way;
//                                                    the pits are elsewhere,
//                                                    and that is worth knowing
//                                                    before anyone "fixes" it
//   both > 0                                      -> shared, and the document
//                                                    mesher is not the cure
//                                                    the host believes it is

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <vector>

#include "clay.h"

namespace {

constexpr int kDim = 8;
constexpr float kVoxel = 0.05f;

struct Doc {
    clay_document* doc = nullptr;
    clay_layer_id layer = 0;
};

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

struct Report {
    bool ok = false;
    std::size_t verts = 0, tris = 0, boundary = 0, nonmanifold = 0, degenerate = 0;
    int watertight = 0, manifold = 0;
    long long euler = 0;
};

Report validate(clay_mesh* m) {
    Report r;
    if (!m) return r;
    clay_validation_report v{};
    v.struct_size = sizeof(v);
    if (clay_mesh_validation_report(m, 0, &v) != CLAY_OK) return r;
    r.ok = true;
    r.verts = v.vertices;
    r.tris = v.triangles;
    r.boundary = v.boundary_edges;
    r.nonmanifold = v.non_manifold_edges;
    r.degenerate = v.degenerate_triangles;
    r.watertight = v.watertight;
    r.manifold = v.manifold;
    r.euler = v.euler_characteristic;
    return r;
}

Report brick_mesh(clay_brick_cache* cache, const clay_document* doc) {
    clay_brick_mesh_params p{};
    p.struct_size = sizeof(p);
    p.normals = CLAY_NORMAL_GRADIENT;
    p.colors = 0;
    clay_mesh* m = nullptr;
    if (clay_brick_cache_mesh(cache, doc, &p, nullptr, 0, nullptr, &m) != CLAY_OK || !m)
        return Report{};
    const Report r = validate(m);
    clay_mesh_destroy(m);
    return r;
}

Report doc_mesh(const clay_document* doc) {
    clay_mesh_params p{};
    p.struct_size = sizeof(p);
    p.voxel_size = kVoxel;
    clay_mesh* m = nullptr;
    if (clay_document_mesh(doc, &p, &m) != CLAY_OK || !m) return Report{};
    const Report r = validate(m);
    clay_mesh_destroy(m);
    return r;
}

void row(const char* label, const Report& r) {
    if (!r.ok) { std::printf("  %-26s  FAILED TO MESH OR VALIDATE\n", label); return; }
    std::printf("  %-26s %8zu %8zu %10zu %8zu %6s %8lld\n", label, r.verts, r.tris, r.boundary,
                r.nonmanifold, r.watertight ? "yes" : "NO", r.euler);
}

// One Move drag, anchored at `centre`, pulled along `dir`. The host's fixture
// is a long MIXED session -- tendrils, then dabs, then polish -- with a
// geometry sync after every stroke, which is what makes it a session rather
// than a batch. This reproduces the shape of that: many separate gestures,
// each followed by a refill.
bool drag(Doc* d, clay_brick_cache* cache, const float centre[3], const float dir[3],
          float radius, float reach) {
    clay_move_params mp{};
    mp.struct_size = sizeof(mp);
    mp.radius = radius;
    clay_sdf_move_tx* tx = clay_sdf_move_begin(d->doc, d->layer, centre, &mp, nullptr);
    if (!tx) return false;
    clay_sculpt_dirty dirty{};
    dirty.struct_size = sizeof(dirty);
    for (int s = 1; s <= 4; ++s) {
        const float f = reach * static_cast<float>(s) / 4.0f;
        const float disp[3] = {dir[0] * f, dir[1] * f, dir[2] * f};
        if (clay_sdf_move_update(tx, disp, &dirty) != CLAY_OK) {
            clay_sdf_move_destroy(tx);
            return false;
        }
    }
    const bool ok = clay_sdf_move_commit(tx, nullptr) == CLAY_OK;
    clay_sdf_move_destroy(tx);
    if (!ok) return false;
    if (dirty.has_bounds) clay_brick_cache_mark_dirty(cache, dirty.bounds_min, dirty.bounds_max);
    return drain(cache, d->doc) >= 0;
}


// THE NORMALS, WHICH IS WHERE "DARK" POINTS.
//
// A missing triangle shows as BACKGROUND. A DARK speck on a surface that is
// present is a wrong NORMAL -- and the brick mesher evaluates gradient normals
// through PER-BRICK CULLED TAPES, which is exactly the machinery that decides
// a warp cannot reach a region and drops it. A vertex near a brick boundary
// whose tape is missing a warp gets a gradient from a field that is not the
// one it sits on, and renders dark.
//
// Checked without needing vertex correspondence between the two meshers: take
// the brick mesh's OWN vertices and its OWN stored normals, ask the document
// for the true field gradient at those same positions, and measure the angle.
// clay_layer_eval_gradients returns unit surface normals (the tetrahedron
// trick), so the two are directly comparable.
struct NormalCheck {
    bool ok = false;
    std::size_t checked = 0, bad = 0;
    double worst_deg = 0.0;
};

NormalCheck check_normals(clay_brick_cache* cache, const Doc& d, double tol_deg) {
    NormalCheck nc;
    clay_brick_mesh_params p{};
    p.struct_size = sizeof(p);
    p.normals = CLAY_NORMAL_GRADIENT;
    p.colors = 0;
    clay_mesh* m = nullptr;
    if (clay_brick_cache_mesh(cache, d.doc, &p, nullptr, 0, nullptr, &m) != CLAY_OK || !m)
        return nc;
    const std::size_t n = clay_mesh_vertex_count(m);
    if (n == 0) { clay_mesh_destroy(m); return nc; }

    struct V { float px, py, pz, nx, ny, nz; };
    std::vector<V> verts(n);
    clay_vertex_layout lay{};
    lay.struct_size = sizeof(lay);
    lay.stride = sizeof(V);
    lay.position_offset = 0;
    lay.normal_offset = static_cast<int32_t>(offsetof(V, nx));
    lay.color_offset = -1;
    lay.uv_offset = -1;
    const clay_result cr =
        clay_mesh_copy_vertices(m, &lay, verts.data(), verts.size() * sizeof(V));
    clay_mesh_destroy(m);
    if (cr != CLAY_OK) return nc;

    std::vector<float> pts(n * 3), grads(n * 3, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        pts[i * 3] = verts[i].px;
        pts[i * 3 + 1] = verts[i].py;
        pts[i * 3 + 2] = verts[i].pz;
    }
    if (clay_layer_eval_gradients(d.doc, d.layer, "cpu", pts.data(), n, grads.data()) != CLAY_OK)
        return nc;

    nc.ok = true;
    for (std::size_t i = 0; i < n; ++i) {
        const float* g = &grads[i * 3];
        const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        const V& v = verts[i];
        const float nl = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (!(gl > 1e-6f) || !(nl > 1e-6f)) continue;  // nothing to compare
        double dot = (double)(g[0] * v.nx + g[1] * v.ny + g[2] * v.nz) / ((double)gl * nl);
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;
        const double deg = std::acos(dot) * 57.29577951308232;
        ++nc.checked;
        if (deg > nc.worst_deg) nc.worst_deg = deg;
        if (deg > tol_deg) ++nc.bad;
    }
    return nc;
}

}  // namespace

int main() {
    std::printf("brick_mesh_holes_probe: does the per-brick mesher leave holes?\n");
    std::printf("  oracle: boundary edges (an edge on exactly ONE triangle) and the Euler\n");
    std::printf("  characteristic, from clay_mesh_validation_report. No renderer involved.\n\n");

    Doc d;
    if (!build(&d)) { std::printf("FAIL: fixture\n"); return 1; }
    clay_brick_config bc{};
    bc.struct_size = sizeof(bc);
    bc.dim = kDim;
    bc.voxel_size = kVoxel;
    bc.band_voxels = 2;
    clay_brick_cache* cache = clay_brick_cache_create(&bc);
    if (!cache) { std::printf("FAIL: cache\n"); return 1; }

    const float wmin[3] = {-3.0f, -3.0f, -3.0f};
    const float wmax[3] = {3.0f, 3.0f, 3.0f};
    clay_brick_cache_mark_dirty(cache, wmin, wmax);
    if (drain(cache, d.doc) <= 0) { std::printf("FAIL: warm refill produced no bricks\n"); return 1; }

    std::printf("  %-26s %8s %8s %10s %8s %6s %8s\n", "stage", "verts", "tris", "boundary",
                "nonmani", "tight", "euler");

    const Report b0 = brick_mesh(cache, d.doc);
    const Report d0 = doc_mesh(d.doc);
    row("plain sphere, brick", b0);
    row("plain sphere, document", d0);

    // A mesh of nothing is watertight for the wrong reason.
    if (!b0.ok || b0.tris == 0) {
        std::printf("\nFAIL: the brick mesh is empty; this probe would measure nothing\n");
        return 1;
    }

    // -- the long mixed session ------------------------------------------
    // Eight tendrils pulled outward on a ring, then a carve/build ring of
    // dabs, then a pass of shallow ones -- each its own gesture with a refill
    // after it, following the order the host's own reproduction describes.
    int strokes = 0;
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const float c[3] = {std::cos(a) * 0.95f, std::sin(a) * 0.95f, 0.0f};
        const float dir[3] = {std::cos(a), std::sin(a), 0.0f};
        if (!drag(&d, cache, c, dir, 0.30f, 0.45f)) { std::printf("FAIL: tendril %d\n", i); return 1; }
        ++strokes;
    }
    const Report b1 = brick_mesh(cache, d.doc);
    const Report d1 = doc_mesh(d.doc);
    row("8 tendrils, brick", b1);
    row("8 tendrils, document", d1);

    for (int i = 0; i < 40; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 40.0f;
        const float c[3] = {std::cos(a) * 0.85f, std::sin(a) * 0.85f, 0.25f};
        const float s = (i % 3 == 0) ? -1.0f : 1.0f;  // every third carves
        const float dir[3] = {std::cos(a) * s, std::sin(a) * s, 0.0f};
        if (!drag(&d, cache, c, dir, 0.18f, 0.10f)) { std::printf("FAIL: dab %d\n", i); return 1; }
        ++strokes;
    }
    const Report b2 = brick_mesh(cache, d.doc);
    const Report d2 = doc_mesh(d.doc);
    row("40 dabs, brick", b2);
    row("40 dabs, document", d2);

    for (int i = 0; i < 12; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 12.0f;
        const float c[3] = {std::cos(a) * 0.60f, std::sin(a) * 0.60f, -0.30f};
        const float dir[3] = {0.0f, 0.0f, 1.0f};
        if (!drag(&d, cache, c, dir, 0.22f, 0.04f)) { std::printf("FAIL: polish %d\n", i); return 1; }
        ++strokes;
    }
    const Report b3 = brick_mesh(cache, d.doc);
    const Report d3 = doc_mesh(d.doc);
    row("12 polish, brick", b3);
    row("12 polish, document", d3);

    std::printf("\n  %d strokes, each with its own refill.\n", strokes);

    // "DARK" is a normal, not a hole. Checked after the full session, when the
    // cull has had every chance to drop a warp it should have kept.
    const NormalCheck nc = check_normals(cache, d, 5.0);
    std::printf("\n  NORMALS on the brick mesh, against the field's own gradient:\n");
    if (!nc.ok) {
        std::printf("    could not be checked\n");
    } else {
        std::printf("    %zu vertices compared, %zu off by more than 5 deg, worst %.2f deg\n",
                    nc.checked, nc.bad, nc.worst_deg);
        if (nc.checked == 0) {
            std::printf("    NOTHING WAS COMPARED -- this line measures nothing\n");
        } else if (nc.worst_deg < 1e-4) {
            // READ THIS BEFORE TRUSTING THE ZERO. A worst case of exactly zero
            // is what a TAUTOLOGY looks like -- two names for one computation.
            // It is believable here only because the two sides are different
            // code paths on purpose: the mesh's normals come from PER-BRICK
            // CULLED tapes and this gradient comes from the WHOLE-DOCUMENT
            // tape. Issue #452 asserts those agree bit-for-bit inside the
            // region, so agreement is the expected result and this check
            // mostly re-confirms #452 rather than saying anything new about
            // the specks. It would catch a cull that dropped a warp it should
            // have kept, and it did not.
            std::printf("    NOTE: exactly zero. The two sides ARE different paths (culled\n"
                        "    per-brick tapes vs the whole-document tape), so this is #452's\n"
                        "    bit-identity holding rather than a tautology -- but it is weak\n"
                        "    evidence about the specks, not strong.\n");
        }
        else if (nc.bad > 0)
            std::printf("    ^ a vertex whose stored normal disagrees with the field it sits\n"
                        "      on renders DARK. This is the shape the host's specks would take.\n");
    }
    std::printf("\n  VERDICT\n");
    const bool brick_holed = b3.boundary > 0 || b1.boundary > 0 || b2.boundary > 0;
    const bool doc_holed = d3.boundary > 0 || d1.boundary > 0 || d2.boundary > 0;
    if (brick_holed && !doc_holed)
        std::printf("    REPRODUCED: the brick mesher leaves boundary edges where the\n"
                    "    document mesher leaves none. The host's workaround is justified.\n");
    else if (brick_holed && doc_holed)
        std::printf("    BOTH leave boundary edges. The document mesher is not the cure the\n"
                    "    host believes it is, and the trade it is paying buys less than it costs.\n");
    else if (!brick_holed && !doc_holed)
        std::printf("    NOT REPRODUCED this way: both meshes are watertight at every stage.\n"
                    "    The pits are not a hole in the triangle set on this fixture -- look at\n"
                    "    the LOD/mip path, colour, or normals before changing the mesher.\n");
    else
        std::printf("    Only the DOCUMENT mesher leaves boundary edges, which inverts the\n"
                    "    host's assumption entirely.\n");

    clay_brick_cache_destroy(cache);
    clay_document_destroy(d.doc);
    return 0;
}
