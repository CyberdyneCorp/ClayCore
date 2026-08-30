// Merging coincident vertices and dropping what that collapses (add-mesh-weld).
//
// WHAT MADE THIS NECESSARY, because the motivating fact is not obvious: the
// default mesher emits ZERO-AREA TRIANGLES. Measured on a plain analytic
// sphere at a 0.02 lattice, 1458 of 70,140 triangles — two per cent — have two
// corners at bit-identical positions. Nothing had noticed because everything
// downstream tolerates them, and `validate` counts them as SLIVERS rather than
// as degenerates (its `degenerate_triangles` means repeated INDICES, and these
// have distinct indices at identical positions), so a marched mesh reports
// `clean()` while carrying them.
//
// `mesh::DynamicSurface` cannot tolerate them, and refuses — correctly. The
// consequence was that no mesh this library marched could become an adaptive
// surface at all.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/quad_mesh.h"
#include "clay/mesh/validate.h"
#include "clay/mesh/weld.h"

using namespace clay;
using namespace clay::mesh;
using kernel::cf2;
using kernel::cf3;

namespace {

// The default mesher over an analytic sphere — the mesh that motivated this.
Mesh marched_sphere(float spacing = 0.02f) {
    auto sample = [spacing](int i, int j, int k) {
        const kernel::cfloat3 p = cf3(-0.7f + spacing * static_cast<float>(i),
                                      -0.7f + spacing * static_cast<float>(j),
                                      -0.7f + spacing * static_cast<float>(k));
        return kernel::clength(p) - 0.5f;
    };
    const int span = static_cast<int>(1.4f / spacing) + 1;
    int cell_min[3] = {0, 0, 0};
    int cell_max[3] = {span, span, span};
    return mesh_lattice(sample, cell_min, cell_max, cf3(-0.7f, -0.7f, -0.7f), spacing);
}

std::size_t exactly_coincident_corners(const Mesh& m) {
    std::size_t n = 0;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const kernel::cfloat3 a = m.positions[m.indices[t]];
        const kernel::cfloat3 b = m.positions[m.indices[t + 1]];
        const kernel::cfloat3 c = m.positions[m.indices[t + 2]];
        if (std::memcmp(&a, &b, sizeof(a)) == 0 || std::memcmp(&b, &c, sizeof(b)) == 0 ||
            std::memcmp(&c, &a, sizeof(c)) == 0)
            ++n;
    }
    return n;
}

// A quad with its two corners split along a UV seam: four positions, two of
// them duplicated with different uvs. The shape a weld must NOT flatten.
Mesh seamed_strip() {
    Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(1, 1, 0), cf3(0, 1, 0),
                   // the seam: the same two positions again, other side of the uv
                   cf3(1, 0, 0), cf3(1, 1, 0), cf3(2, 0, 0), cf3(2, 1, 0)};
    m.uvs = {cf2(0, 0),   cf2(0.5f, 0), cf2(0.5f, 1), cf2(0, 1),
             cf2(1, 0),   cf2(1, 1),    cf2(0.5f, 0), cf2(0.5f, 1)};
    m.indices = {0, 1, 2, 0, 2, 3, 4, 6, 7, 4, 7, 5};
    return m;
}

}  // namespace

TEST_CASE("weld: the default mesher emits zero-area triangles, and this removes them") {
    Mesh m = marched_sphere();
    const ValidationReport before = validate(m);

    // The premise, asserted rather than described — if the marcher ever stops
    // doing this, the rest of the file should stop claiming it does.
    CHECK(exactly_coincident_corners(m) > 0);
    CHECK(before.sliver_triangles > 0);
    // ...and `validate` calls it clean regardless, because its degenerate check
    // is about repeated INDICES and these have distinct indices.
    CHECK(before.degenerate_triangles == 0);
    CHECK(before.watertight);

    const WeldReport r = weld(&m);
    CHECK(r.vertices_merged > 0);
    CHECK(r.triangles_collapsed > 0);
    CHECK(r.vertices_after < r.vertices_before);
    CHECK(r.triangles_after < r.triangles_before);
    CHECK(r.epsilon > 0.0f);  // the relative epsilon, resolved

    CHECK(exactly_coincident_corners(m) == 0);
    const ValidationReport after = validate(m);
    CHECK(after.sliver_triangles == 0);

    // WATERTIGHTNESS SURVIVES, which is the property that makes this safe:
    // merging two coincident vertices and dropping the triangle between them is
    // an edge collapse of a zero-length edge, and a triangle whose corners
    // coincide bounds nothing, so removing it cannot open a hole.
    CHECK(after.watertight);
    CHECK(after.manifold);
    CHECK(after.oriented);
    CHECK(after.degenerate_triangles == 0);
}

TEST_CASE("weld: a marched mesh becomes convertible to an adaptive surface") {
    // THE POINT OF THE WHOLE VERB. Before it, no mesh this library marched
    // could become a DynamicSurface — not `mesh_lattice`, not `mesh_tape`, not
    // `voxel_remesh` — and no caller had ever tried, because the dynamic
    // topology example builds its input analytically.
    Mesh m = marched_sphere();
    DynamicBuildError before = DynamicBuildError::None;
    CHECK_FALSE(DynamicSurface::from_mesh(m, {}, &before).has_value());
    CHECK(before == DynamicBuildError::DegenerateTriangle);

    weld(&m);
    DynamicBuildError after = DynamicBuildError::None;
    const std::optional<DynamicSurface> surface = DynamicSurface::from_mesh(m, {}, &after);
    CHECK(after == DynamicBuildError::None);
    REQUIRE(surface.has_value());
    CHECK(surface->stats().faces == m.triangle_count());
}

TEST_CASE("weld: welding below what the consumer welds at does not help") {
    // The trap, and the reason the header says to weld at AT LEAST the epsilon
    // the consumer will use. An exact weld removes the bit-identical duplicates
    // and leaves the merely-close ones — which `from_mesh` then merges at ITS
    // default, collapsing triangles the exact weld had kept.
    Mesh m = marched_sphere();
    WeldOptions exact;
    exact.epsilon = 0.0f;
    const WeldReport r = weld(&m, exact);
    CHECK(r.vertices_merged > 0);            // it did do something...
    CHECK(exactly_coincident_corners(m) == 0);
    DynamicBuildError error = DynamicBuildError::None;
    CHECK_FALSE(DynamicSurface::from_mesh(m, {}, &error).has_value());  // ...and not enough
    CHECK(error == DynamicBuildError::DegenerateTriangle);
}

TEST_CASE("weld: a mesh with nothing to merge comes back byte-identical") {
    // A clean mesh must not pay a rewrite. Byte-identical rather than
    // equivalent: renumbering a mesh that needed nothing done to it would
    // invalidate every index a caller was holding, for no reason.
    Mesh original = marched_sphere();
    weld(&original);  // now clean at this epsilon
    Mesh again = original;

    const WeldReport r = weld(&again);
    CHECK(r.vertices_merged == 0);
    CHECK(r.triangles_collapsed == 0);
    CHECK(r.vertices_after == r.vertices_before);
    CHECK(r.triangles_after == r.triangles_before);
    REQUIRE(again.positions.size() == original.positions.size());
    CHECK(std::memcmp(again.positions.data(), original.positions.data(),
                      original.positions.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(again.indices == original.indices);
}

TEST_CASE("weld: a uv seam survives by default and is destroyed when asked") {
    // A UV SEAM IS DUPLICATED POSITIONS WITH DIFFERENT UVS. That is not a defect
    // to be cleaned up, it is how a flat mesh represents a seam at all, and
    // merging across one silently destroys the layout.
    Mesh kept = seamed_strip();
    const std::size_t before = kept.positions.size();
    const WeldReport safe = weld(&kept);
    CHECK(safe.vertices_merged == 0);
    CHECK(kept.positions.size() == before);
    REQUIRE(kept.uvs.size() == kept.positions.size());

    // ...and a caller who genuinely wants one mesh out of it can say so.
    Mesh flattened = seamed_strip();
    WeldOptions merge_everything;
    merge_everything.preserve_attribute_splits = false;
    const WeldReport hard = weld(&flattened, merge_everything);
    CHECK(hard.vertices_merged == 2);
    CHECK(flattened.positions.size() == before - 2);
    CHECK(flattened.triangle_count() == 4);  // no triangle collapsed; only vertices merged
}

TEST_CASE("weld: quads are dropped when the triangles are rewritten") {
    // mesh_data.h's rule: an operation that REWRITES `indices` must clear
    // `quads`, or it leaves a quad list describing triangles that no longer
    // exist — a lie that survives into a saved document.
    Mesh m = marched_sphere();
    // A marcher emits none, so give it a plausible pairing to destroy.
    m.quads.assign(m.triangle_count() / 2 * 4, 0u);
    const WeldReport r = weld(&m);
    REQUIRE(r.vertices_merged > 0);
    CHECK(r.quads_dropped);
    CHECK(m.quads.empty());
}

TEST_CASE("weld: an out-of-range index is dropped rather than read") {
    Mesh m = marched_sphere(0.08f);
    const std::size_t good = m.triangle_count();
    m.indices.push_back(0);
    m.indices.push_back(1);
    m.indices.push_back(static_cast<std::uint32_t>(m.positions.size() + 7));
    const WeldReport r = weld(&m);
    CHECK(r.triangles_invalid == 1);
    CHECK(r.triangles_after <= good);
    // The guarantee: every index is in range afterwards, whether or not
    // anything else needed merging.
    for (std::uint32_t index : m.indices) CHECK(index < m.positions.size());
}

TEST_CASE("weld: the same mesh welds to the same bytes") {
    Mesh a = marched_sphere();
    Mesh b = marched_sphere();
    weld(&a);
    weld(&b);
    REQUIRE(a.positions.size() == b.positions.size());
    CHECK(std::memcmp(a.positions.data(), b.positions.data(),
                      a.positions.size() * sizeof(kernel::cfloat3)) == 0);
    CHECK(a.indices == b.indices);
}

TEST_CASE("weld: attributes travel with the vertex that survives") {
    Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 0)};
    m.colors = {cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1), cf3(1, 0, 0)};
    // Two triangles; the second uses the duplicate of vertex 0.
    m.indices = {0, 1, 2, 3, 2, 1};
    const WeldReport r = weld(&m);
    CHECK(r.vertices_merged == 1);
    REQUIRE(m.positions.size() == 3);
    REQUIRE(m.colors.size() == 3);
    // The survivor is the LOWEST index, so vertex 0's colour is what remains.
    CHECK(m.colors[0].x == doctest::Approx(1.0f));
    CHECK(m.triangle_count() == 2);  // neither collapsed: they share an edge, not a corner
}
