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
    std::size_t slivers = 0;
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
    r.slivers = v.sliver_triangles;
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
    std::printf("  %-26s %8zu %8zu %9zu %7zu %8zu %6s\n", label, r.verts, r.tris, r.boundary,
                r.degenerate, r.slivers, r.watertight ? "yes" : "NO");
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

NormalCheck check_normals(clay_brick_cache* cache, const Doc& d, double tol_deg,
                          int normal_mode = CLAY_NORMAL_GRADIENT) {
    NormalCheck nc;
    clay_brick_mesh_params p{};
    p.struct_size = sizeof(p);
    p.normals = normal_mode;
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

    // SLIVERS ARE THE COLUMN THAT MATTERS, and it is here because the host
    // found the cause in their own source after this probe had already cleared
    // the geometry: "face normals make the cache's degenerate preview
    // triangles appear as pits across an otherwise smooth form"
    // (clayspace-app geometry.rs:306).
    //
    // A sliver is legal geometry -- clay_validation_report calls it
    // informational and deliberately leaves it out of `clean` -- but its FACE
    // NORMAL is a cross product of near-parallel edges, which is numerically
    // garbage. Shaded flat, that is a black speck on a smooth form. It needs
    // no missing cell and no wrong gradient, which is exactly why this probe's
    // first two oracles came back clean.
    std::printf("  %-26s %8s %8s %9s %7s %8s %6s\n", "stage", "verts", "tris", "boundary",
                "degen", "SLIVERS", "tight");

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
    const NormalCheck nc = check_normals(cache, d, 5.0, CLAY_NORMAL_GRADIENT);

    // THE TEST THAT DECIDES WHETHER THE SLIVERS CAN SHADE BLACK THROUGH THIS
    // API AT ALL. `compute_face_normals` (marching.cpp:961) accumulates the
    // UNNORMALISED cross product per vertex -- whose length is twice the
    // triangle's area -- and normalises once at the end. That is area
    // weighting with no separate term, so a near-zero-area triangle
    // contributes near-zero to its vertices.
    //
    // If that holds, CLAY_NORMAL_FACE is already a SMOOTH vertex normal rather
    // than a flat per-triangle one, a sliver cannot poison it, and a host
    // asking for FACE at any level already gets shading a sliver cannot dent.
    // Measured against the field's own gradient on a smooth form: flat
    // per-triangle normals would disagree wildly; area-weighted ones should
    // track it closely.
    const NormalCheck fc = check_normals(cache, d, 5.0, CLAY_NORMAL_FACE);
    std::printf("\n  CLAY_NORMAL_FACE against the field gradient (are FACE normals flat?):\n");
    if (!fc.ok || fc.checked == 0) {
        std::printf("    could not be checked -- this line measures nothing\n");
    } else {
        std::printf("    %zu vertices, %zu off by more than 5 deg, worst %.2f deg\n", fc.checked,
                    fc.bad, fc.worst_deg);
        if (fc.worst_deg < 15.0)
            std::printf("    FACE normals are SMOOTH (area-weighted per vertex), not flat. A\n"
                        "    sliver's near-zero cross product cannot poison them, so the specks\n"
                        "    cannot come from this call's normals -- whoever shades them flat\n"
                        "    is doing it downstream of this API.\n");
        else
            std::printf("    FACE normals disagree sharply with the field: they are effectively\n"
                        "    flat, and a sliver CAN shade black through them.\n");
    }
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
      // CANDIDATE FIX, measured before proposing it. `compute_face_normals`
    // accumulates EVERY triangle's cross product. A degenerate triangle has no
    // meaningful normal -- its cross product is a direction computed from
    // near-parallel edges -- so including it is a bug rather than a small
    // contribution that area weighting handles. This recomputes the same
    // area-weighted normal with near-zero-area triangles EXCLUDED and scores
    // it the same way. No vertex moves and no triangle changes; only the
    // normals attribute differs, which is a far smaller blast radius than
    // clamping the marcher's t.
    {
        clay_brick_mesh_params p{};
        p.struct_size = sizeof(p);
        p.normals = CLAY_NORMAL_FACE;
        p.colors = 0;
        clay_mesh* m = nullptr;
        if (clay_brick_cache_mesh(cache, d.doc, &p, nullptr, 0, nullptr, &m) == CLAY_OK && m) {
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
            const bool got = clay_mesh_copy_vertices(m, &lay, pos.data(), nv * sizeof(P)) == CLAY_OK
                          && clay_mesh_copy_indices(m, idx.data(), ni) == CLAY_OK;
            clay_mesh_destroy(m);
            if (got && nv && ni) {
                // Two accumulations over identical geometry: all triangles,
                // and all but the slivers. The threshold is on twice the area,
                // which is the cross product's own length.
                std::vector<double> na(nv * 3, 0.0), nb(nv * 3, 0.0);
                double longest = 0.0;
                std::vector<double> cross(ni / 3 * 3, 0.0), clen(ni / 3, 0.0);
                for (std::size_t t = 0; t < ni / 3; ++t) {
                    const P& a = pos[idx[t * 3]];
                    const P& b = pos[idx[t * 3 + 1]];
                    const P& c = pos[idx[t * 3 + 2]];
                    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
                    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
                    const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz,
                                 cz = ux * vy - uy * vx;
                    cross[t * 3] = cx; cross[t * 3 + 1] = cy; cross[t * 3 + 2] = cz;
                    clen[t] = std::sqrt(cx * cx + cy * cy + cz * cz);
                    if (clen[t] > longest) longest = clen[t];
                }
                const double cut = longest * 1e-3;  // a sliver, relative to the mesh's own scale
                std::size_t excluded = 0;
                for (std::size_t t = 0; t < ni / 3; ++t) {
                    const bool sliver = clen[t] < cut;
                    if (sliver) ++excluded;
                    for (int k = 0; k < 3; ++k) {
                        const std::uint32_t v = idx[t * 3 + k];
                        for (int a2 = 0; a2 < 3; ++a2) {
                            na[v * 3 + a2] += cross[t * 3 + a2];
                            if (!sliver) nb[v * 3 + a2] += cross[t * 3 + a2];
                        }
                    }
                }
                std::vector<float> pts(nv * 3), grads(nv * 3, 0.0f);
                for (std::size_t i = 0; i < nv; ++i) {
                    pts[i * 3] = pos[i].x; pts[i * 3 + 1] = pos[i].y; pts[i * 3 + 2] = pos[i].z;
                }
                if (clay_layer_eval_gradients(d.doc, d.layer, "cpu", pts.data(), nv,
                                              grads.data()) == CLAY_OK) {
                    auto score = [&](const std::vector<double>& acc, std::size_t* bad) {
                        double worst = 0.0;
                        *bad = 0;
                        for (std::size_t i = 0; i < nv; ++i) {
                            const double l = std::sqrt(acc[i*3]*acc[i*3] + acc[i*3+1]*acc[i*3+1]
                                                       + acc[i*3+2]*acc[i*3+2]);
                            const float* g = &grads[i * 3];
                            const double gl = std::sqrt((double)g[0]*g[0] + (double)g[1]*g[1]
                                                        + (double)g[2]*g[2]);
                            if (!(l > 1e-20) || !(gl > 1e-6)) continue;
                            double dot = (acc[i*3]*g[0] + acc[i*3+1]*g[1] + acc[i*3+2]*g[2])
                                         / (l * gl);
                            if (dot > 1.0) dot = 1.0;
                            if (dot < -1.0) dot = -1.0;
                            const double deg = std::acos(dot) * 57.29577951308232;
                            if (deg > 5.0) ++*bad;
                            if (deg > worst) worst = deg;
                        }
                        return worst;
                    };
                    std::size_t bad_a = 0, bad_b = 0;
                    const double wa = score(na, &bad_a);
                    const double wb = score(nb, &bad_b);
                    std::printf("\n  CANDIDATE FIX: exclude slivers from the normal accumulation\n");
                    std::printf("    %zu of %zu triangles excluded (under %.3g of the longest)\n",
                                excluded, ni / 3, 1e-3);
                    std::printf("    all triangles      worst %6.2f deg,  %zu over 5 deg\n", wa, bad_a);
                    std::printf("    slivers excluded   worst %6.2f deg,  %zu over 5 deg\n", wb, bad_b);
                    if (excluded == 0)
                        std::printf("    NOTHING WAS EXCLUDED -- the two rows are the same\n"
                                    "    computation and this comparison measures nothing.\n");
                    else if (wb < wa * 0.5)
                        std::printf("    The exclusion helps materially.\n");
                    else
                        std::printf("    The exclusion does NOT account for the disagreement --\n"
                                    "    so the FACE error is marching-cubes tessellation, not\n"
                                    "    slivers, and excluding them would fix nothing.\n");
                }
            }
        }
    }

  std::printf("\n  VERDICT\n");
    const bool brick_holed = b3.boundary > 0 || b1.boundary > 0 || b2.boundary > 0;
    const bool doc_holed = d3.boundary > 0 || d1.boundary > 0 || d2.boundary > 0;
    if (brick_holed || doc_holed)
        std::printf("    boundary edges present -- see the table; a hole in the triangle set.\n");
    else
        std::printf("    NO HOLES. Every stage watertight on both meshers, so the specks are\n"
                    "    not missing cells. That was this probe's first oracle and it is clean.\n");

    // The one that reproduces. Compared AFTER an edit: on the untouched sphere
    // the two meshers produce the same mesh and so the same slivers, which
    // says nothing about either.
    const std::size_t brick_sl = b1.slivers + b2.slivers + b3.slivers;
    const std::size_t doc_sl = d1.slivers + d2.slivers + d3.slivers;
    std::printf("\n    SLIVERS after editing:  brick %zu   document %zu\n", brick_sl, doc_sl);
    if (brick_sl > doc_sl && brick_sl > 0) {
        std::printf("    REPRODUCED. The brick mesher emits near-zero-area triangles the\n"
                    "    document mesher does not. A sliver is legal geometry and is left out\n"
                    "    of `clean` on purpose, but its FACE normal is a cross product of\n"
                    "    near-parallel edges -- numerically garbage. Flat-shaded, that is a\n"
                    "    black speck on a smooth form, with every cell present and every\n"
                    "    gradient correct. It is only visible where a host cannot use\n"
                    "    gradient normals: a live preview with no document behind it, and the\n"
                    "    coarse LOD, which refuses gradient attributes outright.\n");
    } else if (brick_sl == 0) {
        std::printf("    Not reproduced: the brick mesher emitted no slivers here.\n");
    } else {
        std::printf("    Both meshers emit slivers; this fixture does not separate them.\n");
    }

    clay_brick_cache_destroy(cache);
    clay_document_destroy(d.doc);
    return 0;
}
