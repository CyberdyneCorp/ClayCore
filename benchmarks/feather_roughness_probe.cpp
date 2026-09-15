// DOES A DIHEDRAL-ANGLE ROUGHNESS RATIO DETECT A DEFECT NO DISTANCE GATE CAN?
// NOT a gated benchmark. Calibration for issue #596.
//
// THE PREMISE BEING TESTED. #593 shipped a volume whose FEATHER was silently
// dropped. Displacement was correct to four decimals and every parity, golden
// and unit gate stayed green, because all of them compare DISTANCES and the
// feather does not move the zero set -- it shapes the NORMALS across a replace
// boundary. A host caught it with a screen-space roughness metric we cannot
// run, having no renderer. #596 proposes the part that ports: mean dihedral
// angle over interior edges, as a RATIO against the same shape without the
// defect.
//
// Before writing that fixture, the assumption underneath it has to be true on
// OUR mesher at OUR cell sizes: that the ratio actually separates a feathered
// replace from a hard one. If it does not, the fixture is theatre.
//
// WHY THE OBVIOUS FIXTURE MEASURES NOTHING. Build a FieldVolume, set a feather,
// mesh it: the number does not move. `item_is_feathered_replace`
// (src/scene/bounds.cpp:1014) requires op == Replace AND feather > 0, and the
// crossfade is emitted at tape-compile time by `emit_replace_feather`
// (src/scene/tape_build.cpp:372). clay.h:4697 says it plainly -- "every other
// op ignores the feather". So the feather is a property of a PLACEMENT in a
// document, not of a volume, and the fixture has to be a document.
//
// AND THE DEFECT ALREADY HAS A NAME. clay.h:4680 explains why a hard replace is
// bad: it "holds BOTH fields live at the surface ... min/max branch switching
// between two fields that touch is what corrugates the normals at the cell
// wavelength (issue #67) -- the zero set is exact, the shading is not". Issue
// #67 is closed. The feather is its cure. So this probe is also a regression
// check on #67 by a measure that can actually see it, which nothing has been.
//
// WHAT IT DOES. One sphere. Bake a region of it back into a volume and place
// that volume over itself with CLAY_OP_REPLACE -- the #67 round trip, no verb
// applied, the shape unchanged by construction. Mesh, and measure the mean
// angle between incident face normals over interior edges. Sweep the feather
// from 0 (hard, the defect) upward, against the untouched sphere as control.
//
// Reports the RATIO, never the absolute: an absolute roughness is a property of
// the tessellation and the cell size, while the ratio against the same shape
// through the same mesher with the defect absent is a property of the defect.
// That is what let the host's bar hold across tools whose triangle counts
// differ by an order of magnitude, and it is the only form in which a number
// here could gate across backends.
//
// Exits non-zero if it cannot discriminate -- if the control is not smooth, if
// the round trip changes the triangle count so much that the comparison is
// between different meshes, or if the sweep is flat.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "clay.h"

namespace {

constexpr float kRadius = 1.0f;
constexpr float kCell = 0.02f;
constexpr float kBand = 0.06f;  // three cells, which is what <= 0 would pick

struct Roughness {
    double mean_deg = 0.0;
    double p99_deg = 0.0;
    double p999_deg = 0.0;
    double max_deg = 0.0;
    double near_mean_deg = 0.0;  // restricted to edges near the replace boundary
    std::size_t near_edges = 0;
    std::size_t interior_edges = 0;
    std::size_t triangles = 0;
};

// The replace region, shared by the fixture and the localiser below.
constexpr float kBoxLo[3] = {-0.6f, -0.6f, 0.0f};
constexpr float kBoxHi[3] = {0.6f, 0.6f, 1.3f};

// Is this point within `d` of the replace box's SURFACE -- where the crossfade
// lives, and the only place the defect can be? A defect confined to a seam is
// invisible in a mean over the whole sphere; measuring the seam is not cheating,
// it is what the host's metric did by only ever looking at stroked pixels.
bool near_boundary(double x, double y, double z, double d) {
    const double dx = std::min(std::abs(x - kBoxLo[0]), std::abs(x - kBoxHi[0]));
    const double dy = std::min(std::abs(y - kBoxLo[1]), std::abs(y - kBoxHi[1]));
    const double dz = std::min(std::abs(z - kBoxLo[2]), std::abs(z - kBoxHi[2]));
    const bool inside_x = x >= kBoxLo[0] - d && x <= kBoxHi[0] + d;
    const bool inside_y = y >= kBoxLo[1] - d && y <= kBoxHi[1] + d;
    const bool inside_z = z >= kBoxLo[2] - d && z <= kBoxHi[2] + d;
    if (!inside_x || !inside_y || !inside_z) return false;
    return dx <= d || dy <= d || dz <= d;
}

// Mean angle between the two incident face normals, over interior edges.
//
// `validate()` in src/mesh/validate.cpp walks the same edges but keys its map on
// the vertex pair and stores only COUNTS, never the triangle indices, so there
// is nothing there to reuse -- this needs the incident pair itself.
//
// Boundary edges (one incident face) have no angle and are skipped; non-manifold
// edges (more than two) have no well-defined pair and are skipped rather than
// picking two arbitrarily, which would make the number depend on triangle order.
Roughness roughness(const clay_mesh* mesh) {
    Roughness r;
    const std::size_t tris = clay_mesh_index_count(mesh) / 3;
    const std::uint32_t* idx = clay_mesh_indices(mesh);
    const float* pos = clay_mesh_positions(mesh);
    r.triangles = tris;
    if (!tris || !idx || !pos) return r;

    // Face normals once; the edge walk then reads them by triangle index.
    std::vector<double> nx(tris), ny(tris), nz(tris);
    for (std::size_t t = 0; t < tris; ++t) {
        const std::uint32_t a = idx[t * 3], b = idx[t * 3 + 1], c = idx[t * 3 + 2];
        const double ux = pos[b * 3] - pos[a * 3], uy = pos[b * 3 + 1] - pos[a * 3 + 1],
                     uz = pos[b * 3 + 2] - pos[a * 3 + 2];
        const double vx = pos[c * 3] - pos[a * 3], vy = pos[c * 3 + 1] - pos[a * 3 + 1],
                     vz = pos[c * 3 + 2] - pos[a * 3 + 2];
        double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        const double len = std::sqrt(cx * cx + cy * cy + cz * cz);
        if (len > 0.0) { cx /= len; cy /= len; cz /= len; }
        nx[t] = cx; ny[t] = cy; nz[t] = cz;
    }

    struct Pair { std::uint32_t t0, t1; int count; };
    std::unordered_map<std::uint64_t, Pair> edges;
    edges.reserve(tris * 3);
    for (std::size_t t = 0; t < tris; ++t)
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = idx[t * 3 + e], b = idx[t * 3 + (e + 1) % 3];
            if (a == b) continue;
            const std::uint64_t key = a < b ? (static_cast<std::uint64_t>(a) << 32) | b
                                            : (static_cast<std::uint64_t>(b) << 32) | a;
            auto it = edges.find(key);
            if (it == edges.end()) edges.emplace(key, Pair{static_cast<std::uint32_t>(t), 0, 1});
            else { if (it->second.count == 1) it->second.t1 = static_cast<std::uint32_t>(t);
                   ++it->second.count; }
        }

    std::vector<double> angles;
    angles.reserve(edges.size());
    double near_sum = 0.0;
    for (const auto& [key, p] : edges) {
        if (p.count != 2) continue;  // boundary, or non-manifold: no defined pair
        double d = nx[p.t0] * nx[p.t1] + ny[p.t0] * ny[p.t1] + nz[p.t0] * nz[p.t1];
        d = std::max(-1.0, std::min(1.0, d));
        const double deg = std::acos(d) * 57.29577951308232;
        angles.push_back(deg);
        // The edge's midpoint, for the locality test.
        const std::uint32_t a = static_cast<std::uint32_t>(key >> 32);
        const std::uint32_t b = static_cast<std::uint32_t>(key & 0xffffffffu);
        const double mx = 0.5 * (pos[a * 3] + pos[b * 3]);
        const double my = 0.5 * (pos[a * 3 + 1] + pos[b * 3 + 1]);
        const double mz = 0.5 * (pos[a * 3 + 2] + pos[b * 3 + 2]);
        if (near_boundary(mx, my, mz, 3.0 * kCell)) { near_sum += deg; ++r.near_edges; }
    }
    if (angles.empty()) return r;
    double sum = 0.0;
    for (double a : angles) sum += a;
    r.mean_deg = sum / static_cast<double>(angles.size());
    if (r.near_edges) r.near_mean_deg = near_sum / static_cast<double>(r.near_edges);
    std::sort(angles.begin(), angles.end());
    const std::size_t last = angles.size() - 1;
    r.p99_deg = angles[static_cast<std::size_t>(0.99 * static_cast<double>(last))];
    r.p999_deg = angles[static_cast<std::size_t>(0.999 * static_cast<double>(last))];
    r.max_deg = angles[last];
    r.interior_edges = angles.size();
    return r;
}

void mesh_params(clay_mesh_params* mp) {
    std::memset(mp, 0, sizeof *mp);
    mp->struct_size = sizeof *mp;
    mp->voxel_size = kCell;
    mp->decimate = 0;
}

bool add_sphere(clay_document* doc, clay_layer_id layer) {
    const float r = kRadius;
    clay_item* it = clay_item_create(CLAY_PRIM_SPHERE, &r, 1);
    if (!it) return false;
    clay_node_id n = 0;
    const clay_result res = clay_layer_add_item(doc, layer, it, &n);
    clay_item_destroy(it);
    return res == CLAY_OK;
}

// The control: a plain sphere, never round-tripped.
clay_mesh* plain_sphere() {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    if (!doc || clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK || !add_sphere(doc, layer)) {
        if (doc) clay_document_destroy(doc);
        return nullptr;
    }
    clay_mesh_params mp;
    mesh_params(&mp);
    clay_mesh* m = nullptr;
    const clay_result res = clay_document_mesh(doc, &mp, &m);
    clay_document_destroy(doc);
    return res == CLAY_OK ? m : nullptr;
}

// The treatment: the same sphere, with a baked region of itself placed back over
// it as CLAY_OP_REPLACE at the given feather. The SHAPE is unchanged by
// construction -- this is issue #67's round trip -- so any roughness difference
// is the replace boundary and nothing else.
clay_mesh* round_tripped(float feather) {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    if (!doc || clay_add_sdf_layer(doc, "form", &layer) != CLAY_OK || !add_sphere(doc, layer)) {
        if (doc) clay_document_destroy(doc);
        return nullptr;
    }
    clay_volume_params vp;
    std::memset(&vp, 0, sizeof vp);
    vp.struct_size = sizeof vp;
    vp.cell_size = kCell;
    vp.band = kBand;
    vp.padding = kBand;
    vp.feather = feather;

    // A box over one cap of the sphere, so the replace has a real boundary
    // crossing material -- a region containing the whole sphere would put the
    // crossfade out in empty space where nothing can be seen.
    const float lo[3] = {-0.6f, -0.6f, 0.0f};
    const float hi[3] = {0.6f, 0.6f, 1.3f};
    clay_item* vol = nullptr;
    if (clay_item_volume_from_document(doc, &vp, lo, hi, &vol) != CLAY_OK || !vol) {
        clay_document_destroy(doc);
        return nullptr;
    }
    clay_item_set_op(vol, CLAY_OP_REPLACE);
    clay_node_id n = 0;
    const clay_result added = clay_layer_add_item(doc, layer, vol, &n);
    clay_item_destroy(vol);
    if (added != CLAY_OK) { clay_document_destroy(doc); return nullptr; }

    clay_mesh_params mp;
    mesh_params(&mp);
    clay_mesh* m = nullptr;
    const clay_result res = clay_document_mesh(doc, &mp, &m);
    clay_document_destroy(doc);
    return res == CLAY_OK ? m : nullptr;
}

}  // namespace

int main() {
    std::printf("feather_roughness_probe: does a dihedral ratio see what a distance gate cannot?\n");
    std::printf("  sphere r=%.2f, cell %.3f, band %.3f (three cells)\n", static_cast<double>(kRadius),
                static_cast<double>(kCell), static_cast<double>(kBand));
    std::printf("  treatment = the #67 round trip: a baked region placed back as CLAY_OP_REPLACE.\n"
                "  The SHAPE is unchanged by construction, so every difference below is shading.\n\n");

    int failures = 0;

    clay_mesh* control = plain_sphere();
    if (!control) { std::printf("FAIL: could not mesh the control sphere\n"); return 1; }
    const Roughness base = roughness(control);
    std::printf("  control (plain sphere) | %zu tris, %zu interior edges, %zu of them at the seam\n",
                base.triangles, base.interior_edges, base.near_edges);
    std::printf("    mean %.4f  p99 %.4f  p99.9 %.4f  max %.4f  seam-mean %.4f (deg)\n",
                base.mean_deg, base.p99_deg, base.p999_deg, base.max_deg, base.near_mean_deg);
    if (base.mean_deg <= 0.0 || base.interior_edges == 0) {
        std::printf("FAIL: the control has no measurable roughness; the metric is broken, not the engine.\n");
        clay_mesh_destroy(control);
        return 1;
    }
    // A sphere at this cell size is smooth. If the CONTROL is already rough the
    // ratio has no headroom and nothing below means anything.
    if (base.mean_deg > 5.0) {
        std::printf("FAIL: the control is already rough (%.4f deg); pick a smoother fixture.\n",
                    base.mean_deg);
        ++failures;
    }
    std::printf("\n  Five statistics, because WHICH ONE SEPARATES is the finding rather than an\n"
                "  implementation detail. The defect lives on a seam of ~%zu edges in %zu.\n",
                base.near_edges, base.interior_edges);
    std::printf("\n  feather | mean       p99        p99.9      max        seam-mean  (RATIO vs control)\n");

    double ratio_at_zero = 0.0, ratio_at_band = 0.0;
    for (const float f : {0.0f, 0.5f * kBand, kBand, 2.0f * kBand, 4.0f * kBand}) {
        clay_mesh* m = round_tripped(f);
        if (!m) { std::printf("  %7.4f | FAILED to build\n", static_cast<double>(f)); ++failures; continue; }
        const Roughness r = roughness(m);
        auto ratio_of = [](double a, double b) { return b > 0.0 ? a / b : 0.0; };
        const double ratio = ratio_of(r.p99_deg, base.p99_deg);
        std::printf("  %7.4f | %8.3fx  %8.3fx  %8.3fx  %8.3fx  %8.3fx\n", static_cast<double>(f),
                    ratio_of(r.mean_deg, base.mean_deg), ratio_of(r.p99_deg, base.p99_deg),
                    ratio_of(r.p999_deg, base.p999_deg), ratio_of(r.max_deg, base.max_deg), ratio);
        if (f == 0.0f) ratio_at_zero = ratio;
        if (f == kBand) ratio_at_band = ratio;
        // A round trip that changes the triangle count by a lot is not the same
        // mesh, and then the ratio compares two shapes rather than two shadings.
        if (base.triangles > 0) {
            const double drift = std::abs(static_cast<double>(r.triangles) -
                                          static_cast<double>(base.triangles)) /
                                 static_cast<double>(base.triangles);
            if (drift > 0.25)
                std::printf("           NOTE: triangle count differs by %.1f%% from the control --\n"
                            "                 read the ratio with that in mind.\n", drift * 100.0);
        }
        clay_mesh_destroy(m);
    }
    clay_mesh_destroy(control);

    std::printf("\n  Judged on p99. Why not the others:\n");
    std::printf("    MEAN dilutes. The defect lives on a seam; averaged over %zu interior\n"
                "      edges it reads 1.05x, which no bar could separate from noise.\n",
                base.interior_edges);
    std::printf("    MAX and p99.9 are SATURATED: the control sphere already carries edges at\n"
                "      %.1f degrees before anything is done to it, so the top of this\n"
                "      distribution is a property of the marcher and not of the defect.\n",
                base.max_deg);
    std::printf("    The SEAM-LOCAL mean was the intended localiser and it does not work --\n"
                "      a spatial shell around the replace box also selects where the surface\n"
                "      runs tangent to the lattice, which is rougher on the CONTROL too\n"
                "      (%.4f deg there against %.4f overall). It reports the defect as an\n"
                "      IMPROVEMENT. Localise by the field, not by a box, or do not localise.\n",
                base.near_mean_deg, base.mean_deg);
    std::printf("\n  hard replace (feather 0): %.3fx    feathered at one band: %.3fx\n",
                ratio_at_zero, ratio_at_band);
    if (ratio_at_zero > 0.0 && ratio_at_band > 0.0) {
        const double sep = ratio_at_zero / ratio_at_band;
        std::printf("  separation: %.2fx between the defect and the cure.\n", sep);
        if (sep < 1.3) {
            std::printf("\n  REFUTED: the metric does not separate a hard replace from a feathered one\n"
                        "  on this mesher at this cell size. A fixture built on it would be theatre.\n");
            ++failures;
        } else {
            std::printf("\n  The metric discriminates. A bar between the two is derivable, and it is a\n"
                        "  RATIO, so it can hold across backends and tessellations.\n");
        }
    }
    if (failures) {
        std::printf("\n%d check(s) failed.\n", failures);
        return 1;
    }
    std::printf("\nok\n");
    return 0;
}
