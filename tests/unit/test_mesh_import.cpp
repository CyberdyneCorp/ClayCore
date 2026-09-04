// Importing a mesh as a field (meshing spec, add-mesh-to-field-import).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "clay/field/volume.h"
#include "clay/mesh/bvh.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/to_field.h"
#include "clay/scene/tape.h"

using namespace clay;
using kernel::cf3;
using mesh::Bvh;
using mesh::Mesh;

namespace {

// Not M_PI: that is a POSIX extension, and MSVC defines it only when
// _USE_MATH_DEFINES is set before <cmath> — which a header included earlier in
// the translation unit can silently defeat. A constant cannot be undefined.
constexpr float kPi = 3.14159265358979323846f;

// A UV sphere. `rings` controls the tessellation, which is also how the tests
// tell an approximation error from a tessellation error.
Mesh sphere_mesh(float r = 1.0f, int rings = 24, kernel::cfloat3 centre = kernel::cf3(0, 0, 0)) {
    Mesh m;
    const int segments = rings * 2;
    for (int i = 0; i <= rings; ++i) {
        float phi = kPi * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j <= segments; ++j) {
            float theta =
                2.0f * kPi * static_cast<float>(j) / static_cast<float>(segments);
            m.positions.push_back(centre + cf3(r * std::sin(phi) * std::cos(theta),
                                               r * std::cos(phi),
                                               r * std::sin(phi) * std::sin(theta)));
        }
    }
    auto at = [&](int i, int j) { return static_cast<std::uint32_t>(i * (segments + 1) + j); };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            // Wound so the normals point outward, which the winding number uses
            // for its sign — and which one of the tests deliberately reverses.
            m.indices.insert(m.indices.end(), {at(i, j), at(i, j + 1), at(i + 1, j)});
            m.indices.insert(m.indices.end(), {at(i, j + 1), at(i + 1, j + 1), at(i + 1, j)});
        }
    return m;
}

Mesh box_mesh(kernel::cfloat3 half, kernel::cfloat3 centre = kernel::cf3(0, 0, 0)) {
    Mesh m;
    for (int i = 0; i < 8; ++i)
        m.positions.push_back(centre + cf3((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y,
                                           (i & 4) ? half.z : -half.z));
    // Outward-wound faces of a unit cube by corner index.
    const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                             {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        m.indices.insert(m.indices.end(), {static_cast<std::uint32_t>(f[0]),
                                           static_cast<std::uint32_t>(f[1]),
                                           static_cast<std::uint32_t>(f[2])});
        m.indices.insert(m.indices.end(), {static_cast<std::uint32_t>(f[0]),
                                           static_cast<std::uint32_t>(f[2]),
                                           static_cast<std::uint32_t>(f[3])});
    }
    return m;
}

Mesh merged(const Mesh& a, const Mesh& b) {
    Mesh m = a;
    std::uint32_t offset = static_cast<std::uint32_t>(a.positions.size());
    m.positions.insert(m.positions.end(), b.positions.begin(), b.positions.end());
    for (std::uint32_t i : b.indices) m.indices.push_back(i + offset);
    return m;
}

}  // namespace

TEST_CASE("mesh import: distance matches the analytic shape") {
    const float r = 1.0f;
    const int rings = 48;
    Bvh bvh = Bvh::build(sphere_mesh(r, rings));

    // The tolerance is the SAGITTA, not a percentage. A facet is a chord, so a
    // tessellated sphere deviates from the analytic one by r(1 - cos(half the
    // angular step)) — an absolute error that does not shrink as the query
    // approaches the surface, while a relative tolerance does. Right at the
    // surface the truth goes to zero and only an absolute bound means anything.
    // The angular circumradius of a facet, not one step: a triangle spans a
    // half-step in latitude AND a half-step in longitude, so the point of the
    // sphere furthest from its chord is a diagonal away, not an edge away.
    const float step = kPi / static_cast<float>(rings);
    const float sagitta = r * (1.0f - std::cos(step * 0.5f * std::sqrt(2.0f)));
    INFO("sagitta at " << rings << " rings: " << sagitta);

    float worst = 0.0f;
    for (float x = -2.0f; x <= 2.0f; x += 0.13f)
        for (float y = -2.0f; y <= 2.0f; y += 0.17f) {
            kernel::cfloat3 p = cf3(x, y, 0.11f);
            float truth = std::abs(kernel::clength(p) - r);
            CAPTURE(x);
            CAPTURE(y);
            float error = std::abs(bvh.unsigned_distance(p) - truth);
            worst = std::max(worst, error);
            CHECK(error <= sagitta + 0.01f * truth);
        }
    INFO("worst error: " << worst);

    SUBCASE("and a finer tessellation converges on the analytic answer") {
        // Which is the check that the residual really is the tessellation and
        // not something in the traversal: quadrupling the rings should quarter
        // it, since the sagitta goes as the square of the step.
        Bvh finer = Bvh::build(sphere_mesh(r, rings * 4));
        float worst_fine = 0.0f;
        for (float x = -2.0f; x <= 2.0f; x += 0.13f)
            for (float y = -2.0f; y <= 2.0f; y += 0.17f) {
                kernel::cfloat3 p = cf3(x, y, 0.11f);
                float truth = std::abs(kernel::clength(p) - r);
                worst_fine = std::max(worst_fine, std::abs(finer.unsigned_distance(p) - truth));
            }
        INFO("worst at " << rings << ": " << worst << ", at " << rings * 4 << ": " << worst_fine);
        CHECK(worst_fine < worst * 0.2f);
    }
}

TEST_CASE("mesh import: the sign is right for a closed mesh") {
    Bvh bvh = Bvh::build(sphere_mesh(1.0f, 32));
    CHECK(bvh.is_inside(cf3(0, 0, 0)));
    CHECK(bvh.is_inside(cf3(0.5f, 0.3f, -0.2f)));
    CHECK_FALSE(bvh.is_inside(cf3(1.5f, 0, 0)));
    CHECK_FALSE(bvh.is_inside(cf3(0, 3.0f, 0)));

    SUBCASE("the winding number is what it should be, not merely the right side of a half") {
        // Summed exactly it is 1 inside a closed surface and 0 outside, whatever
        // the tessellation — that is a topological statement, not a numerical one.
        CHECK(bvh.winding_number(cf3(0, 0, 0), 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(bvh.winding_number(cf3(4.0f, 0, 0), 0.0f) ==
              doctest::Approx(0.0f).epsilon(1e-4).scale(1));

        // With distant nodes summarized it is close, not exact. A few percent
        // is the price of not summing ten thousand solid angles per sample,
        // and it is nowhere near the half that decides the sign.
        CHECK(bvh.winding_number(cf3(0, 0, 0)) == doctest::Approx(1.0f).epsilon(0.05));
        CHECK(bvh.winding_number(cf3(4.0f, 0, 0)) == doctest::Approx(0.0f).epsilon(0.05).scale(1));
    }

    SUBCASE("signed distance agrees with the two halves") {
        CHECK(bvh.signed_distance(cf3(0, 0, 0)) < 0.0f);
        CHECK(bvh.signed_distance(cf3(2.0f, 0, 0)) == doctest::Approx(1.0f).epsilon(0.02));
    }
}

TEST_CASE("mesh import: a hole does not flip a half-space") {
    // This is the case ray parity gets catastrophically wrong: with a hole in
    // the surface, a ray leaving through it crosses an even number of times and
    // reports the interior as outside. The winding number degrades continuously
    // instead, so away from the opening the answer is unchanged.
    Mesh closed = sphere_mesh(1.0f, 32);
    Mesh open = closed;
    // Remove a patch: the first rows of triangles, which is a cap around +Y.
    const std::size_t drop = open.indices.size() / 12;
    open.indices.erase(open.indices.begin(),
                       open.indices.begin() + static_cast<std::ptrdiff_t>(drop * 3));

    Bvh whole = Bvh::build(closed);
    Bvh holed = Bvh::build(open);
    REQUIRE(holed.triangle_count() < whole.triangle_count());

    // Well away from the opening, on the far side, the answer must not move.
    for (kernel::cfloat3 p : {cf3(0, -0.6f, 0), cf3(0.5f, -0.4f, 0.3f), cf3(0, -0.2f, 0)}) {
        CAPTURE(p.y);
        CHECK(holed.is_inside(p));
    }
    for (kernel::cfloat3 p : {cf3(0, -2.0f, 0), cf3(2.0f, 0, 0), cf3(0, 0, -1.8f)})
        CHECK_FALSE(holed.is_inside(p));

    SUBCASE("and the winding number passes smoothly through a half across the opening") {
        // Straight out through the hole: no jump, which is exactly the property
        // that makes an open mesh usable at all.
        float previous = holed.winding_number(cf3(0, 0.0f, 0));
        float biggest_jump = 0.0f;
        for (float y = 0.05f; y <= 2.0f; y += 0.05f) {
            float w = holed.winding_number(cf3(0, y, 0));
            biggest_jump = std::max(biggest_jump, std::abs(w - previous));
            previous = w;
        }
        INFO("largest step in the winding number: " << biggest_jump);
        CHECK(biggest_jump < 0.35f);
    }
}

TEST_CASE("mesh import: overlapping shapes do not cancel") {
    // Two intersecting spheres as one mesh. Parity counting reports the overlap
    // as OUTSIDE — two surfaces crossed — which hollows out exactly the region
    // that should be most solid. The winding number adds to 2 there instead.
    Mesh two = merged(sphere_mesh(0.8f, 20, cf3(-0.35f, 0, 0)),
                      sphere_mesh(0.8f, 20, cf3(0.35f, 0, 0)));
    Bvh bvh = Bvh::build(two);

    CHECK(bvh.is_inside(cf3(0, 0, 0)));         // the overlap
    CHECK(bvh.is_inside(cf3(-0.9f, 0, 0)));     // only the left one
    CHECK(bvh.is_inside(cf3(0.9f, 0, 0)));      // only the right one
    CHECK_FALSE(bvh.is_inside(cf3(0, 1.5f, 0)));
    CHECK(bvh.winding_number(cf3(0, 0, 0), 0.0f) == doctest::Approx(2.0f).epsilon(1e-3));
}

TEST_CASE("mesh import: reversing the winding does not move the surface") {
    // A flipped mesh describes the same surface. The winding number changes
    // sign, so a naive "> 0.5" would call the whole world inside; the field
    // must come back the same shape either way.
    Mesh forward = sphere_mesh(1.0f, 24);
    Mesh reversed = forward;
    for (std::size_t t = 0; t + 2 < reversed.indices.size(); t += 3)
        std::swap(reversed.indices[t + 1], reversed.indices[t + 2]);

    Bvh a = Bvh::build(forward);
    Bvh b = Bvh::build(reversed);
    CHECK(a.winding_number(cf3(0, 0, 0), 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(b.winding_number(cf3(0, 0, 0), 0.0f) == doctest::Approx(-1.0f).epsilon(1e-4));

    // The UNSIGNED distance is the same surface either way, which is the part
    // the shape depends on.
    for (float x = -1.6f; x <= 1.6f; x += 0.19f) {
        kernel::cfloat3 p = cf3(x, 0.2f, 0);
        CAPTURE(x);
        CHECK(a.unsigned_distance(p) == doctest::Approx(b.unsigned_distance(p)));
    }
}

TEST_CASE("mesh import: summarizing distant nodes agrees with summing every triangle") {
    Bvh bvh = Bvh::build(sphere_mesh(1.0f, 28));
    int disagreements = 0;
    float worst = 0.0f;
    for (float x = -2.5f; x <= 2.5f; x += 0.21f)
        for (float y = -2.5f; y <= 2.5f; y += 0.23f) {
            kernel::cfloat3 p = cf3(x, y, 0.07f);
            float fast = bvh.winding_number(p, 2.0f);
            float exact = bvh.winding_number(p, 0.0f);  // beta 0: no summarizing
            worst = std::max(worst, std::abs(fast - exact));
            if ((fast > 0.5f) != (exact > 0.5f)) ++disagreements;
        }
    INFO("worst winding-number difference: " << worst);
    CHECK(disagreements == 0);
    CHECK(worst < 0.05f);
}

TEST_CASE("mesh import: the tree is what makes it affordable") {
    // Not a benchmark, a shape check: ten times the triangles must not cost ten
    // times the WORK, or the summarization is not working and a real import
    // would be unusable.
    //
    // COUNTED, NOT TIMED, and the history is the argument. This asserted a
    // ratio of wall clocks, and both sides run in well under a millisecond on a
    // shared runner, so one preemption inside the coarse side inflates the
    // ratio: it read 5.379 against a 5.333 bound on macos-latest with the tree
    // unchanged. Scoring each side as the fastest of several passes was the
    // first repair and was not enough -- it read 5.459 on the same platform
    // twice more, while an A/B on identical hardware put the change that
    // triggered it at 2.254 against main's 2.246, which is no effect at all.
    //
    // The property was never a duration. Summarization means the walk STOPS at
    // a distant node and answers with one dipole term instead of descending it,
    // so what it saves is nodes visited and triangles tested -- and those are
    // integers, identical on every machine. `BvhWalkStats` reports them.
    auto walk_work = [](const Mesh& m, std::uint64_t* out_summarized) {
        Bvh bvh = Bvh::build(m);
        mesh::BvhWalkStats stats;
        bvh.set_walk_stats(&stats);
        float sink = 0.0f;
        // `winding_number` rather than `signed_distance`: summarizing is this
        // walk's own trick and the only thing the gate is about, and counting
        // it alone keeps the instrumentation off `closest`, which picking and
        // meshing lean on.
        for (float x = -1.5f; x <= 1.5f; x += 0.05f)
            for (float y = -1.5f; y <= 1.5f; y += 0.05f)
                sink += bvh.winding_number(cf3(x, y, 0.03f));
        bvh.set_walk_stats(nullptr);
        CHECK(std::isfinite(sink));
        *out_summarized = stats.summarized;
        return stats.work();
    };

    Mesh coarse = sphere_mesh(1.0f, 12);
    Mesh fine = sphere_mesh(1.0f, 48);
    const double ratio_triangles =
        static_cast<double>(fine.triangle_count()) / static_cast<double>(coarse.triangle_count());

    std::uint64_t coarse_summarized = 0, fine_summarized = 0;
    const std::uint64_t coarse_work = walk_work(coarse, &coarse_summarized);
    const std::uint64_t fine_work = walk_work(fine, &fine_summarized);
    const double ratio_work =
        static_cast<double>(fine_work) / static_cast<double>(std::max<std::uint64_t>(coarse_work, 1));

    INFO("triangles x" << ratio_triangles << ", work x" << ratio_work << " (" << coarse_work
                       << " -> " << fine_work << "), summarized " << coarse_summarized << " -> "
                       << fine_summarized);
    CHECK(ratio_triangles > 10.0);
    // THE GATE. Deterministic, and it fails by the model ratio rather than by a
    // couple of per cent if the summarization stops working: with beta at 0 the
    // walk descends to every leaf and this reads about the triangle ratio.
    CHECK(ratio_work < ratio_triangles / 3.0);
    // And the summarization is actually happening rather than the tree simply
    // being small: a walk that never took the dipole branch would pass the
    // ratio above by descending both sides equally badly.
    CHECK(fine_summarized > 0);
    CHECK(coarse_summarized > 0);
}

TEST_CASE("mesh import: the affordability gate fails when summarizing is off") {
    // Proof that the gate above catches its own regression, in the same file so
    // the two cannot drift apart. beta = 0 is the documented way to disable
    // summarizing -- `winding_number` sums every triangle -- and it is what the
    // approximation is compared against two cases up. With it off, the work
    // ratio must climb to about the triangle ratio.
    auto walk_work = [](const Mesh& m, float beta) {
        Bvh bvh = Bvh::build(m);
        mesh::BvhWalkStats stats;
        bvh.set_walk_stats(&stats);
        float sink = 0.0f;
        for (float x = -1.5f; x <= 1.5f; x += 0.05f)
            for (float y = -1.5f; y <= 1.5f; y += 0.05f)
                sink += bvh.winding_number(cf3(x, y, 0.03f), beta);
        bvh.set_walk_stats(nullptr);
        CHECK(std::isfinite(sink));
        return stats.work();
    };
    Mesh coarse = sphere_mesh(1.0f, 12);
    Mesh fine = sphere_mesh(1.0f, 48);
    const double ratio_triangles =
        static_cast<double>(fine.triangle_count()) / static_cast<double>(coarse.triangle_count());
    const double summarizing =
        static_cast<double>(walk_work(fine, 2.0f)) / static_cast<double>(walk_work(coarse, 2.0f));
    const double flat =
        static_cast<double>(walk_work(fine, 0.0f)) / static_cast<double>(walk_work(coarse, 0.0f));
    INFO("triangles x" << ratio_triangles << ", summarizing x" << summarizing << ", off x" << flat);
    CHECK(summarizing < ratio_triangles / 3.0);   // the gate passes
    CHECK(flat > ratio_triangles / 3.0);          // and fails with it off
    CHECK(flat > summarizing * 2.0);              // by a wide margin, not a whisker
}

TEST_CASE("mesh import: a mesh becomes a field") {
    Mesh m = sphere_mesh(1.0f, 32);
    auto volume = mesh::to_field(m, {0.05f, 0.15f, 0.0f, 2.0f});
    REQUIRE(volume.has_value());

    for (float x = -1.4f; x <= 1.4f; x += 0.11f)
        for (float y = -1.4f; y <= 1.4f; y += 0.13f) {
            kernel::cfloat3 p = cf3(x, y, 0.05f);
            float truth = kernel::clength(p) - 1.0f;
            if (std::abs(truth) > 0.12f) continue;  // inside the band
            CAPTURE(x);
            CAPTURE(y);
            CHECK(volume->eval(p) == doctest::Approx(truth).epsilon(0.1).scale(0.05));
        }
}

TEST_CASE("mesh import: an imported mesh is an ordinary item") {
    auto volume = mesh::to_field(sphere_mesh(0.7f, 24), {0.04f, 0.12f, 0.0f, 2.0f});
    REQUIRE(volume.has_value());

    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("l");
    scene::Node box;
    box.prim = scene::Prim::box(cf3(1.0f, 1.0f, 1.0f));
    l.sdf->insert(std::move(box));
    scene::Node cut;
    cut.prim = scene::Prim::volume();
    cut.volume = std::make_shared<field::FieldVolume>(*volume);
    cut.op = scene::Op::Subtract;
    l.sdf->insert(std::move(cut));

    scene::Tape tape = scene::compile_document(doc);
    CHECK(tape.eval(cf3(0, 0, 0)).d > 0.0f);           // the sphere is gone
    CHECK(tape.eval(cf3(0.95f, 0.95f, 0.95f)).d < 0.0f);  // the corner remains
}

TEST_CASE("mesh import: a mesh with no triangles is refused") {
    Mesh empty;
    CHECK_FALSE(mesh::to_field(empty).has_value());

    Mesh points;
    points.positions.push_back(cf3(0, 0, 0));
    CHECK_FALSE(mesh::to_field(points).has_value());

    SUBCASE("and a mesh whose indices point nowhere drops them rather than reading past the end") {
        Mesh bad = sphere_mesh(1.0f, 8);
        std::size_t before = Bvh::build(bad).triangle_count();
        bad.indices[0] = 99999;
        CHECK(Bvh::build(bad).triangle_count() == before - 1);
    }
}

TEST_CASE("mesh import: a default cell size follows the mesh's own scale") {
    // A default in world units would be far too fine for a building and far
    // too coarse for a bolt, so it is a fraction of the longest side.
    auto small = mesh::to_field(box_mesh(cf3(0.05f, 0.05f, 0.05f)));
    auto large = mesh::to_field(box_mesh(cf3(50.0f, 50.0f, 50.0f)));
    REQUIRE(small.has_value());
    REQUIRE(large.has_value());
    CHECK(large->cell_size() > small->cell_size() * 100.0f);
    // ...and both stay affordable, which is the point of scaling it at all.
    CHECK(small->brick_count() < 4000);
    CHECK(large->brick_count() < 4000);
}
