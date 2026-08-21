// Fixed-topology mesh brushes (mesh-fixed-topology-brushes): adjacency, the
// eleven verbs, vertex-delta undo and mesh picking.
//
// The bar every test here defends is the contract: topology never changes. The
// index and quad buffers are compared byte for byte, not "checked for the same
// count", because a mesher that rewrote them to the same length would still
// have destroyed the retopology the feature exists to preserve.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "clay/brush/stroke.h"
#include "clay/mesh/adjacency.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/sculpt.h"
#include "clay/pick/pick.h"
#include "clay/scene/tape.h"
#include "scene_utils.h"

using namespace clay;
using namespace clay::kernel;
using clay_test::item;
using mesh::Adjacency;
using mesh::Mesh;
using mesh::MeshBrush;
using mesh::MeshBrushSettings;
using mesh::MeshSculptor;
using mesh::VertexDeltas;

namespace {

// A flat grid on the XZ plane, `n` quads a side, spanning [-half, half].
// Deterministic, analytically checkable, and the only fixture where "did the
// brush move exactly these vertices" has an obvious right answer.
Mesh plane_grid(int n, float half, bool with_normals = true) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
            if (with_normals) m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// The same grid with the middle column of vertices DUPLICATED, so the two
// halves share positions and no indices — a UV seam, as every exported model
// has. Built by hand because no mesher in this tree produces one.
Mesh seamed_plane(int n, float half) {
    Mesh left = plane_grid(n, half);
    Mesh m;
    m.positions = left.positions;
    m.normals = left.normals;
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    const std::uint32_t seam_x = stride / 2;
    // Duplicate the seam column.
    std::vector<std::uint32_t> twin(stride, 0);
    for (std::uint32_t z = 0; z <= static_cast<std::uint32_t>(n); ++z) {
        const std::uint32_t v = z * stride + seam_x;
        twin[z] = static_cast<std::uint32_t>(m.positions.size());
        m.positions.push_back(m.positions[v]);
        m.normals.push_back(m.normals[v]);
    }
    // Rebuild the faces, sending everything right of the seam to the twins.
    for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(n); ++z)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(n); ++x) {
            auto at = [&](std::uint32_t zz, std::uint32_t xx) {
                return xx == seam_x && x >= seam_x ? twin[zz] : zz * stride + xx;
            };
            const std::uint32_t a = at(z, x), b = at(z, x + 1), c = at(z + 1, x),
                                d = at(z + 1, x + 1);
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// Two parallel sheets a hair apart: the closed mouth. A straight-line falloff
// from a point on the upper sheet reaches the lower one; a surface walk has to
// go around the rim, which is what the geodesic test asserts.
Mesh closed_mouth(int n, float half, float gap) {
    Mesh upper = plane_grid(n, half);
    Mesh m = upper;
    const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
    for (const cfloat3& p : upper.positions) {
        m.positions.push_back(cf3(p.x, p.y - gap, p.z));
        m.normals.push_back(cf3(0, -1, 0));
    }
    for (std::uint32_t i : upper.indices) m.indices.push_back(base + i);
    return m;
}

scene::Tape sphere_tape(float r) {
    static scene::Document doc;
    doc = scene::Document{};
    scene::Layer& l = doc.add_sdf_layer("l");
    l.sdf->insert(item(scene::Prim::sphere(r), cf3(0, 0, 0)));
    return scene::compile_document(doc);
}

Mesh sphere_quads(float r, float cell) {
    const math::Aabb region{cf3(-r * 1.4f, -r * 1.4f, -r * 1.4f),
                            cf3(r * 1.4f, r * 1.4f, r * 1.4f)};
    return mesh::mesh_tape_quads(sphere_tape(r), region, cell);
}

bool same_bytes(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(std::uint32_t)) == 0);
}

bool same_bytes(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(cfloat3)) == 0);
}

MeshBrushSettings centred(cfloat3 at, float radius, float strength) {
    MeshBrushSettings s;
    s.center = at;
    s.radius = radius;
    s.strength = strength;
    s.falloff = mesh::MeshFalloff::Smooth;
    return s;
}

// Worst deviation from the least-squares plane y = a + b*x + c*z, over the
// vertices whose ORIGINAL position lies within `radius` of the axis. "Is this
// patch flat" asked properly, rather than by the y spread — which also counts
// the plane's tilt.
float plane_residual(const Mesh& m, const Mesh& base, float radius) {
    double s[3][4] = {{0}};  // normal equations, augmented
    std::vector<std::size_t> window;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const cfloat3& b = base.positions[v];
        if (clength(cf3(b.x, 0, b.z)) > radius) continue;
        window.push_back(v);
        const double t[3] = {1.0, m.positions[v].x, m.positions[v].z};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) s[i][j] += t[i] * t[j];
            s[i][3] += t[i] * static_cast<double>(m.positions[v].y);
        }
    }
    if (window.size() < 4) return 0.0f;
    for (int i = 0; i < 3; ++i) {  // Gauss-Jordan on a 3x3
        int pivot = i;
        for (int r = i + 1; r < 3; ++r)
            if (std::fabs(s[r][i]) > std::fabs(s[pivot][i])) pivot = r;
        if (std::fabs(s[pivot][i]) < 1e-12) return 0.0f;
        for (int j = 0; j < 4; ++j) std::swap(s[i][j], s[pivot][j]);
        for (int r = 0; r < 3; ++r) {
            if (r == i) continue;
            const double f = s[r][i] / s[i][i];
            for (int j = 0; j < 4; ++j) s[r][j] -= f * s[i][j];
        }
    }
    float worst = 0.0f;
    for (std::size_t v : window) {
        const double fit = s[0][3] / s[0][0] + s[1][3] / s[1][1] * m.positions[v].x +
                           s[2][3] / s[2][2] * m.positions[v].z;
        worst = std::max(worst, static_cast<float>(std::fabs(fit - m.positions[v].y)));
    }
    return worst;
}

const MeshBrush kAllVerbs[] = {MeshBrush::Grab,   MeshBrush::Draw,     MeshBrush::Inflate,
                               MeshBrush::Smooth, MeshBrush::Pinch,    MeshBrush::Flatten,
                               MeshBrush::Clay,   MeshBrush::Crease,   MeshBrush::Scrape,
                               MeshBrush::Polish, MeshBrush::Snakehook};

}  // namespace

// -- adjacency ---------------------------------------------------------------

TEST_CASE("adjacency: a plane's interior vertex has the ring its triangulation gives it") {
    Mesh m = plane_grid(4, 1.0f);
    Adjacency adj = Adjacency::build(m);
    CHECK(adj.matches(m));
    CHECK(adj.vertex_count() == m.positions.size());
    CHECK(adj.class_count() == m.positions.size());  // nothing coincides

    // The centre of a 5x5 grid, triangulated with the a-c-b / b-c-d diagonal:
    // four axial neighbours plus the two the diagonal adds.
    const std::uint32_t centre = 2 * 5 + 2;
    std::size_t n = 0;
    adj.ring(adj.class_of(centre), &n);
    CHECK(n == 6);

    std::size_t tris = 0;
    adj.triangles_of(adj.class_of(centre), &tris);
    CHECK(tris == 6);
}

TEST_CASE("adjacency: welding crosses a UV seam that the index buffer does not") {
    Mesh m = seamed_plane(4, 1.0f);
    // The seam column really is duplicated: more vertices than a plain grid.
    CHECK(m.positions.size() > plane_grid(4, 1.0f).positions.size());

    Adjacency welded = Adjacency::build(m);
    CHECK(welded.class_count() == plane_grid(4, 1.0f).positions.size());

    // Exact welding on the raw indices would leave the two sides separate.
    Adjacency raw = Adjacency::build(m, 0.0f);
    CHECK(raw.class_count() == welded.class_count());  // the duplicates are bit-identical

    // A seam class carries both of its vertices, and its ring reaches both ways.
    const std::uint32_t seam_vertex = 2 * 5 + 2;
    std::size_t members = 0;
    welded.members(welded.class_of(seam_vertex), &members);
    CHECK(members == 2);
}

TEST_CASE("adjacency: it refuses to be paired with a different mesh") {
    Mesh a = plane_grid(4, 1.0f);
    Mesh b = plane_grid(6, 1.0f);
    Adjacency adj = Adjacency::build(a);
    CHECK(adj.matches(a));
    CHECK_FALSE(adj.matches(b));

    MeshSculptor bad(b, Adjacency::build(a));
    CHECK_FALSE(bad.valid());
    CHECK(bad.stamp(MeshBrush::Draw, centred(cf3(0, 0, 0), 0.5f, 1.0f)) == 0);
}

// -- the contract ------------------------------------------------------------

TEST_CASE("every verb leaves indices and quads byte-identical, on a quad mesh") {
    const Mesh original = sphere_quads(1.0f, 0.14f);
    REQUIRE(!original.empty());
    REQUIRE(original.has_quads());
    REQUIRE(mesh::quads_consistent(original));

    for (MeshBrush verb : kAllVerbs) {
        Mesh m = original;
        MeshSculptor sculptor(m);
        MeshBrushSettings s = centred(cf3(0, 1.0f, 0), 0.5f, 0.6f);
        s.direction = cf3(0.1f, 0.05f, 0.0f);
        s.geodesic = mesh::default_geodesic(verb);
        CAPTURE(static_cast<int>(verb));
        CHECK(sculptor.stamp(verb, s) > 0);
        CHECK(same_bytes(m.indices, original.indices));
        CHECK(same_bytes(m.quads, original.quads));
        CHECK(m.positions.size() == original.positions.size());
        CHECK(mesh::quads_consistent(m));
        CHECK_FALSE(same_bytes(m.positions, original.positions));
    }
}

TEST_CASE("mesh sculpting is deterministic: the same stroke twice, bit for bit") {
    auto run = [] {
        Mesh m = plane_grid(24, 1.0f);
        MeshSculptor sculptor(m);
        std::vector<brush::StrokeSample> samples;
        for (int i = 0; i < 12; ++i)
            samples.push_back({cf3(-0.6f + 0.1f * static_cast<float>(i), 0.0f, 0.05f), 0.8f, 0.0f});
        brush::StrokePreset preset;
        preset.radius = 0.3f;
        preset.strength = 0.7f;
        preset.jitter_position = 0.2f;
        preset.seed = 7;
        preset.taper_start = 0.2f;
        brush::apply_to_mesh(sculptor, brush::resolve_stroke(samples, preset), MeshBrush::Draw,
                             centred(cf3(0, 0, 0), 0.3f, 1.0f));
        return m;
    };
    const Mesh a = run(), b = run();
    CHECK(same_bytes(a.positions, b.positions));
    CHECK(same_bytes(a.normals, b.normals));
}

TEST_CASE("smoothing a seamed mesh leaves no crack: coincident vertices stay coincident") {
    Mesh m = seamed_plane(8, 1.0f);
    // Roughen it so there is something to smooth.
    for (std::size_t i = 0; i < m.positions.size(); ++i)
        m.positions[i].y += (i % 3 == 0 ? 0.05f : -0.02f);

    MeshSculptor sculptor(m);
    const Adjacency& adj = sculptor.adjacency();
    MeshBrushSettings s = centred(cf3(0, 0, 0), 0.9f, 1.0f);
    s.smooth_iterations = 3;
    CHECK(sculptor.stamp(MeshBrush::Smooth, s) > 0);

    for (std::uint32_t c = 0; c < adj.class_count(); ++c) {
        std::size_t n = 0;
        const std::uint32_t* members = adj.members(c, &n);
        for (std::size_t k = 1; k < n; ++k) {
            CHECK(m.positions[members[k]].x == m.positions[members[0]].x);
            CHECK(m.positions[members[k]].y == m.positions[members[0]].y);
            CHECK(m.positions[members[k]].z == m.positions[members[0]].z);
        }
    }
}

// -- the falloff -------------------------------------------------------------

TEST_CASE("a surface-measured falloff does not reach across a closed mouth") {
    const float gap = 0.02f;
    Mesh m = closed_mouth(8, 1.0f, gap);
    const std::size_t upper = m.positions.size() / 2;

    auto lower_moved = [&](bool geodesic) {
        Mesh work = m;
        MeshSculptor sculptor(work);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.5f, 1.0f);
        s.geodesic = geodesic;
        s.falloff = mesh::MeshFalloff::Constant;
        sculptor.stamp(MeshBrush::Draw, s);
        std::size_t moved = 0;
        for (std::size_t v = upper; v < work.positions.size(); ++v)
            if (work.positions[v].y != m.positions[v].y) ++moved;
        return moved;
    };

    // A straight line reaches the lower sheet through the gap; the surface walk
    // would have to leave the sheet, and the two sheets share no edge.
    CHECK(lower_moved(false) > 0);
    CHECK(lower_moved(true) == 0);
}

TEST_CASE("the surface walk is not clipped by the grid it walks on") {
    // The regression for a ragged rim. A path along EDGES overestimates
    // geodesic distance — on a structured grid by up to sqrt(2), where a
    // diagonal has to zigzag — so a region bounded by path length alone stops
    // short in some directions and not others. Bounding it by the BALL and
    // weighing by the straight line makes the two modes agree exactly on a
    // connected sheet, which is what this asserts.
    const Mesh base = plane_grid(24, 1.0f);
    auto run = [&](bool geodesic) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.6f, 0.7f);
        s.geodesic = geodesic;
        sculptor.stamp(MeshBrush::Draw, s);
        return m;
    };
    const Mesh walked = run(true), straight = run(false);
    CHECK(same_bytes(walked.positions, straight.positions));
    CHECK_FALSE(same_bytes(walked.positions, base.positions));
}

// -- the verbs ---------------------------------------------------------------

TEST_CASE("draw takes one direction and inflate takes each vertex's own") {
    // A saddle: the two verbs must disagree, and where they disagree is the
    // whole distinction between them.
    Mesh base = plane_grid(16, 1.0f);
    for (cfloat3& p : base.positions) p.y = 0.4f * (p.x * p.x - p.z * p.z);

    auto run = [&](MeshBrush verb) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        sculptor.stamp(verb, centred(cf3(0, 0, 0), 0.7f, 0.5f));
        return m;
    };
    const Mesh drawn = run(MeshBrush::Draw), inflated = run(MeshBrush::Inflate);
    CHECK_FALSE(same_bytes(drawn.positions, inflated.positions));

    // Draw's displacements are all parallel; inflate's are not.
    auto spread = [&](const Mesh& m) {
        cfloat3 first = cf3(0, 0, 0);
        float worst = 0.0f;
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            const cfloat3 d = m.positions[v] - base.positions[v];
            if (clength(d) < 1e-5f) continue;
            const cfloat3 u = cnormalize(d);
            if (clength(first) == 0.0f) first = u;
            worst = std::max(worst, 1.0f - cdot(u, first));
        }
        return worst;
    };
    CHECK(spread(drawn) < 1e-4f);
    CHECK(spread(inflated) > 1e-2f);
}

TEST_CASE("pinch is one signed deformation: positive gathers, negative spreads") {
    Mesh base = plane_grid(16, 1.0f);
    auto radial_spread = [&](float strength) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        sculptor.stamp(MeshBrush::Pinch, centred(cf3(0, 0, 0), 0.6f, strength));
        float sum = 0.0f;
        for (const cfloat3& p : m.positions) sum += clength(cf3(p.x, 0, p.z));
        return sum;
    };
    const float rest = radial_spread(0.0f);
    CHECK(radial_spread(0.5f) < rest);   // pinch pulls in
    CHECK(radial_spread(-0.5f) > rest);  // magnify pushes out
}

TEST_CASE("pinch stays on the surface: it gathers tangentially, it does not sink") {
    Mesh m = plane_grid(16, 1.0f);
    MeshSculptor sculptor(m);
    sculptor.stamp(MeshBrush::Pinch, centred(cf3(0, 0, 0), 0.6f, 0.8f));
    for (const cfloat3& p : m.positions) CHECK(std::fabs(p.y) < 1e-6f);
}

TEST_CASE("flatten's three modes cut, fill and do both") {
    Mesh base = plane_grid(16, 1.0f);
    for (std::size_t v = 0; v < base.positions.size(); ++v)
        base.positions[v].y = (v % 2 == 0) ? 0.1f : -0.1f;

    auto run = [&](field::FlattenMode mode) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.5f, 1.0f);
        s.geodesic = false;
        s.falloff = mesh::MeshFalloff::Constant;
        s.use_given_plane = true;
        s.plane_point = cf3(0, 0, 0);
        s.plane_normal = cf3(0, 1, 0);
        s.flatten_mode = mode;
        sculptor.stamp(MeshBrush::Flatten, s);
        int cut = 0, filled = 0;
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            if (base.positions[v].y > 0.0f && m.positions[v].y < base.positions[v].y) ++cut;
            if (base.positions[v].y < 0.0f && m.positions[v].y > base.positions[v].y) ++filled;
        }
        return std::pair<int, int>{cut, filled};
    };

    const auto both = run(field::FlattenMode::TwoSided);
    const auto cut_only = run(field::FlattenMode::CutOnly);
    const auto fill_only = run(field::FlattenMode::FillOnly);
    CHECK(both.first > 0);
    CHECK(both.second > 0);
    CHECK(cut_only.first > 0);
    CHECK(cut_only.second == 0);
    CHECK(fill_only.first == 0);
    CHECK(fill_only.second > 0);
}

TEST_CASE("clay leaves a flat top where draw keeps whatever was underneath") {
    // The difference only shows on an UNEVEN surface, which is the point: draw
    // adds the same deposit to every vertex and so carries the bumps up with
    // it; clay fills to a plane and so levels them.
    Mesh base = plane_grid(20, 1.0f);
    for (std::size_t v = 0; v < base.positions.size(); ++v)
        base.positions[v].y = (v % 3 == 0 ? 0.04f : -0.02f);

    auto run = [&](MeshBrush verb) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.6f, 0.5f);
        s.geodesic = false;
        s.falloff = mesh::MeshFalloff::Constant;  // isolate the clamp from the curve
        sculptor.stamp(verb, s);
        return m;
    };
    const Mesh clayed = run(MeshBrush::Clay), drawn = run(MeshBrush::Draw);

    auto height_spread = [&](const Mesh& m) {
        float lo = 1e9f, hi = -1e9f;
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            const cfloat3& b = base.positions[v];
            if (clength(cf3(b.x, 0, b.z)) > 0.4f) continue;
            lo = std::min(lo, m.positions[v].y);
            hi = std::max(hi, m.positions[v].y);
        }
        return hi - lo;
    };
    // Clay's top is FLAT — but flat against the region's own averaged normal,
    // which a bumpy surface tilts a little off vertical, so the y spread alone
    // is not the measurement. Residual from a least-squares plane is.
    CHECK(plane_residual(clayed, base, 0.4f) < 1e-5f);
    CHECK(plane_residual(drawn, base, 0.4f) > 0.01f);
    CHECK(height_spread(clayed) < height_spread(drawn) * 0.25f);
    CHECK(height_spread(drawn) == doctest::Approx(0.06f).epsilon(1e-3));  // the bumps rode up
    // And it is a deposit, not a flatten: the middle rose.
    CHECK(clayed.positions[20 / 2 * 21 + 20 / 2].y > 0.05f);
}

TEST_CASE("crease cuts and squeezes in one stamp") {
    Mesh base = plane_grid(24, 1.0f);
    Mesh m = base;
    MeshSculptor sculptor(m);
    MeshBrushSettings s = centred(cf3(0, 0, 0), 0.4f, 0.6f);
    s.geodesic = false;
    CHECK(sculptor.stamp(MeshBrush::Crease, s) > 0);

    bool cut = false, gathered = false;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        if (m.positions[v].y < base.positions[v].y - 1e-4f) cut = true;
        const float before = clength(cf3(base.positions[v].x, 0, base.positions[v].z));
        const float after = clength(cf3(m.positions[v].x, 0, m.positions[v].z));
        if (before > 1e-3f && after < before - 1e-5f) gathered = true;
    }
    CHECK(cut);
    CHECK(gathered);
}

TEST_CASE("polish keeps a hard edge that a plain smooth rounds off") {
    // A chevron: two flats meeting at a 77-degree crease, with a LOW-FREQUENCY
    // ripple on both. Low frequency on purpose — per-vertex jitter is not what
    // surface error looks like, and it defeats any curvature gate by
    // construction, because noise steeper than the feature makes every flat
    // look like an edge. See the note on ring_disagreement.
    Mesh base = plane_grid(28, 1.0f);
    for (std::size_t v = 0; v < base.positions.size(); ++v) {
        cfloat3& p = base.positions[v];
        const float ripple = 0.015f * std::sin(p.x * 10.5f) * std::sin(p.z * 10.5f);
        p.y = std::fabs(p.x) * 0.8f + ripple;
    }

    auto run = [&](MeshBrush verb) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.9f, 1.0f);
        s.falloff = mesh::MeshFalloff::Constant;
        s.smooth_iterations = 4;
        s.polish_angle = 0.25f;
        sculptor.stamp(verb, s);
        return m;
    };
    const Mesh polished = run(MeshBrush::Polish), smoothed = run(MeshBrush::Smooth);

    // How much of the crease each verb destroyed, along x == 0.
    auto crease_damage = [&](const Mesh& m) {
        float sum = 0.0f;
        int n = 0;
        for (std::size_t v = 0; v < m.positions.size(); ++v)
            if (std::fabs(base.positions[v].x) < 1e-6f) {
                sum += std::fabs(m.positions[v].y - base.positions[v].y);
                ++n;
            }
        return n ? sum / static_cast<float>(n) : 0.0f;
    };
    // And how much of the ripple is left on the flats — the job it was asked
    // to do, measured away from the crease so the crease cannot flatter it.
    auto ripple_left = [&](const Mesh& m) {
        float sum = 0.0f;
        int n = 0;
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            const cfloat3& b = base.positions[v];
            if (std::fabs(b.x) < 0.25f || std::fabs(b.x) > 0.75f) continue;
            const float ideal = std::fabs(m.positions[v].x) * 0.8f;
            sum += std::fabs(m.positions[v].y - ideal);
            ++n;
        }
        return n ? sum / static_cast<float>(n) : 0.0f;
    };

    // Polish removed the ripple...
    CHECK(ripple_left(polished) < ripple_left(base) * 0.8f);
    // ...and left the crease alone, where a plain smooth rounded it off.
    CHECK(crease_damage(smoothed) > 0.01f);
    CHECK(crease_damage(polished) < crease_damage(smoothed) * 0.35f);
}

TEST_CASE("scrape is one operation, not flatten followed by smooth") {
    Mesh base = plane_grid(20, 1.0f);
    for (std::size_t v = 0; v < base.positions.size(); ++v)
        base.positions[v].y = (v % 4 == 0 ? 0.08f : -0.03f);

    Mesh one = base;
    {
        MeshSculptor sculptor(one);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.6f, 0.8f);
        s.geodesic = false;
        sculptor.stamp(MeshBrush::Scrape, s);
    }
    Mesh sequenced = base;
    {
        MeshSculptor sculptor(sequenced);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.6f, 0.8f);
        s.geodesic = false;
        s.flatten_mode = field::FlattenMode::CutOnly;
        sculptor.stamp(MeshBrush::Flatten, s);
        sculptor.stamp(MeshBrush::Smooth, s);
    }
    CHECK_FALSE(same_bytes(one.positions, sequenced.positions));
}

// -- masking -----------------------------------------------------------------

TEST_CASE("a mask protects half a region, for a displacement verb and for smooth") {
    for (MeshBrush verb : {MeshBrush::Draw, MeshBrush::Smooth}) {
        Mesh base = plane_grid(20, 1.0f);
        for (std::size_t v = 0; v < base.positions.size(); ++v)
            base.positions[v].y = (v % 3 == 0 ? 0.03f : -0.01f);
        Mesh m = base;
        MeshSculptor sculptor(m);
        MeshBrushSettings s = centred(cf3(0, 0, 0), 0.9f, 1.0f);
        s.geodesic = false;

        // Everything with x > 0 is frozen.
        field::MaskGate gate = [](cfloat3 p) { return p.x > 0.0f ? 1.0f : 0.0f; };
        CAPTURE(static_cast<int>(verb));
        CHECK(sculptor.stamp(verb, s, gate) > 0);

        bool moved_free = false;
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            if (base.positions[v].x > 0.0f) {
                CHECK(m.positions[v].x == base.positions[v].x);
                CHECK(m.positions[v].y == base.positions[v].y);
                CHECK(m.positions[v].z == base.positions[v].z);
            } else if (m.positions[v].y != base.positions[v].y) {
                moved_free = true;
            }
        }
        CHECK(moved_free);
    }
}

TEST_CASE("a MaskField reaches every verb through apply_to_mesh") {
    Mesh base = plane_grid(20, 1.0f);
    Mesh m = base;
    MeshSculptor sculptor(m);

    voxel::MaskField mask(0.05f);
    voxel::BrushParams p;
    p.size = 40;
    p.shape = voxel::BrushShape::Cube;
    p.strength = 1.0f;
    mask.paint(cf3(0.6f, 0.0f, 0.0f), p, 1.0f);

    std::vector<brush::StrokeSample> samples{{cf3(-0.4f, 0, 0), 1.0f, 0.0f},
                                             {cf3(0.4f, 0, 0), 1.0f, 0.0f}};
    brush::StrokePreset preset;
    preset.radius = 0.25f;
    preset.strength = 1.0f;
    const std::vector<brush::Stamp> stamps = brush::resolve_stroke(samples, preset);
    REQUIRE(stamps.size() > 1);

    CHECK(brush::apply_to_mesh(sculptor, stamps, MeshBrush::Draw,
                               centred(cf3(0, 0, 0), 0.25f, 1.0f), &mask) > 0);

    // Deep inside the painted region nothing moved.
    for (std::size_t v = 0; v < m.positions.size(); ++v)
        if (base.positions[v].x > 0.5f && std::fabs(base.positions[v].z) < 0.4f)
            CHECK(m.positions[v].y == base.positions[v].y);
}

// -- undo --------------------------------------------------------------------

TEST_CASE("vertex deltas restore a stroked mesh bit-exactly") {
    const Mesh original = sphere_quads(1.0f, 0.12f);
    REQUIRE(!original.empty());
    Mesh m = original;
    MeshSculptor sculptor(m);

    std::vector<brush::StrokeSample> samples;
    for (int i = 0; i < 10; ++i)
        samples.push_back({cf3(-0.5f + 0.1f * static_cast<float>(i), 0.9f, 0.0f), 1.0f, 0.0f});
    brush::StrokePreset preset;
    preset.radius = 0.3f;
    preset.strength = 0.8f;

    VertexDeltas deltas;
    const std::vector<brush::Stamp> stamps = brush::resolve_stroke(samples, preset);
    CHECK(brush::apply_to_mesh(sculptor, stamps, MeshBrush::Draw, centred(cf3(0, 0, 0), 0.3f, 1.0f),
                               nullptr, &deltas) > 0);
    CHECK_FALSE(same_bytes(m.positions, original.positions));
    CHECK(deltas.size() > 0);
    // One gesture, one record: bounded by the vertices reached, not by the
    // stamps taken.
    CHECK(deltas.size() <= original.positions.size());

    CHECK(deltas.revert(m));
    CHECK(same_bytes(m.positions, original.positions));
    CHECK(same_bytes(m.normals, original.normals));
    CHECK(same_bytes(m.indices, original.indices));
    CHECK(same_bytes(m.quads, original.quads));

    // Idempotent, and re-applicable.
    CHECK(deltas.revert(m));
    CHECK(same_bytes(m.positions, original.positions));
    CHECK(deltas.apply(m));
    CHECK_FALSE(same_bytes(m.positions, original.positions));
}

TEST_CASE("a deferred-normal stroke undoes exactly as a per-stamp one does") {
    const Mesh original = plane_grid(20, 1.0f);
    std::vector<brush::StrokeSample> samples;
    for (int i = 0; i < 8; ++i)
        samples.push_back({cf3(-0.4f + 0.1f * static_cast<float>(i), 0.0f, 0.0f), 1.0f, 0.0f});
    brush::StrokePreset preset;
    preset.radius = 0.25f;
    preset.strength = 0.6f;
    const std::vector<brush::Stamp> stamps = brush::resolve_stroke(samples, preset);

    auto run = [&](bool defer) {
        Mesh m = original;
        MeshSculptor sculptor(m);
        brush::MeshStrokeOptions options;
        options.defer_normals = defer;
        VertexDeltas deltas;
        brush::apply_to_mesh(sculptor, stamps, MeshBrush::Draw, centred(cf3(0, 0, 0), 0.25f, 1.0f),
                             nullptr, &deltas, options);
        Mesh sculpted = m;
        CHECK(deltas.revert(m));
        return std::pair<Mesh, Mesh>{sculpted, m};
    };
    const auto eager = run(false);
    const auto lazy = run(true);
    // Deferring changes nothing about the result...
    CHECK(same_bytes(eager.first.positions, lazy.first.positions));
    CHECK(same_bytes(eager.first.normals, lazy.first.normals));
    // ...nor about the undo.
    CHECK(same_bytes(lazy.second.positions, original.positions));
    CHECK(same_bytes(lazy.second.normals, original.normals));
}

TEST_CASE("a record refuses a mesh it does not belong to") {
    Mesh m = plane_grid(8, 1.0f);
    MeshSculptor sculptor(m);
    VertexDeltas deltas;
    sculptor.stamp(MeshBrush::Draw, centred(cf3(0, 0, 0), 0.5f, 1.0f), {}, &deltas);
    Mesh other = plane_grid(2, 1.0f);
    CHECK_FALSE(deltas.revert(other));
}

// -- strokes -----------------------------------------------------------------

TEST_CASE("grab anchors and snakehook walks") {
    const Mesh base = plane_grid(24, 1.5f);
    std::vector<brush::StrokeSample> samples;
    for (int i = 0; i < 10; ++i)
        samples.push_back({cf3(-0.5f + 0.12f * static_cast<float>(i), 0.0f, 0.0f), 1.0f, 0.0f});
    brush::StrokePreset preset;
    preset.radius = 0.25f;
    preset.strength = 1.0f;
    const std::vector<brush::Stamp> stamps = brush::resolve_stroke(samples, preset);

    auto reach = [&](MeshBrush verb) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        brush::apply_to_mesh(sculptor, stamps, verb, centred(cf3(0, 0, 0), 0.25f, 1.0f));
        float furthest = -1e9f;
        for (std::size_t v = 0; v < m.positions.size(); ++v)
            if (clength(m.positions[v] - base.positions[v]) > 1e-5f)
                furthest = std::max(furthest, base.positions[v].x);
        return furthest;
    };
    // The snakehook's region travels with the drag, so it touches vertices far
    // to the right that the anchored grab never reaches.
    CHECK(reach(MeshBrush::Snakehook) > reach(MeshBrush::Grab) + 0.1f);
}

TEST_CASE("a stroke's stamps carry the radius and strength, not the settings") {
    const Mesh base = plane_grid(24, 1.0f);
    auto peak = [&](float preset_strength) {
        Mesh m = base;
        MeshSculptor sculptor(m);
        std::vector<brush::StrokeSample> samples{{cf3(0, 0, 0), 1.0f, 0.0f}};
        brush::StrokePreset preset;
        preset.radius = 0.3f;
        preset.strength = preset_strength;
        brush::apply_to_mesh(sculptor, brush::resolve_stroke(samples, preset), MeshBrush::Draw,
                             centred(cf3(0, 0, 0), 999.0f, 1.0f));
        float hi = 0.0f;
        for (const cfloat3& p : m.positions) hi = std::max(hi, p.y);
        return hi;
    };
    CHECK(peak(1.0f) > peak(0.4f));
    // The settings' absurd radius was ignored: only the stamp's 0.3 acted.
    Mesh m = base;
    MeshSculptor sculptor(m);
    std::vector<brush::StrokeSample> samples{{cf3(0, 0, 0), 1.0f, 0.0f}};
    brush::StrokePreset preset;
    preset.radius = 0.3f;
    brush::apply_to_mesh(sculptor, brush::resolve_stroke(samples, preset), MeshBrush::Draw,
                         centred(cf3(0, 0, 0), 999.0f, 1.0f));
    for (std::size_t v = 0; v < m.positions.size(); ++v)
        if (clength(cf3(base.positions[v].x, 0, base.positions[v].z)) > 0.35f)
            CHECK(m.positions[v].y == base.positions[v].y);
}

// -- picking -----------------------------------------------------------------

TEST_CASE("raycast_mesh names a triangle and reconstructs the hit from it") {
    Mesh m = plane_grid(8, 1.0f);
    mesh::Bvh bvh = mesh::Bvh::build(m);
    math::Ray ray{cf3(0.21f, 1.0f, -0.13f), cf3(0, -1, 0)};
    const pick::MeshHit hit = pick::raycast_mesh(m, bvh, ray);
    REQUIRE(hit.hit);
    CHECK(hit.t == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(hit.position.y == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(1e-4));
    REQUIRE(hit.triangle * 3 + 2 < m.indices.size());

    const cfloat3 a = m.positions[m.indices[hit.triangle * 3]];
    const cfloat3 b = m.positions[m.indices[hit.triangle * 3 + 1]];
    const cfloat3 c = m.positions[m.indices[hit.triangle * 3 + 2]];
    const cfloat3 rebuilt = a * (1.0f - hit.u - hit.v) + b * hit.u + c * hit.v;
    CHECK(clength(rebuilt - hit.position) < 1e-5f);
}

TEST_CASE("raycast_mesh works through a layer transform") {
    Mesh m = plane_grid(8, 1.0f);
    mesh::Bvh bvh = mesh::Bvh::build(m);
    math::Transform xform;
    xform.position = cf3(2.0f, 0.5f, -1.0f);
    xform.rotation = math::Quat::from_axis_angle(cf3(0, 0, 1), 0.3f);
    xform.scale = 2.0f;

    const cfloat3 world_target = xform.apply(cf3(0.1f, 0.0f, 0.2f));
    math::Ray ray{world_target + cf3(0, 5, 0), cf3(0, -1, 0)};
    const pick::MeshHit hit = pick::raycast_mesh(m, bvh, ray, xform);
    REQUIRE(hit.hit);
    CHECK(clength(hit.position - ray.at(hit.t)) < 1e-4f);
    CHECK(std::fabs(clength(hit.normal) - 1.0f) < 1e-4f);
    // The plane is tilted by the transform, so the normal is too.
    CHECK(cdot(hit.normal, xform.rotation.rotate(cf3(0, 1, 0))) > 0.99f);
}

TEST_CASE("raycast_mesh falls back to the geometric normal, and reports a miss") {
    Mesh m = plane_grid(4, 1.0f, /*with_normals=*/false);
    mesh::Bvh bvh = mesh::Bvh::build(m);
    const pick::MeshHit hit =
        pick::raycast_mesh(m, bvh, math::Ray{cf3(0.1f, 2, 0.1f), cf3(0, -1, 0)});
    REQUIRE(hit.hit);
    CHECK(std::fabs(std::fabs(hit.normal.y) - 1.0f) < 1e-4f);

    const pick::MeshHit missed = pick::raycast_mesh(m, bvh, math::Ray{cf3(9, 2, 9), cf3(0, -1, 0)});
    CHECK_FALSE(missed.hit);
}

TEST_CASE("the BVH hits back faces: a shell is pickable from inside") {
    Mesh m = plane_grid(4, 1.0f);
    mesh::Bvh bvh = mesh::Bvh::build(m);
    const pick::MeshHit below =
        pick::raycast_mesh(m, bvh, math::Ray{cf3(0.1f, -2, 0.1f), cf3(0, 1, 0)});
    CHECK(below.hit);
}

TEST_CASE("the ray index did not disturb the distance and winding queries") {
    Mesh m = sphere_quads(1.0f, 0.1f);
    REQUIRE(!m.empty());
    mesh::Bvh bvh = mesh::Bvh::build(m);
    CHECK(bvh.unsigned_distance(cf3(0, 0, 0)) == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(bvh.is_inside(cf3(0, 0, 0)));
    CHECK_FALSE(bvh.is_inside(cf3(3, 0, 0)));
    CHECK(bvh.signed_distance(cf3(2, 0, 0)) > 0.0f);
}

// -- the layering ------------------------------------------------------------

TEST_CASE("sculpting a mesh layer does not change what the document evaluates to") {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("body");
    l.sdf->insert(item(scene::Prim::sphere(0.5f), cf3(0, 0, 0)));
    const scene::Tape before = scene::compile_document(doc);

    Mesh carried = plane_grid(8, 1.0f);
    MeshSculptor sculptor(carried);
    sculptor.stamp(MeshBrush::Draw, centred(cf3(0, 0, 0), 0.5f, 1.0f));

    const scene::Tape after = scene::compile_document(doc);
    for (float x = -1.0f; x <= 1.0f; x += 0.25f)
        for (float y = -1.0f; y <= 1.0f; y += 0.25f)
            CHECK(before.eval(cf3(x, y, 0.1f)).d == after.eval(cf3(x, y, 0.1f)).d);
}

// -- the verbs the vocabulary was missing --------------------------------------
// Relax, Layer and Nudge, plus alphas on every verb at once
// (close-mesh-brush-vocabulary).

namespace {

// A dome, because CURVATURE is what separates relax from smooth. On a flat
// plane the Laplacian displacement is already tangential, so removing its
// normal component changes nothing and the two verbs are identical — a fixture
// that would pass the test while proving none of it.
Mesh dome_grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            const float px = -half + step * static_cast<float>(x);
            const float pz = -half + step * static_cast<float>(z);
            m.positions.push_back(cf3(px, 0.6f * std::exp(-(px * px + pz * pz) * 1.5f), pz));
            m.normals.push_back(cf3(0, 1, 0));
        }
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a =
                static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
            const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
            for (std::uint32_t i : {a, c, b, b, c, d}) m.indices.push_back(i);
        }
    return m;
}

// The dome's analytic normal, so "how far did it move along the surface normal"
// is measured against the shape rather than against the mesh's own stored
// normals, which the verbs are busy changing.
cfloat3 dome_normal(cfloat3 p) {
    const float e = 0.6f * std::exp(-(p.x * p.x + p.z * p.z) * 1.5f);
    return cnormalize(cf3(3.0f * p.x * e, 1.0f, 3.0f * p.z * e));
}

// Mean motion split into "along the surface normal" and "across the surface".
void split_motion(const Mesh& before, const Mesh& after, cfloat3 c, float rad, float* along,
                  float* across) {
    double an = 0, tg = 0;
    int k = 0;
    for (std::size_t i = 0; i < before.positions.size(); ++i) {
        if (clength(before.positions[i] - c) > rad) continue;
        const cfloat3 d = after.positions[i] - before.positions[i];
        const cfloat3 n = dome_normal(before.positions[i]);
        const float along_n = cdot(d, n);
        an += std::fabs(along_n);
        tg += clength(d - n * along_n);
        ++k;
    }
    *along = k ? static_cast<float>(an / k) : 0.0f;
    *across = k ? static_cast<float>(tg / k) : 0.0f;
}

// Coefficient of variation of edge length in the region — how uneven the
// triangulation is, which is what relax exists to lower.
float edge_variation(const Mesh& m, cfloat3 c, float rad) {
    std::vector<float> len;
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
        for (int e = 0; e < 3; ++e) {
            const cfloat3 a = m.positions[m.indices[i + e]];
            const cfloat3 b = m.positions[m.indices[i + (e + 1) % 3]];
            if (clength((a + b) * 0.5f - c) > rad) continue;
            len.push_back(clength(a - b));
        }
    if (len.size() < 2) return 0.0f;
    float mean = 0.0f;
    for (float v : len) mean += v;
    mean /= static_cast<float>(len.size());
    float var = 0.0f;
    for (float v : len) var += (v - mean) * (v - mean);
    return std::sqrt(var / static_cast<float>(len.size())) / mean;
}

// A dome stretched sideways by a big grab — the state relax exists to recover,
// and the one topology-fixed sculpting actually produces.
Mesh stretched_dome(cfloat3 centre) {
    Mesh m = dome_grid(48, 1.0f);
    MeshSculptor sc(m);
    MeshBrushSettings s;
    s.center = centre;
    s.radius = 0.7f;
    s.direction = cf3(0.25f, 0, 0);
    sc.stamp(MeshBrush::Grab, s, {}, nullptr);
    return m;
}

}  // namespace

TEST_CASE("relax evens the triangulation while barely moving the surface") {
    // The distinction from smooth, which is the whole reason it is a separate
    // verb: smooth moves toward the Laplacian average, which is INWARD on a
    // convex region — that is why smoothing shrinks. Relax takes the same
    // target and keeps only its tangential part.
    const cfloat3 centre = cf3(0, 0.6f, 0);  // the dome's apex
    const Mesh stretched = stretched_dome(centre);
    const float before = edge_variation(stretched, centre, 0.45f);
    REQUIRE(before > 0.1f);  // the grab really did stretch it

    auto run = [&](MeshBrush verb) {
        Mesh m = stretched;
        MeshSculptor sc(m);
        MeshBrushSettings s;
        s.center = centre;
        s.radius = 0.5f;
        s.strength = 1.0f;
        s.smooth_iterations = 2;
        for (int i = 0; i < 8; ++i) sc.stamp(verb, s, {}, nullptr);
        return m;
    };
    const Mesh relaxed = run(MeshBrush::Relax);
    const Mesh smoothed = run(MeshBrush::Smooth);

    float relax_along = 0, relax_across = 0, smooth_along = 0, smooth_across = 0;
    split_motion(stretched, relaxed, centre, 0.45f, &relax_along, &relax_across);
    split_motion(stretched, smoothed, centre, 0.45f, &smooth_along, &smooth_across);
    CAPTURE(relax_along);
    CAPTURE(smooth_along);

    // Both even the triangulation...
    CHECK(edge_variation(relaxed, centre, 0.45f) < before);
    CHECK(edge_variation(smoothed, centre, 0.45f) < before);
    // ...and relax does it while moving the SURFACE a fraction as far.
    CHECK(relax_along < smooth_along * 0.5f);
    // Its motion is mostly across the surface rather than through it, which is
    // the property; it is not exactly zero, and the header says why.
    CHECK(relax_across > relax_along * 2.0f);
}

TEST_CASE("layer deposits to a ceiling where draw keeps going") {
    // Every other deposit verb accumulates, so a slow stroke digs deeper than a
    // fast one over the same path. Layer measures against the surface as the
    // STROKE found it and stops.
    const cfloat3 centre = cf3(0, 0.6f, 0);
    const Mesh base = dome_grid(48, 1.0f);

    auto run = [&](MeshBrush verb, int stamps) {
        Mesh m = base;
        MeshSculptor sc(m);
        VertexDeltas record;
        MeshBrushSettings s;
        s.center = centre;
        s.radius = 0.5f;
        s.strength = 0.15f;
        s.layer_height = 0.08f;
        s.deposit_normal = cf3(0, 1, 0);
        for (int i = 0; i < stamps; ++i) sc.stamp(verb, s, {}, &record);
        float along = 0, across = 0;
        split_motion(base, m, centre, 0.1f, &along, &across);
        return along;
    };

    const float layer_1 = run(MeshBrush::Layer, 1);
    const float layer_12 = run(MeshBrush::Layer, 12);
    const float draw_1 = run(MeshBrush::Draw, 1);
    const float draw_12 = run(MeshBrush::Draw, 12);
    CAPTURE(layer_1);
    CAPTURE(layer_12);
    CAPTURE(draw_12);

    // Layer converges on its ceiling and never passes it.
    CHECK(layer_12 > layer_1);
    CHECK(layer_12 <= 0.08f + 1e-4f);
    // Draw, at the same settings, is well past it.
    CHECK(draw_12 > 0.08f * 2.0f);
    CHECK(draw_12 > layer_12 * 3.0f);
    // ...and one stamp of each is comparable, so the difference really is the
    // accumulation rather than a strength mismatch in the fixture.
    CHECK(draw_1 < 0.08f);
}

TEST_CASE("layer without a stroke record is refused rather than becoming draw") {
    // With no record there is no stroke origin, so every stamp would clamp
    // against the CURRENT surface — which is draw wearing layer's name. A verb
    // that silently becomes a different verb is worse than one that refuses.
    Mesh m = dome_grid(24, 1.0f);
    const Mesh before = m;
    MeshSculptor sc(m);
    MeshBrushSettings s;
    s.center = cf3(0, 0.6f, 0);
    s.radius = 0.5f;
    s.layer_height = 0.08f;

    CHECK(sc.stamp(MeshBrush::Layer, s, {}, nullptr) == 0);
    CHECK(same_bytes(m.positions, before.positions));

    // With one, it works.
    VertexDeltas record;
    CHECK(sc.stamp(MeshBrush::Layer, s, {}, &record) > 0);
}

TEST_CASE("nudge moves material along the surface, where grab carries it off") {
    // The distinction from grab: grab applies the drag rigidly, so a drag with
    // a component off the surface lifts material off it. Nudge projects into
    // each vertex's own tangent plane.
    Mesh flat;
    {
        // A FLAT plane here on purpose: its tangent planes are exactly y = 0,
        // so "did any motion leave the surface" is an exact question.
        const int n = 24;
        const float step = 2.0f / static_cast<float>(n);
        for (int z = 0; z <= n; ++z)
            for (int x = 0; x <= n; ++x) {
                flat.positions.push_back(cf3(-1.0f + step * static_cast<float>(x), 0.0f,
                                             -1.0f + step * static_cast<float>(z)));
                flat.normals.push_back(cf3(0, 1, 0));
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x) {
                const std::uint32_t a =
                    static_cast<std::uint32_t>(z) * stride + static_cast<std::uint32_t>(x);
                const std::uint32_t b = a + 1, c = a + stride, d = c + 1;
                for (std::uint32_t i : {a, c, b, b, c, d}) flat.indices.push_back(i);
            }
    }

    // A drag half of which points straight off the plane.
    const cfloat3 drag = cf3(0.2f, 0.2f, 0);
    auto run = [&](MeshBrush verb) {
        Mesh m = flat;
        MeshSculptor sc(m);
        MeshBrushSettings s;
        s.center = cf3(0, 0, 0);
        s.radius = 0.5f;
        s.direction = drag;
        sc.stamp(verb, s, {}, nullptr);
        float off = 0, along = 0;
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            off = std::max(off, std::fabs(m.positions[i].y));
            along = std::max(along, std::fabs(m.positions[i].x - flat.positions[i].x));
        }
        return std::pair<float, float>{off, along};
    };

    const auto nudged = run(MeshBrush::Nudge);
    const auto grabbed = run(MeshBrush::Grab);
    // Nudge stayed exactly in the plane and still moved material across it.
    CHECK(nudged.first == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(nudged.second > 0.1f);
    // Grab, given the same drag, lifted it off.
    CHECK(grabbed.first > 0.1f);
}

TEST_CASE("an absent alpha leaves every verb exactly as it was") {
    // The additive claim. `alpha` defaults to null and multiplies the weight by
    // 1, so this is byte-identity rather than a tolerance.
    const Mesh base = dome_grid(32, 1.0f);
    for (MeshBrush verb : {MeshBrush::Draw, MeshBrush::Inflate, MeshBrush::Smooth, MeshBrush::Pinch,
                           MeshBrush::Clay, MeshBrush::Relax}) {
        Mesh a = base, b = base;
        MeshSculptor sa(a), sb(b);
        MeshBrushSettings s;
        s.center = cf3(0, 0.6f, 0);
        s.radius = 0.5f;
        s.strength = 0.4f;
        MeshBrushSettings with_null = s;
        with_null.alpha = nullptr;  // explicit, and the default

        sa.stamp(verb, s, {}, nullptr);
        sb.stamp(verb, with_null, {}, nullptr);
        CAPTURE(static_cast<int>(verb));
        REQUIRE(same_bytes(a.positions, b.positions));
    }
}

TEST_CASE("an alpha gates every verb through one multiplication") {
    // Folded into the WEIGHT rather than into each verb, so it composes with
    // all of them and with every falloff at no per-verb cost.
    const Mesh base = dome_grid(32, 1.0f);
    const std::vector<float> zeros(16 * 16, 0.0f);
    const std::vector<float> ones(16 * 16, 1.0f);

    for (MeshBrush verb : {MeshBrush::Draw, MeshBrush::Inflate, MeshBrush::Pinch}) {
        MeshBrushSettings s;
        s.center = cf3(0, 0.6f, 0);
        s.radius = 0.5f;
        s.strength = 0.4f;
        s.alpha_width = 16;
        s.alpha_height = 16;
        s.alpha_direction = cf3(0, 1, 0);
        s.alpha_tangent = cf3(1, 0, 0);
        CAPTURE(static_cast<int>(verb));

        // An all-zero stamp moves nothing at all.
        Mesh dead = base;
        MeshSculptor sd(dead);
        MeshBrushSettings zero = s;
        zero.alpha = zeros.data();
        CHECK(sd.stamp(verb, zero, {}, nullptr) == 0);
        CHECK(same_bytes(dead.positions, base.positions));

        // An all-ones stamp is the unmodulated brush.
        Mesh full = base, plain = base;
        MeshSculptor sf(full), sp(plain);
        MeshBrushSettings one = s;
        one.alpha = ones.data();
        sf.stamp(verb, one, {}, nullptr);
        sp.stamp(verb, s, {}, nullptr);
        for (std::size_t i = 0; i < full.positions.size(); ++i)
            REQUIRE(clength(full.positions[i] - plain.positions[i]) < 1e-6f);
    }
}

TEST_CASE("an alpha's shape reaches the mesh") {
    // Not just its magnitude: half the stamp masked off must move half the
    // region. A stamp that was sampled at the wrong coordinates would still
    // pass the all-zero and all-ones cases above.
    const Mesh base = dome_grid(48, 1.0f);
    std::vector<float> half(32 * 32, 0.0f);
    for (int y = 0; y < 32; ++y)
        for (int x = 16; x < 32; ++x) half[static_cast<std::size_t>(y) * 32 + x] = 1.0f;

    Mesh m = base;
    MeshSculptor sc(m);
    MeshBrushSettings s;
    s.center = cf3(0, 0.6f, 0);
    s.radius = 0.6f;
    s.strength = 0.4f;
    s.alpha = half.data();
    s.alpha_width = 32;
    s.alpha_height = 32;
    s.alpha_direction = cf3(0, 1, 0);
    s.alpha_tangent = cf3(1, 0, 0);  // u runs along +x
    s.deposit_normal = cf3(0, 1, 0);
    sc.stamp(MeshBrush::Draw, s, {}, nullptr);

    // The +u half moved; the -u half did not.
    float moved_plus = 0, moved_minus = 0;
    for (std::size_t i = 0; i < base.positions.size(); ++i) {
        const float d = clength(m.positions[i] - base.positions[i]);
        if (base.positions[i].x > 0.1f) moved_plus = std::max(moved_plus, d);
        if (base.positions[i].x < -0.1f) moved_minus = std::max(moved_minus, d);
    }
    CAPTURE(moved_plus);
    CAPTURE(moved_minus);
    CHECK(moved_plus > 0.01f);
    CHECK(moved_minus == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("the new verbs keep the standing mesh contract") {
    // Topology never changes, and a stroke reverts bit-exactly. The contract
    // every other verb is held to, checked for the three that are new.
    const Mesh base = dome_grid(32, 1.0f);
    for (MeshBrush verb : {MeshBrush::Relax, MeshBrush::Layer, MeshBrush::Nudge}) {
        Mesh m = base;
        MeshSculptor sc(m);
        VertexDeltas record;
        MeshBrushSettings s;
        s.center = cf3(0, 0.6f, 0);
        s.radius = 0.5f;
        s.strength = 0.6f;
        s.direction = cf3(0.1f, 0.05f, 0);
        s.layer_height = 0.05f;
        CAPTURE(static_cast<int>(verb));

        for (int i = 0; i < 4; ++i) sc.stamp(verb, s, {}, &record);
        REQUIRE(same_bytes(m.indices, base.indices));
        REQUIRE(same_bytes(m.quads, base.quads));
        CHECK(record.size() > 0);

        REQUIRE(record.revert(m));
        REQUIRE(same_bytes(m.positions, base.positions));
        REQUIRE(same_bytes(m.normals, base.normals));
    }
}

TEST_CASE("a DISPLACEMENT brush leaves vertex colours untouched") {
    // Stated as a test rather than left implicit (decide-surface-colour). It
    // used to be true by OMISSION — no verb wrote colour — and the point of
    // writing it down was that a colour brush would have to add colour writing
    // deliberately rather than by accident. Paint and Smear are that deliberate
    // act, so the claim is now about the verbs that MOVE vertices, which is
    // what lets an imported model keep its colours through a sculpt.
    Mesh base = dome_grid(24, 1.0f);
    base.colors.assign(base.positions.size(), cf3(0.2f, 0.6f, 0.9f));
    for (std::size_t i = 0; i < base.colors.size(); i += 3) base.colors[i] = cf3(0.9f, 0.1f, 0.1f);

    for (MeshBrush verb : {MeshBrush::Draw, MeshBrush::Grab, MeshBrush::Smooth, MeshBrush::Relax,
                           MeshBrush::Nudge, MeshBrush::Pinch}) {
        Mesh m = base;
        MeshSculptor sc(m);
        VertexDeltas record;
        MeshBrushSettings s;
        s.center = cf3(0, 0.6f, 0);
        s.radius = 0.5f;
        s.strength = 0.6f;
        s.direction = cf3(0.1f, 0.05f, 0);
        CAPTURE(static_cast<int>(verb));
        REQUIRE(sc.stamp(verb, s, {}, &record) > 0);  // it really did move something
        REQUIRE(same_bytes(m.colors, base.colors));
    }
}

// -- the colour pair (add-mesh-colour-brushes) --------------------------------
//
// Paint and Smear are the only verbs that do not move a vertex, and that is the
// property most worth pinning: a colour pass over a finished sculpt must not
// show up as a diff on the geometry.

namespace {

// A dome whose colour splits cleanly down the x = 0 line, so a smear across
// that line has something unambiguous to drag.
Mesh two_tone_dome(int n = 24, float half = 1.0f) {
    Mesh m = dome_grid(n, half);
    m.colors.resize(m.positions.size());
    for (std::size_t i = 0; i < m.positions.size(); ++i)
        m.colors[i] = m.positions[i].x < 0.0f ? cf3(1, 0, 0) : cf3(0, 0, 1);
    return m;
}

}  // namespace

TEST_CASE("a colour brush moves no vertex") {
    for (MeshBrush verb : {MeshBrush::Paint, MeshBrush::Smear}) {
        Mesh base = two_tone_dome();
        Mesh m = base;
        MeshSculptor sc(m);
        VertexDeltas record;
        MeshBrushSettings s = centred(cf3(0, 0.6f, 0), 0.5f, 0.8f);
        s.color = cf3(0, 1, 0);
        s.direction = cf3(0.15f, 0, 0);
        CAPTURE(static_cast<int>(verb));
        REQUIRE(sc.stamp(verb, s, {}, &record) > 0);  // it really did paint something
        CHECK(same_bytes(m.positions, base.positions));
        CHECK(same_bytes(m.normals, base.normals));
        CHECK_FALSE(same_bytes(m.colors, base.colors));
    }
}

TEST_CASE("a colour brush refuses a mesh with no colour attribute") {
    // Rather than allocating one: twelve bytes per vertex is a real cost to
    // hide behind a brush stroke, and a silent creation would make "I painted
    // and nothing happened" indistinguishable from "this mesh had no colours".
    Mesh m = dome_grid(16, 1.0f);
    REQUIRE(m.colors.empty());
    MeshSculptor sc(m);
    MeshBrushSettings s = centred(cf3(0, 0.6f, 0), 0.5f, 1.0f);
    s.color = cf3(1, 0, 0);
    CHECK(sc.stamp(MeshBrush::Paint, s, {}, nullptr) == 0);
    CHECK(m.colors.empty());  // and it did not quietly create one

    SUBCASE("and paints once the host asks for one") {
        CHECK(sc.ensure_colors(cf3(1, 1, 1)));
        CHECK(m.colors.size() == m.positions.size());
        CHECK_FALSE(sc.ensure_colors(cf3(0, 0, 0)));  // already there, left alone
        CHECK(m.colors[0].x == 1.0f);
        CHECK(sc.stamp(MeshBrush::Paint, s, {}, nullptr) > 0);
    }
}

TEST_CASE("paint reaches its target exactly at full weight") {
    // The ends of a blend have to be EXACT. mix(a, b, 1) is a + (b - a) * 1,
    // which is not b in floating point, so a fully-weighted dab would leave a
    // one-ULP seam along the rim of every stroke.
    Mesh m = two_tone_dome();
    MeshSculptor sc(m);
    MeshBrushSettings s = centred(cf3(0, 0.6f, 0), 0.6f, 1.0f);
    s.falloff = mesh::MeshFalloff::Constant;  // weight 1 everywhere it reaches
    s.color = cf3(0.25f, 0.5f, 0.75f);
    REQUIRE(sc.stamp(MeshBrush::Paint, s, {}, nullptr) > 0);

    std::size_t exact = 0;
    for (std::size_t i = 0; i < m.colors.size(); ++i)
        if (std::memcmp(&m.colors[i], &s.color, sizeof(cfloat3)) == 0) ++exact;
    CHECK(exact > 0);  // bit-identical to the target, not merely close
}

TEST_CASE("paint falls off from the centre and leaves the rim alone") {
    Mesh base = two_tone_dome();
    Mesh m = base;
    MeshSculptor sc(m);
    MeshBrushSettings s = centred(cf3(0, 0.6f, 0), 0.4f, 0.9f);
    s.color = cf3(0, 1, 0);
    REQUIRE(sc.stamp(MeshBrush::Paint, s, {}, nullptr) > 0);

    // Somewhere well outside the brush is untouched, and the vertex nearest the
    // centre moved further toward the target than one near the edge.
    float near_green = 0.0f, far_green = 1.0f;
    float best_near = 1e9f, best_far = 1e9f;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const cfloat3 p = m.positions[i];
        const float d = std::sqrt((p.x - s.center.x) * (p.x - s.center.x) +
                                  (p.z - s.center.z) * (p.z - s.center.z));
        if (d < best_near) { best_near = d; near_green = m.colors[i].y; }
        if (std::abs(d - 0.38f) < best_far) { best_far = std::abs(d - 0.38f); far_green = m.colors[i].y; }
        if (d > 0.8f) CHECK(std::memcmp(&m.colors[i], &base.colors[i], sizeof(cfloat3)) == 0);
    }
    CHECK(near_green > far_green);
}

TEST_CASE("a paint stroke reverts bit-identically") {
    Mesh base = two_tone_dome();
    Mesh m = base;
    MeshSculptor sc(m);
    VertexDeltas record;
    MeshBrushSettings s = centred(cf3(0, 0.6f, 0), 0.5f, 0.7f);
    s.color = cf3(0, 1, 0);
    for (int i = 0; i < 5; ++i) {
        s.center = cf3(-0.2f + 0.1f * static_cast<float>(i), 0.6f, 0);
        REQUIRE(sc.stamp(MeshBrush::Paint, s, {}, &record) > 0);
    }
    REQUIRE_FALSE(same_bytes(m.colors, base.colors));

    Mesh painted = m;
    REQUIRE(record.revert(m));
    CHECK(same_bytes(m.colors, base.colors));
    CHECK(same_bytes(m.positions, base.positions));
    REQUIRE(record.apply(m));
    CHECK(same_bytes(m.colors, painted.colors));
}

TEST_CASE("smear drags colour in the drag direction and is not a smooth") {
    // The boundary sits at x = 0. Dragging +x has to carry red across it; a
    // symmetric blur would move the boundary nowhere.
    // The red CENTROID rather than red mass past a line: one stamp carries
    // colour about one edge length, and the boundary column sits at exactly
    // x == 0, so a strict `x > 0` test measures the one place the first stamp
    // cannot reach.
    auto red_centroid_x = [](const Mesh& m) {
        float mass = 0.0f, moment = 0.0f;
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            mass += m.colors[i].x;
            moment += m.colors[i].x * m.positions[i].x;
        }
        return mass > 0.0f ? moment / mass : 0.0f;
    };

    Mesh base = two_tone_dome();
    const float before = red_centroid_x(base);

    Mesh m = base;
    MeshSculptor sc(m);
    MeshBrushSettings s = centred(cf3(0, 0.75f, 0), 0.5f, 1.0f);
    s.direction = cf3(0.2f, 0, 0);
    REQUIRE(sc.stamp(MeshBrush::Smear, s, {}, nullptr) > 0);
    CHECK(red_centroid_x(m) > before);

    SUBCASE("dragging the other way carries blue the other way") {
        Mesh back = base;
        MeshSculptor sc2(back);
        MeshBrushSettings t = s;
        t.direction = cf3(-0.2f, 0, 0);
        REQUIRE(sc2.stamp(MeshBrush::Smear, t, {}, nullptr) > 0);
        CHECK(red_centroid_x(back) < before);
    }

    SUBCASE("no direction is no smear, rather than a smooth") {
        Mesh still = base;
        MeshSculptor sc3(still);
        MeshBrushSettings t = s;
        t.direction = cf3(0, 0, 0);
        CHECK(sc3.stamp(MeshBrush::Smear, t, {}, nullptr) == 0);
        CHECK(same_bytes(still.colors, base.colors));
    }
}

TEST_CASE("a colour brush is deterministic") {
    Mesh a = two_tone_dome(), b = two_tone_dome();
    for (MeshBrush verb : {MeshBrush::Paint, MeshBrush::Smear}) {
        Mesh ma = a, mb = b;
        MeshSculptor sa(ma), sb(mb);
        MeshBrushSettings s = centred(cf3(0.05f, 0.7f, -0.02f), 0.45f, 0.65f);
        s.color = cf3(0.1f, 0.9f, 0.3f);
        s.direction = cf3(0.13f, 0.02f, -0.07f);
        CAPTURE(static_cast<int>(verb));
        REQUIRE(sa.stamp(verb, s, {}, nullptr) > 0);
        REQUIRE(sb.stamp(verb, s, {}, nullptr) > 0);
        CHECK(same_bytes(ma.colors, mb.colors));
    }
}
