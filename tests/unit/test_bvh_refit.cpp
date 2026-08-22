// Refitting the mesh BVH when vertices move (add-bvh-refit).
//
// A mesh layer's topology is fixed, so a brush that moves vertices leaves the
// tree's SHAPE a valid partition of the same triangles and only its bounds
// stale. These tests hold the two properties that makes safe:
//
//   CONSERVATIVE — every node's box contains the triangles beneath it. This is
//   the one that matters. A box left too small does not make a query slow, it
//   makes it WRONG, by pruning a subtree that should have been descended, and
//   nothing about the answer says so.
//
//   EQUIVALENT — a refitted tree answers like a rebuilt one. Bit-identity is
//   not the claim and cannot be: boxes are exact, because a union of unions is
//   the same union and min/max do not round, but the winding-number dipole is a
//   float sum and addition is not associative, so combining two subtree totals
//   differs in the last bits from summing a span. The tests below assert
//   exactness where it exists and tolerance where it does not, separately, so
//   neither claim can quietly weaken into the other.

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "clay/mesh/bvh.h"
#include "clay/mesh/mesh_data.h"

using namespace clay;
using namespace clay::mesh;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// A closed sphere: the winding number needs an inside, and an open sheet has
// none — the trap this repo has walked into more than once.
Mesh sphere(int rings = 24, int segments = 32, float r = 1.0f) {
    Mesh m;
    for (int i = 0; i <= rings; ++i) {
        const float phi = 3.14159265f * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segments; ++j) {
            const float th = 6.2831853f * static_cast<float>(j) / static_cast<float>(segments);
            m.positions.push_back(cf3(r * std::sin(phi) * std::cos(th), r * std::cos(phi),
                                      r * std::sin(phi) * std::sin(th)));
        }
    }
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            const std::uint32_t a = static_cast<std::uint32_t>(i * segments + j);
            const std::uint32_t b =
                static_cast<std::uint32_t>(i * segments + (j + 1) % segments);
            const std::uint32_t c = a + static_cast<std::uint32_t>(segments);
            const std::uint32_t d = b + static_cast<std::uint32_t>(segments);
            for (std::uint32_t k : {a, c, b, b, c, d}) m.indices.push_back(k);
        }
    return m;
}

// Push every vertex within `reach` of `centre` outward along its own direction,
// the way a draw brush would.
std::vector<std::uint32_t> bump(Mesh* m, cfloat3 centre, float reach, float amount) {
    std::vector<std::uint32_t> moved_verts;
    for (std::size_t v = 0; v < m->positions.size(); ++v) {
        const cfloat3 d = m->positions[v] - centre;
        if (kernel::clength(d) > reach) continue;
        const float len = kernel::clength(m->positions[v]);
        if (len > 1e-9f) m->positions[v] = m->positions[v] * ((len + amount) / len);
        moved_verts.push_back(static_cast<std::uint32_t>(v));
    }
    return moved_verts;
}

// The triangles incident to any of `verts` — exactly the set that changed.
std::vector<std::uint32_t> triangles_touching(const Mesh& m,
                                              const std::vector<std::uint32_t>& verts) {
    std::vector<char> is_moved(m.positions.size(), 0);
    for (std::uint32_t v : verts) is_moved[v] = 1;
    std::vector<std::uint32_t> out;
    for (std::size_t t = 0; t < m.triangle_count(); ++t)
        if (is_moved[m.indices[t * 3]] || is_moved[m.indices[t * 3 + 1]] ||
            is_moved[m.indices[t * 3 + 2]])
            out.push_back(static_cast<std::uint32_t>(t));
    return out;
}

std::vector<cfloat3> probe_points(int n, float radius, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-radius, radius);
    std::vector<cfloat3> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(cf3(d(rng), d(rng), d(rng)));
    return out;
}

}  // namespace

TEST_CASE("bvh refit: a refitted tree answers for the moved surface") {
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);

    const std::vector<std::uint32_t> verts = bump(&m, cf3(0, 1, 0), 0.6f, 0.35f);
    REQUIRE(!verts.empty());
    const std::vector<std::uint32_t> tris = triangles_touching(m, verts);
    REQUIRE(!tris.empty());
    REQUIRE(tree.refit(m, tris.data(), tris.size()));

    const Bvh fresh = Bvh::build(m);
    double worst_distance = 0.0, worst_exact_winding = 0.0;
    double refit_error = 0.0, fresh_error = 0.0;
    int inside_disagreements = 0;
    for (cfloat3 p : probe_points(600, 1.6f, 7)) {
        worst_distance = std::max<double>(
            worst_distance, std::fabs(tree.unsigned_distance(p) - fresh.unsigned_distance(p)));
        // beta = 0 sums every triangle rather than summarising any node, so
        // this compares the GEOMETRY the two trees hold, with no approximation
        // in the way.
        worst_exact_winding =
            std::max<double>(worst_exact_winding, std::fabs(tree.winding_number(p, 0.0f) -
                                                            fresh.winding_number(p, 0.0f)));
        if (tree.is_inside(p) != fresh.is_inside(p)) ++inside_disagreements;
        // How far each tree's dipole approximation sits from its own exact sum.
        // This is the question worth asking about a refit: are the summaries as
        // good as a rebuild's? Comparing the two APPROXIMATIONS to each other
        // is not — a fresh build over moved positions partitions differently,
        // so the two summarise different nodes and disagree by the size of the
        // approximation itself, which says nothing about either.
        refit_error = std::max<double>(
            refit_error, std::fabs(tree.winding_number(p) - tree.winding_number(p, 0.0f)));
        fresh_error = std::max<double>(
            fresh_error, std::fabs(fresh.winding_number(p) - fresh.winding_number(p, 0.0f)));
    }
    // The distance query reads only boxes and triangles. Boxes a refit
    // reproduces EXACTLY — a union of unions is the same union and min/max do
    // not round — so this one is exact.
    CHECK(worst_distance == 0.0);
    // The geometry agrees to float rounding: the dipole is a sum, and addition
    // is not associative, so a different traversal order lands a few ulps away.
    CHECK(worst_exact_winding < 1e-5);
    CHECK(inside_disagreements == 0);
    // The refitted tree's approximation is no worse than a rebuilt tree's,
    // which is the claim that matters and the one that would fail if `combine`
    // were wrong.
    CHECK(refit_error < fresh_error * 1.5 + 1e-6);
    MESSAGE("exact-winding delta " << worst_exact_winding << "; approximation error refitted "
                                   << refit_error << " vs rebuilt " << fresh_error);
}

TEST_CASE("bvh refit: the closest point and the ray agree with a rebuild") {
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);
    const std::vector<std::uint32_t> verts = bump(&m, cf3(0.8f, 0, 0.4f), 0.5f, -0.25f);
    const std::vector<std::uint32_t> tris = triangles_touching(m, verts);
    REQUIRE(tree.refit(m, tris.data(), tris.size()));
    const Bvh fresh = Bvh::build(m);

    int checked = 0;
    for (cfloat3 p : probe_points(300, 1.5f, 11)) {
        const Bvh::ClosestPoint a = tree.closest(p), b = fresh.closest(p);
        REQUIRE(a.found == b.found);
        if (!a.found) continue;
        ++checked;
        // Tolerance, not identity, and the reason is not float noise: a build
        // over the MOVED positions partitions differently, so the two trees are
        // different trees. Where two triangles are equidistant they break the
        // tie differently and report different indices — both correct. What
        // must agree is the distance.
        CHECK(a.distance == doctest::Approx(b.distance).epsilon(1e-5));

        math::Ray ray;
        ray.origin = p;
        const cfloat3 to_origin = cf3(0, 0, 0) - p;
        const float to_len = kernel::clength(to_origin);
        if (!(to_len > 1e-6f)) continue;
        ray.dir = to_origin * (1.0f / to_len);
        const Bvh::RayHit ha = tree.raycast(ray), hb = fresh.raycast(ray);
        REQUIRE(ha.hit == hb.hit);
        if (ha.hit) CHECK(ha.t == doctest::Approx(hb.t).epsilon(1e-5));
    }
    CHECK(checked > 200);
}

TEST_CASE("bvh refit: bounds stay conservative") {
    // The property a wrong recomputation ORDER breaks silently — a parent
    // combined before its children keeps the pre-move box, which is too small
    // exactly where the brush pushed material out of it.
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);
    const std::vector<std::uint32_t> verts = bump(&m, cf3(0, -1, 0), 0.7f, 0.5f);
    const std::vector<std::uint32_t> tris = triangles_touching(m, verts);
    REQUIRE(tree.refit(m, tris.data(), tris.size()));
    CHECK(tree.bounds_contain_their_triangles(&m));
}

TEST_CASE("bvh refit: naming a superset matches naming exactly the moved set") {
    Mesh a = sphere(12, 16);
    Mesh b = a;
    Bvh exact = Bvh::build(a);
    Bvh superset = Bvh::build(b);

    const std::vector<std::uint32_t> verts = bump(&a, cf3(0, 1, 0), 0.5f, 0.3f);
    b.positions = a.positions;
    const std::vector<std::uint32_t> tris = triangles_touching(a, verts);
    REQUIRE(exact.refit(a, tris.data(), tris.size()));
    REQUIRE(superset.refit(b));  // every triangle, most of which did not move

    for (cfloat3 p : probe_points(200, 1.5f, 3)) {
        CHECK(exact.unsigned_distance(p) == superset.unsigned_distance(p));
        CHECK(std::fabs(exact.winding_number(p) - superset.winding_number(p)) < 1e-5);
    }
}

TEST_CASE("bvh refit: a mismatched mesh is refused and changes nothing") {
    Mesh m = sphere(8, 8);
    Bvh tree = Bvh::build(m);
    const float before = tree.unsigned_distance(cf3(2, 0, 0));

    Mesh other = sphere(9, 8);  // a different triangle count
    CHECK_FALSE(tree.refit(other));
    CHECK(tree.unsigned_distance(cf3(2, 0, 0)) == before);
}

TEST_CASE("bvh refit: degenerate input does not produce a degenerate tree") {
    SUBCASE("a single triangle") {
        Mesh m;
        m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0)};
        m.indices = {0, 1, 2};
        Bvh tree = Bvh::build(m);
        m.positions[2] = cf3(0, 3, 0);
        REQUIRE(tree.refit(m));
        CHECK(tree.bounds_contain_their_triangles(&m));
        CHECK(tree.unsigned_distance(cf3(0, 3, 0)) == doctest::Approx(0.0f).epsilon(1e-5));
    }
    SUBCASE("zero-area triangles have no centroid to weight") {
        Mesh m;
        for (int i = 0; i < 12; ++i) m.positions.push_back(cf3(float(i), 0, 0));
        for (std::uint32_t i = 0; i + 2 < 12; ++i) {
            m.indices.push_back(i);       // all three corners collinear:
            m.indices.push_back(i + 1);   // every triangle has zero area
            m.indices.push_back(i + 2);
        }
        Bvh tree = Bvh::build(m);
        for (auto& p : m.positions) p = p + cf3(0, 0.5f, 0);
        REQUIRE(tree.refit(m));
        CHECK(tree.bounds_contain_their_triangles(&m));
        // The dipole is meaningless here and must at least be finite.
        CHECK(std::isfinite(tree.winding_number(cf3(5, 5, 5))));
    }
    SUBCASE("a mesh whose bad indices were dropped at build time") {
        Mesh m = sphere(6, 6);
        m.indices[0] = 99999;  // drops triangle 0 from the tree
        Bvh tree = Bvh::build(m);
        CHECK(tree.triangle_count() == m.triangle_count() - 1);
        m.indices[0] = 0;  // and now it is valid again, but the tree has no slot
        for (auto& p : m.positions) p = p * 1.1f;
        REQUIRE(tree.refit(m));  // must not write through the missing slot
        CHECK(tree.bounds_contain_their_triangles(&m));
    }
    SUBCASE("an empty mesh") {
        Mesh m;
        Bvh tree = Bvh::build(m);
        CHECK(tree.refit(m));
    }
}

TEST_CASE("bvh refit: stretching a mesh raises the reported query cost") {
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);
    const float at_build = tree.quality();

    // A long thin pull: the deformation that swells the boxes along the arm, so
    // a ray descends both children where it used to descend one.
    const std::vector<std::uint32_t> verts = bump(&m, cf3(0, 1, 0), 0.35f, 8.0f);
    const std::vector<std::uint32_t> tris = triangles_touching(m, verts);
    REQUIRE(tree.refit(m, tris.data(), tris.size()));

    CHECK(tree.quality() > at_build);
    MESSAGE("query cost " << at_build << " -> " << tree.quality());
}

TEST_CASE("bvh refit: a rebuild is not the remedy, and the docs must not imply it is") {
    // Written because the intuition is strong and wrong, and it was wrong in
    // this change's own first draft: the spec said a rebuild "restores the
    // score". Measured, it usually does not. Median split partitions on
    // triangle CENTROIDS, so a deformation that makes triangles long defeats a
    // fresh build as thoroughly as it defeats an existing partition — and often
    // more, because the refit keeps a clustering chosen while the geometry was
    // still compact.
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);
    const std::vector<std::uint32_t> verts = bump(&m, cf3(0, 1, 0), 0.35f, 8.0f);
    const std::vector<std::uint32_t> tris = triangles_touching(m, verts);
    REQUIRE(tree.refit(m, tris.data(), tris.size()));

    const float refitted = tree.quality();
    const float rebuilt = Bvh::build(m).quality();
    CHECK(rebuilt > refitted);
    MESSAGE("after a long pull: refitted " << refitted << ", rebuilt " << rebuilt);
}

TEST_CASE("bvh refit: a uniform scale changes nothing the measure can see") {
    // The measure is normalised by the root box, and a uniform scale grows
    // every leaf and the root by the same factor. A partition that was good
    // stays exactly as good, and the number says so — which is what makes it
    // comparable across meshes rather than a function of how big they are.
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);
    const float before = tree.quality();
    for (cfloat3& p : m.positions) p = p * 4.0f;
    REQUIRE(tree.refit(m));
    CHECK(tree.quality() == doctest::Approx(before).epsilon(1e-4));
}

TEST_CASE("bvh refit: a stale tree is wrong in the way the header says") {
    // The docstring this change corrects claimed a stale tree reports the
    // surface as it was. It does not: the hit follows the moved triangle but is
    // found through stale bounds, so it lands OFF the ray. Pinned here because
    // a wrong explanation in a header is what sent a reader looking in the
    // wrong place once already.
    Mesh m = sphere();
    Bvh tree = Bvh::build(m);
    const std::vector<std::uint32_t> verts = bump(&m, cf3(0, 1, 0), 0.6f, 0.4f);
    const std::vector<std::uint32_t> tris = triangles_touching(m, verts);

    math::Ray ray;
    ray.origin = cf3(0.11f, 4.0f, 0.07f);
    ray.dir = cf3(0, -1, 0);
    // What pick::raycast_mesh reports as the hit POSITION: the triangle's
    // barycentrics against the mesh's CURRENT vertices, not origin + dir*t. So
    // a stale tree gives old barycentrics on a moved triangle, and the point
    // they interpolate is no longer on the ray at all.
    auto surface_point = [&](const Bvh::RayHit& h) {
        const std::uint32_t i0 = m.indices[h.triangle * 3];
        const std::uint32_t i1 = m.indices[h.triangle * 3 + 1];
        const std::uint32_t i2 = m.indices[h.triangle * 3 + 2];
        return m.positions[i0] * (1.0f - h.u - h.v) + m.positions[i1] * h.u +
               m.positions[i2] * h.v;
    };
    auto off_ray = [&](const Bvh::RayHit& h) {
        const cfloat3 p = surface_point(h);
        return std::max(std::fabs(p.x - ray.origin.x), std::fabs(p.z - ray.origin.z));
    };

    const Bvh::RayHit stale = tree.raycast(ray);
    REQUIRE(stale.hit);
    const float drift_before = off_ray(stale);

    REQUIRE(tree.refit(m, tris.data(), tris.size()));
    const Bvh::RayHit fitted = tree.raycast(ray);
    REQUIRE(fitted.hit);
    const float drift_after = off_ray(fitted);

    CHECK(drift_before > drift_after);
    CHECK(drift_after < 1e-5f);
    MESSAGE("off-ray drift " << drift_before << " -> " << drift_after);
}

// -- through the sculptor ----------------------------------------------------
//
// The tests above exercise `Bvh::refit` directly, where the caller names the
// changed triangles and can be trusted to name them all. `MeshSculptor` is the
// caller that has to work that set out for itself, and getting it wrong is
// SILENT: the tree stays self-consistent against its own stored triangles, so
// `bounds_contain_their_triangles()` still returns true while queries answer
// for geometry that has moved.

#include "clay/mesh/adjacency.h"
#include "clay/mesh/sculpt.h"

namespace {

// How far the sculptor's tree disagrees with one built for the mesh as it is
// now. Zero is the only acceptable answer after a refit.
double tree_disagreement(MeshSculptor& s, const Mesh& m) {
    const Bvh fresh = Bvh::build(m);
    double worst = 0.0;
    for (cfloat3 p : probe_points(400, 1.8f, 23))
        worst = std::max<double>(
            worst, std::fabs(s.bvh().unsigned_distance(p) - fresh.unsigned_distance(p)));
    return worst;
}

}  // namespace

TEST_CASE("sculptor refit: one stamp") {
    Mesh m = sphere();
    MeshSculptor s(m, Adjacency::build(m));
    (void)s.bvh();  // build the tree before the edit, as a host's first pick does

    MeshBrushSettings settings;
    settings.center = cf3(0, 1, 0);
    settings.radius = 0.4f;
    settings.strength = 0.3f;
    REQUIRE(s.stamp(MeshBrush::Draw, settings) > 0);
    s.refit_bvh();
    CHECK(tree_disagreement(s, m) == 0.0);
}

TEST_CASE("sculptor refit: a STROKE of many stamps, then one refit") {
    // The regression for the defect this change shipped with in its first
    // draft, and the reason it was worth an adversarial pass: `refit_bvh` read
    // `region_`, which every stamp overwrites, so a stroke refitted only its
    // final dab and left every earlier one holding pre-stroke bounds. It
    // reported success, and the conservativeness check passed, because the tree
    // was consistent with its own stale copy of the triangles.
    //
    // Measured before the fix on a 2,400-triangle sphere: a 15-stamp stroke
    // left 175 triangles at pre-stroke positions and the worst distance query
    // was off by 0.153.
    Mesh m = sphere();
    MeshSculptor s(m, Adjacency::build(m));
    (void)s.bvh();

    for (int i = 0; i < 12; ++i) {
        const float t = static_cast<float>(i) / 11.0f;
        MeshBrushSettings settings;
        // A stroke that WALKS, so each stamp's region is a different set and
        // the last one cannot stand in for the rest.
        settings.center = cf3(std::sin(t * 2.5f) * 0.9f, std::cos(t * 2.5f) * 0.9f, 0.2f);
        settings.radius = 0.3f;
        settings.strength = 0.25f;
        s.stamp(MeshBrush::Draw, settings);
    }
    s.refit_bvh();
    CHECK(tree_disagreement(s, m) == 0.0);
    CHECK(s.bvh().bounds_contain_their_triangles(&m));
}

TEST_CASE("sculptor refit: refresh clears what a refit owed") {
    Mesh m = sphere();
    MeshSculptor s(m, Adjacency::build(m));
    (void)s.bvh();
    MeshBrushSettings settings;
    settings.center = cf3(0, 1, 0);
    settings.radius = 0.4f;
    settings.strength = 0.3f;
    s.stamp(MeshBrush::Draw, settings);
    s.refresh_bvh();          // covers everything
    s.refit_bvh();            // ...so this must be a no-op, not a stale replay
    CHECK(tree_disagreement(s, m) == 0.0);
}

TEST_CASE("sculptor refit: a whole-mesh deformer is not a brush region") {
    // `apply_lattice` and `apply_deformer` move every vertex, so there is no
    // small dirty set to name. A refit derived from the last stamp's region
    // would refit a handful of triangles and leave the rest of the tree behind
    // — correct-looking and wrong.
    Mesh m = sphere();
    MeshSculptor s(m, Adjacency::build(m));
    (void)s.bvh();

    MeshDeformSettings deform;
    deform.verb = MeshDeform::Twist;
    deform.origin = cf3(0, 0, 0);
    deform.axis = cf3(0, 1, 0);
    deform.span = 2.0f;
    deform.angle = 1.0f;
    REQUIRE(s.apply_deformer(deform, {}) > 0);
    s.refit_bvh();
    CHECK(tree_disagreement(s, m) == 0.0);
    CHECK(s.bvh().bounds_contain_their_triangles(&m));
}

TEST_CASE("sculptor refit: refitting without a tree builds nothing") {
    Mesh m = sphere(8, 8);
    MeshSculptor s(m, Adjacency::build(m));
    MeshBrushSettings settings;
    settings.center = cf3(0, 1, 0);
    settings.radius = 0.5f;
    settings.strength = 0.2f;
    s.stamp(MeshBrush::Draw, settings);
    s.refit_bvh();  // no tree yet: must not build one behind the caller's back
    // ...and the tree the next query builds is correct, because it is built
    // from the mesh as it is now.
    CHECK(tree_disagreement(s, m) == 0.0);
}

TEST_CASE("bvh refit: quality says nothing rather than saying zero") {
    // Zero is the BEST end of this scale, so a degenerate tree returning it
    // would read as "queries got cheaper" — the reading a host acts on. NaN
    // makes every comparison false instead, which is the honest answer to a
    // question that has none.
    // A box needs TWO flat axes to have no surface area, so a plane is not
    // enough — this is a LINE, which a collapsed region really can produce.
    Mesh flat;
    flat.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(2, 0, 0), cf3(3, 0, 0)};
    flat.indices = {0, 1, 2, 1, 2, 3};
    const Bvh tree = Bvh::build(flat);
    const float q = tree.quality();
    CHECK(std::isnan(q));
    CHECK_FALSE(q > 0.0f);  // the comparison a host would make, and it is false
}

// -- the tree as a spatial index (issue #192) ---------------------------------
//
// The brush region and the walk's seed used to be linear scans over every weld
// class. They ask the tree now, so the tree's answers have to be EXACT: a
// region query that misses a vertex is a brush that does not move it, and no
// tolerance makes that acceptable. Both cases below check against brute force.

TEST_CASE("bvh index: triangles_in_ball misses nothing") {
    Mesh m = sphere(20, 28);
    const Bvh tree = Bvh::build(m);
    std::vector<std::uint32_t> got;

    for (float radius : {0.05f, 0.2f, 0.7f, 3.0f}) {
        for (cfloat3 centre : probe_points(25, 1.3f, 31)) {
            tree.triangles_in_ball(centre, radius, &got);
            std::vector<char> reported(m.triangle_count(), 0);
            for (std::uint32_t t : got) {
                REQUIRE(t < m.triangle_count());
                CHECK_FALSE(reported[t]);  // and no duplicates
                reported[t] = 1;
            }
            // Every triangle with a VERTEX in the ball must be reported. The
            // query may also report triangles that merely reach it, which a
            // caller filters — over-admitting is the safe direction.
            const float r2 = radius * radius;
            for (std::size_t t = 0; t < m.triangle_count(); ++t) {
                bool inside = false;
                for (int k = 0; k < 3; ++k)
                    if (kernel::cdot2(m.positions[m.indices[t * 3 + k]] - centre) <= r2)
                        inside = true;
                if (inside) REQUIRE(reported[t]);
            }
        }
    }
}

TEST_CASE("bvh index: nearest_vertex agrees with a brute-force scan") {
    Mesh m = sphere(16, 20);
    const Bvh tree = Bvh::build(m);
    for (cfloat3 p : probe_points(200, 2.0f, 37)) {
        const Bvh::NearestVertex hit = tree.nearest_vertex(p);
        REQUIRE(hit.found);
        float brute = std::numeric_limits<float>::max();
        for (std::size_t t = 0; t < m.triangle_count(); ++t)
            for (int k = 0; k < 3; ++k)
                brute = std::min(brute, kernel::cdot2(m.positions[m.indices[t * 3 + k]] - p));
        // The DISTANCE must match exactly; which of several coincident vertices
        // is named may differ, and a weld class makes those interchangeable.
        CHECK(hit.distance * hit.distance == doctest::Approx(brute).epsilon(1e-6));
        const std::uint32_t v =
            m.indices[static_cast<std::size_t>(hit.triangle) * 3 + hit.corner];
        CHECK(kernel::cdot2(m.positions[v] - p) == doctest::Approx(brute).epsilon(1e-6));
    }
}

TEST_CASE("bvh index: an empty tree answers rather than crashing") {
    Mesh empty;
    const Bvh tree = Bvh::build(empty);
    std::vector<std::uint32_t> got{7, 7, 7};
    tree.triangles_in_ball(cf3(0, 0, 0), 1.0f, &got);
    CHECK(got.empty());
    CHECK_FALSE(tree.nearest_vertex(cf3(0, 0, 0)).found);
}

TEST_CASE("sculptor index: the region is the same set with and without a tree") {
    // The fallback and the indexed path must agree exactly. They are the same
    // question asked two ways, and if they ever diverge a brush would behave
    // differently depending on whether the host happened to have picked.
    for (bool geodesic : {false, true}) {
        Mesh a = sphere(18, 24);
        Mesh b = a;
        MeshSculptor scanned(a, Adjacency::build(a));   // never picks: no tree
        MeshSculptor indexed(b, Adjacency::build(b));
        (void)indexed.bvh();                            // a host that picks

        MeshBrushSettings settings;
        settings.center = cf3(0.3f, 0.9f, 0.1f);
        settings.radius = 0.45f;
        settings.strength = 0.4f;
        settings.geodesic = geodesic;
        const std::size_t moved_scanned = scanned.stamp(MeshBrush::Draw, settings);
        const std::size_t moved_indexed = indexed.stamp(MeshBrush::Draw, settings);
        CHECK(moved_scanned == moved_indexed);
        REQUIRE(a.positions.size() == b.positions.size());
        float worst = 0.0f;
        for (std::size_t v = 0; v < a.positions.size(); ++v)
            worst = std::max(worst, kernel::clength(a.positions[v] - b.positions[v]));
        CHECK(worst == 0.0f);
    }
}
