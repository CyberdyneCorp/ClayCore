// Whole-form deformers on a mesh layer (add-mesh-deformers).
//
// The test that matters is the LAST one: a taper or a twist applied to a mesh
// has to land where the same deformer applied to the same shape as a FIELD
// lands. If the two disagree, the forward map is not the inverse map's
// counterpart and everything else here is measuring the wrong thing
// consistently.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay/eval/backend.h"
#include "clay/mesh/deform.h"
#include "clay/mesh/sculpt.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::mesh;
using kernel::cf3;
using kernel::cfloat3;

namespace {

// A closed cylinder-ish column along Y, so a taper and a twist both have
// something to act on and the span is unambiguous.
Mesh column(int rings = 24, int segments = 20, float radius = 0.35f, float height = 2.0f) {
    Mesh m;
    for (int r = 0; r <= rings; ++r) {
        const float y = height * (static_cast<float>(r) / rings) - height * 0.5f;
        for (int s = 0; s < segments; ++s) {
            const float a = 6.283185307f * static_cast<float>(s) / segments;
            m.positions.push_back(cf3(radius * std::cos(a), y, radius * std::sin(a)));
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const std::uint32_t a = static_cast<std::uint32_t>(r * segments + s);
            const std::uint32_t b = static_cast<std::uint32_t>(r * segments + (s + 1) % segments);
            const std::uint32_t c = a + static_cast<std::uint32_t>(segments);
            const std::uint32_t d = b + static_cast<std::uint32_t>(segments);
            for (std::uint32_t i : {a, c, b, b, c, d}) m.indices.push_back(i);
        }
    return m;
}

MeshDeformSettings taper_of(float s0, float s1, float span = 2.0f) {
    MeshDeformSettings s;
    s.verb = MeshDeform::Taper;
    s.origin = cf3(0, -1.0f, 0);
    s.axis = cf3(0, 1, 0);
    s.span = span;
    s.scale_start = s0;
    s.scale_end = s1;
    return s;
}

MeshDeformSettings twist_of(float radians, float span = 2.0f) {
    MeshDeformSettings s;
    s.verb = MeshDeform::Twist;
    s.origin = cf3(0, -1.0f, 0);
    s.axis = cf3(0, 1, 0);
    s.span = span;
    s.angle = radians;
    return s;
}

bool same_bytes(const std::vector<cfloat3>& a, const std::vector<cfloat3>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(cfloat3)) == 0);
}

bool same_bytes(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(std::uint32_t)) == 0);
}

float radius_at(const Mesh& m, float y, float tol = 0.05f) {
    float best = 0.0f;
    for (const cfloat3& p : m.positions)
        if (std::fabs(p.y - y) < tol) best = std::max(best, std::hypot(p.x, p.z));
    return best;
}

}  // namespace

TEST_CASE("mesh deform: a taper narrows the far end and leaves the near one") {
    Mesh m = column();
    const float r0 = radius_at(m, -1.0f), r1 = radius_at(m, 1.0f);
    MeshSculptor sc(m);
    REQUIRE(sc.apply_deformer(taper_of(1.0f, 0.4f)) > 0);

    CHECK(radius_at(m, -1.0f) == doctest::Approx(r0).epsilon(0.02));
    CHECK(radius_at(m, 1.0f) == doctest::Approx(r1 * 0.4f).epsilon(0.02));
}

TEST_CASE("mesh deform: a twist rotates the far end and leaves the near one") {
    Mesh m = column();
    Mesh base = m;
    MeshSculptor sc(m);
    REQUIRE(sc.apply_deformer(twist_of(1.2f)) > 0);

    // A vertex at the span's start has not turned; one at the end has turned
    // by the full angle.
    const auto angle_at = [](const Mesh& mesh, float y) {
        for (std::size_t i = 0; i < mesh.positions.size(); ++i)
            if (std::fabs(mesh.positions[i].y - y) < 0.05f)
                return std::atan2(mesh.positions[i].z, mesh.positions[i].x);
        return 0.0f;
    };
    CHECK(std::fabs(angle_at(m, -1.0f) - angle_at(base, -1.0f)) < 0.02f);
    const float turned = std::fabs(angle_at(m, 1.0f) - angle_at(base, 1.0f));
    CHECK(turned == doctest::Approx(1.2f).epsilon(0.05));
}

TEST_CASE("mesh deform: topology and seams survive") {
    Mesh m = column();
    // Split one ring's vertices into coincident copies, the way a hard edge or
    // a UV seam does. They must come out still coincident, bit for bit.
    const std::size_t split_from = 40;
    m.positions.push_back(m.positions[split_from]);
    const std::uint32_t twin = static_cast<std::uint32_t>(m.positions.size() - 1);
    m.indices.push_back(twin);
    m.indices.push_back(twin);
    m.indices.push_back(static_cast<std::uint32_t>(split_from));

    const Mesh base = m;
    MeshSculptor sc(m);
    REQUIRE(sc.apply_deformer(twist_of(0.9f)) > 0);

    CHECK(same_bytes(m.indices, base.indices));
    CHECK(std::memcmp(&m.positions[split_from], &m.positions[twin], sizeof(cfloat3)) == 0);
}

TEST_CASE("mesh deform: a gate holds part of the form still") {
    Mesh m = column();
    const Mesh base = m;
    MeshSculptor sc(m);
    // Fully protect everything below the middle.
    field::MaskGate gate = [](cfloat3 p) { return p.y < 0.0f ? 1.0f : 0.0f; };
    REQUIRE(sc.apply_deformer(taper_of(1.0f, 0.3f), gate) > 0);

    std::size_t held = 0, moved = 0;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        if (base.positions[i].y < 0.0f) {
            if (std::memcmp(&m.positions[i], &base.positions[i], sizeof(cfloat3)) == 0) ++held;
        } else if (std::memcmp(&m.positions[i], &base.positions[i], sizeof(cfloat3)) != 0) {
            ++moved;
        }
    }
    CHECK(held > 0);
    CHECK(moved > 0);
    // Bit-identical, not merely close: a fully gated vertex must not be a lerp
    // that lands one ulp from where it started.
    for (std::size_t i = 0; i < m.positions.size(); ++i)
        if (base.positions[i].y < 0.0f)
            REQUIRE(std::memcmp(&m.positions[i], &base.positions[i], sizeof(cfloat3)) == 0);
}

TEST_CASE("mesh deform: an identity deformer moves nothing and records nothing") {
    Mesh m = column();
    const Mesh base = m;
    MeshSculptor sc(m);
    VertexDeltas record;

    CHECK(sc.apply_deformer(taper_of(1.0f, 1.0f), {}, &record) == 0);
    CHECK(sc.apply_deformer(twist_of(0.0f), {}, &record) == 0);
    MeshDeformSettings no_span = taper_of(1.0f, 0.2f, 0.0f);
    CHECK(sc.apply_deformer(no_span, {}, &record) == 0);

    CHECK(record.empty());
    CHECK(same_bytes(m.positions, base.positions));
}

TEST_CASE("mesh deform: a deformation reverts and re-applies bit-identically") {
    Mesh m = column();
    const Mesh base = m;
    MeshSculptor sc(m);
    VertexDeltas record;
    REQUIRE(sc.apply_deformer(twist_of(1.1f), {}, &record) > 0);
    REQUIRE(sc.apply_deformer(taper_of(1.0f, 0.6f), {}, &record) > 0);
    const Mesh deformed = m;

    REQUIRE(record.revert(m));
    CHECK(same_bytes(m.positions, base.positions));
    CHECK(same_bytes(m.normals, base.normals));
    REQUIRE(record.apply(m));
    CHECK(same_bytes(m.positions, deformed.positions));
}

TEST_CASE("mesh deform: deterministic") {
    Mesh a = column(), b = column();
    MeshSculptor sa(a), sb(b);
    MeshDeformSettings s = twist_of(0.77f);
    s.axis = cf3(0.3f, 1.0f, -0.2f);  // an off-axis frame, to exercise the basis
    REQUIRE(sa.apply_deformer(s) > 0);
    REQUIRE(sb.apply_deformer(s) > 0);
    CHECK(same_bytes(a.positions, b.positions));
}

TEST_CASE("mesh deform: the forward map is the kernel inverse map's counterpart") {
    // THE test this change rests on. A point pushed through the mesh's FORWARD
    // map and then through the kernel's INVERSE map must come back where it
    // started — that is what makes a deformed mesh and a deformed field the
    // same shape rather than two plausible ones.
    const float span = 2.0f, y0 = -1.0f;

    SUBCASE("taper") {
        MeshDeformSettings s = taper_of(1.0f, 0.45f, span);
        float worst = 0.0f;
        for (float y = -1.0f; y <= 1.0f; y += 0.2f)
            for (float x = -0.5f; x <= 0.5f; x += 0.25f)
                for (float z = -0.5f; z <= 0.5f; z += 0.25f) {
                    const cfloat3 rest = cf3(x, y, z);
                    const cfloat3 fwd = deform_point(s, rest);
                    // The kernel's inverse taper, over the same span.
                    const cfloat3 back =
                        kernel::ctaper_point(fwd, y0, y0 + span, 1.0f, 0.45f, 0);
                    worst = std::max(worst, kernel::clength(back - rest));
                }
        CHECK(worst < 1e-5f);
    }

    SUBCASE("twist") {
        const float radians = 1.3f;
        MeshDeformSettings s = twist_of(radians, span);
        float worst = 0.0f;
        for (float y = -1.0f; y <= 1.0f; y += 0.2f)
            for (float x = -0.5f; x <= 0.5f; x += 0.25f)
                for (float z = -0.5f; z <= 0.5f; z += 0.25f) {
                    const cfloat3 rest = cf3(x, y, z);
                    const cfloat3 fwd = deform_point(s, rest);
                    const cfloat3 back = kernel::ctwist_range_point(fwd, radians / span, y0,
                                                                    y0 + span, 0);
                    worst = std::max(worst, kernel::clength(back - rest));
                }
        CHECK(worst < 1e-5f);
    }
}
