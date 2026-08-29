// The mutable surface: stable identities, welding, seams, and the round trip
// (dynamic-topology spec, add-dynamic-topology).
//
// The bar here is that `mesh -> DynamicSurface -> mesh` gives back the same
// surface under the stated seam semantics, and that the identities survive
// edits that have nothing to do with them. Those two are what everything above
// this file — the operators, the remesher, the sculptor, the undo record —
// assumes without checking.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <array>
#include <set>
#include <vector>

#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/dynamic_validate.h"
#include "clay/mesh/topology_ops.h"

using namespace clay;
using namespace clay::kernel;
using mesh::DynamicSurface;
using mesh::EdgeConstraint;
using mesh::Mesh;

namespace {

// A flat grid on the XZ plane, `n` quads a side, spanning [-half, half]. Every
// coordinate is a multiple of a power of two.
Mesh plane_grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
            m.normals.push_back(cf3(0, 1, 0));
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

// A closed surface, so the boundary cases and the closed cases are both
// covered. A cube-sphere: normalizing a cube grid uses only correctly-rounded
// arithmetic, where sin and cos do not.
Mesh cube_sphere(int n, float radius) {
    Mesh m;
    const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
    const float signs[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
    for (int f = 0; f < 6; ++f) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
        for (int v = 0; v <= n; ++v)
            for (int u = 0; u <= n; ++u) {
                float c[3];
                c[axes[f][0]] = -1.0f + 2.0f * static_cast<float>(u) / static_cast<float>(n);
                c[axes[f][1]] = -1.0f + 2.0f * static_cast<float>(v) / static_cast<float>(n);
                c[axes[f][2]] = signs[f];
                const cfloat3 p = cf3(c[0], c[1], c[2]);
                const cfloat3 unit = p / clength(p);
                m.positions.push_back(unit * radius);
                m.normals.push_back(unit);
            }
        const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const std::uint32_t a =
                    base + static_cast<std::uint32_t>(v) * stride + static_cast<std::uint32_t>(u);
                const std::uint32_t b = a + 1, c2 = a + stride, d = c2 + 1;
                if (signs[f] > 0.0f)
                    m.indices.insert(m.indices.end(), {a, c2, b, b, c2, d});
                else
                    m.indices.insert(m.indices.end(), {a, b, c2, b, d, c2});
            }
    }
    return m;
}

// The same grid with the middle column DUPLICATED and given different UVs on
// each side — a UV seam, as every exported model has. Built by hand because no
// mesher in this tree produces one.
Mesh seamed_plane(int n, float half) {
    Mesh base = plane_grid(n, half);
    Mesh m;
    m.positions = base.positions;
    m.normals = base.normals;
    m.uvs.resize(m.positions.size());
    const std::uint32_t stride = static_cast<std::uint32_t>(n + 1);
    const std::uint32_t seam_x = stride / 2;
    for (std::uint32_t i = 0; i < m.positions.size(); ++i)
        m.uvs[i] = cf2(static_cast<float>(i % stride) / static_cast<float>(n), 0.0f);

    // Duplicate the seam column, with a DIFFERENT u, and give the right half's
    // triangles the duplicates.
    std::vector<std::uint32_t> dup(stride, 0xffffffffu);
    for (std::uint32_t z = 0; z <= static_cast<std::uint32_t>(n); ++z) {
        const std::uint32_t src = z * stride + seam_x;
        dup[z] = static_cast<std::uint32_t>(m.positions.size());
        m.positions.push_back(m.positions[src]);
        m.normals.push_back(m.normals[src]);
        m.uvs.push_back(cf2(1.0f, 0.0f));  // the other side of the seam
    }
    for (std::uint32_t z = 0; z < static_cast<std::uint32_t>(n); ++z)
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(n); ++x) {
            std::uint32_t a = z * stride + x, b = a + 1, c = a + stride, d = c + 1;
            if (x >= seam_x) {
                // The right half reads the duplicated column where it meets it.
                if (x == seam_x) {
                    a = dup[z];
                    c = dup[z + 1];
                }
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            } else {
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            }
        }
    return m;
}

}  // namespace

TEST_CASE("dynamic surface: a closed mesh imports and validates") {
    const Mesh sphere = cube_sphere(4, 1.0f);
    mesh::DynamicBuildError err = mesh::DynamicBuildError::None;
    auto surface = DynamicSurface::from_mesh(sphere, {}, &err);
    REQUIRE(surface.has_value());
    CHECK(err == mesh::DynamicBuildError::None);

    const mesh::ValidationReport report = mesh::validate_dynamic_surface(*surface);
    CAPTURE(report.summary());
    CHECK(report.ok);

    const mesh::DynamicSurfaceStats stats = surface->stats();
    CHECK(stats.faces == sphere.indices.size() / 3);
    // A closed surface has no boundary at all — the seam duplicates a cube
    // sphere carries along its face borders weld away, which is what welding is
    // for.
    CHECK(stats.boundary_edges == 0);
    // Euler: V - E + F = 2 for a sphere. The strongest single statement that
    // the connectivity is what it claims to be.
    const long euler = static_cast<long>(stats.vertices) - static_cast<long>(stats.edges) +
                       static_cast<long>(stats.faces);
    CHECK(euler == 2);
    // Every edge has two half-edges.
    CHECK(stats.halfedges == stats.edges * 2);
}

TEST_CASE("dynamic surface: an open patch keeps its boundary") {
    const Mesh grid = plane_grid(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());
    CHECK(mesh::validate_dynamic_surface(*surface).ok);

    const mesh::DynamicSurfaceStats stats = surface->stats();
    CHECK(stats.faces == grid.indices.size() / 3);
    // A 4x4 grid has 16 border edges.
    CHECK(stats.boundary_edges == 16);
    // ...and the flag agrees with the incidence, which the validator also
    // checks but is worth naming here because the remesher trusts the flag.
    std::size_t flagged = 0;
    surface->edges().for_each_live([&](mesh::EdgeId, const mesh::DynamicEdge& e) {
        if (mesh::has_constraint(e.constraints, EdgeConstraint::Boundary)) ++flagged;
    });
    CHECK(flagged == 16);

    // A corner vertex is on the boundary; the middle one is not.
    const mesh::VertexId corner = surface->vertices().id_at(0);
    REQUIRE(corner.valid());
    CHECK(surface->is_boundary_vertex(corner));
}

TEST_CASE("dynamic surface: the round trip preserves the surface") {
    const Mesh sphere = cube_sphere(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(sphere);
    REQUIRE(surface.has_value());

    const Mesh back = surface->to_mesh();
    CHECK(back.indices.size() == sphere.indices.size());
    // WELDED, so the export has as many vertices as there are geometric ones —
    // fewer than the source, whose cube-sphere faces duplicated their borders.
    CHECK(back.positions.size() == surface->stats().vertices);
    CHECK(back.positions.size() < sphere.positions.size());
    CHECK(back.normals.size() == back.positions.size());

    // A DYNAMIC SURFACE IS TRIANGLES, and the export says so rather than
    // re-deriving a quad pairing that would vary with the sculpt.
    CHECK(back.quads.empty());

    // Every exported position is one the source had. Not a set equality —
    // welding removes duplicates — but nothing may be invented.
    std::set<std::array<float, 3>> source;
    for (const cfloat3& p : sphere.positions) source.insert({p.x, p.y, p.z});
    for (const cfloat3& p : back.positions) {
        CHECK(source.count({p.x, p.y, p.z}) == 1);
    }

    // The re-import is stable: a surface built from the export has the same
    // counts as the one it came from, so the conversion is a fixed point rather
    // than something that drifts each time through.
    auto again = DynamicSurface::from_mesh(back);
    REQUIRE(again.has_value());
    CHECK(again->stats().vertices == surface->stats().vertices);
    CHECK(again->stats().faces == surface->stats().faces);
    CHECK(again->stats().edges == surface->stats().edges);
}

TEST_CASE("dynamic surface: a UV seam survives as an edge property") {
    const Mesh seamed = seamed_plane(4, 1.0f);
    auto surface = DynamicSurface::from_mesh(seamed);
    REQUIRE(surface.has_value());
    CHECK(mesh::validate_dynamic_surface(*surface).ok);

    // THE POINT OF THE CORNER DOMAIN: the duplicated column welded into single
    // geometric vertices, so the surface is connected across the seam...
    CHECK(surface->stats().vertices < seamed.positions.size());

    // ...and the seam is still there, as a flag on the edges along it, rather
    // than as a crack in the geometry.
    std::size_t seam_edges = 0;
    surface->edges().for_each_live([&](mesh::EdgeId, const mesh::DynamicEdge& e) {
        if (mesh::has_constraint(e.constraints, EdgeConstraint::UvSeam)) ++seam_edges;
    });
    CHECK(seam_edges > 0);

    // And the export puts the duplicates back, because that is how a flat mesh
    // represents a seam.
    const Mesh back = surface->to_mesh();
    CHECK(back.uvs.size() == back.positions.size());
    CHECK(back.positions.size() > surface->stats().vertices);
}

TEST_CASE("dynamic surface: a local edit renumbers nothing else") {
    // The property everything above this file assumes. Erasing an element in
    // one region must not move any other element's slot.
    Mesh grid = plane_grid(6, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());

    std::vector<mesh::VertexId> before;
    surface->vertices().for_each_live(
        [&](mesh::VertexId id, const mesh::DynamicVertex&) { before.push_back(id); });
    REQUIRE(before.size() > 8);

    // Erase a face in one corner. Nothing about any vertex may change.
    const mesh::FaceId victim = surface->faces().id_at(0);
    REQUIRE(victim.valid());
    REQUIRE(surface->erase_face(victim));

    std::vector<mesh::VertexId> after;
    surface->vertices().for_each_live(
        [&](mesh::VertexId id, const mesh::DynamicVertex&) { after.push_back(id); });
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(before[i].slot == after[i].slot);
        CHECK(before[i].generation == after[i].generation);
    }
}

TEST_CASE("dynamic surface: a stale handle is detected, not silently rebound") {
    // The failure this exists to prevent is the worst kind there is: silent,
    // plausible, and appearing at a distance from its cause.
    Mesh grid = plane_grid(3, 1.0f);
    auto surface = DynamicSurface::from_mesh(grid);
    REQUIRE(surface.has_value());

    const mesh::FaceId original = surface->faces().id_at(0);
    REQUIRE(surface->live(original));
    REQUIRE(surface->erase_face(original));
    CHECK_FALSE(surface->live(original));
    CHECK(surface->face(original) == nullptr);

    // The slot is reused by the next create...
    const mesh::FaceId reused = surface->create_face(mesh::DynamicFace{});
    CHECK(reused.slot == original.slot);
    // ...and the old handle STILL does not name it.
    CHECK(reused.generation != original.generation);
    CHECK(surface->live(reused));
    CHECK_FALSE(surface->live(original));
    CHECK(surface->face(original) == nullptr);
}

TEST_CASE("dynamic surface: a non-manifold input is refused, not repaired") {
    // Three faces on one edge cannot be expressed by a half-edge surface, and
    // silently dropping the third would be a conversion that changes the model
    // without saying so.
    Mesh m;
    m.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0), cf3(0, 0, 1), cf3(0, -1, 0)};
    m.indices = {0, 1, 2, 0, 1, 3, 0, 1, 4};  // three faces sharing edge 0-1
    mesh::DynamicBuildError err = mesh::DynamicBuildError::None;
    auto surface = DynamicSurface::from_mesh(m, {}, &err);
    CHECK_FALSE(surface.has_value());
    CHECK(err == mesh::DynamicBuildError::NonManifoldEdge);

    // And the other refusals, each reported as itself rather than as a generic
    // failure, because a caller fixing an import needs to know which.
    Mesh empty;
    CHECK_FALSE(DynamicSurface::from_mesh(empty, {}, &err).has_value());
    CHECK(err == mesh::DynamicBuildError::EmptyMesh);

    Mesh out_of_range;
    out_of_range.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0)};
    out_of_range.indices = {0, 1, 9};
    CHECK_FALSE(DynamicSurface::from_mesh(out_of_range, {}, &err).has_value());
    CHECK(err == mesh::DynamicBuildError::IndexOutOfRange);

    Mesh degenerate;
    degenerate.positions = {cf3(0, 0, 0), cf3(1, 0, 0), cf3(0, 1, 0)};
    degenerate.indices = {0, 1, 1};
    CHECK_FALSE(DynamicSurface::from_mesh(degenerate, {}, &err).has_value());
    CHECK(err == mesh::DynamicBuildError::DegenerateTriangle);
}

TEST_CASE("dynamic surface: traversal answers what the connectivity says") {
    const Mesh sphere = cube_sphere(3, 1.0f);
    auto surface = DynamicSurface::from_mesh(sphere);
    REQUIRE(surface.has_value());

    std::vector<mesh::HalfEdgeId> ring;
    std::vector<mesh::VertexId> neighbours;
    std::vector<mesh::FaceId> faces;
    std::size_t checked = 0;
    surface->vertices().for_each_live([&](mesh::VertexId v, const mesh::DynamicVertex&) {
        REQUIRE(surface->outgoing_halfedges(v, &ring));
        REQUIRE(surface->one_ring(v, &neighbours));
        REQUIRE(surface->incident_faces(v, &faces));
        // A closed surface: the fan closes, so there are as many faces as
        // neighbours, and the valence agrees with all three.
        CHECK(ring.size() == neighbours.size());
        CHECK(faces.size() == ring.size());
        CHECK(surface->valence(v) == ring.size());
        // Every outgoing half-edge actually starts here.
        for (mesh::HalfEdgeId h : ring) CHECK(surface->origin_of(h) == v);
        // The neighbours are distinct: a repeat means the fan visited a vertex
        // twice, which is a pinched surface the validator should have caught.
        std::vector<std::uint32_t> slots;
        for (mesh::VertexId n : neighbours) slots.push_back(n.slot);
        std::sort(slots.begin(), slots.end());
        CHECK(std::adjacent_find(slots.begin(), slots.end()) == slots.end());
        ++checked;
    });
    CHECK(checked > 20);
}

TEST_CASE("dynamic surface: the encoding round-trips, generations included") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(4, 1.0f));
    REQUIRE(surface.has_value());

    // Edit it first, so the pools carry dead slots and bumped generations —
    // which is what a real document holds and what a naive encoding gets wrong.
    std::vector<mesh::EdgeId> edges;
    surface->edges().for_each_live(
        [&](mesh::EdgeId id, const mesh::DynamicEdge&) { edges.push_back(id); });
    std::size_t edited = 0;
    for (std::size_t i = 0; i < edges.size() && edited < 12; i += 5) {
        if (mesh::split_edge(*surface, edges[i], 0.5f).result == mesh::TopologyResult::Ok)
            ++edited;
    }
    REQUIRE(edited > 4);
    for (std::size_t i = 1; i < edges.size() && edited < 20; i += 9) {
        if (surface->live(edges[i]) &&
            mesh::collapse_edge(*surface, edges[i]).result == mesh::TopologyResult::Ok)
            ++edited;
    }

    const std::vector<std::uint8_t> bytes = surface->encode();
    REQUIRE(bytes.size() > 32);

    DynamicSurface back;
    REQUIRE(DynamicSurface::decode(bytes.data(), bytes.size(), &back));
    CHECK(mesh::validate_dynamic_surface(back).ok);

    // The same surface, element for element.
    CHECK(back.stats().vertices == surface->stats().vertices);
    CHECK(back.stats().edges == surface->stats().edges);
    CHECK(back.stats().faces == surface->stats().faces);
    CHECK(back.stats().halfedges == surface->stats().halfedges);
    CHECK(back.stats().boundary_edges == surface->stats().boundary_edges);

    // GENERATIONS PRESERVED, which is the part a cheap encoding skips. A
    // document reloaded with them reset hands back handles that a saved undo
    // record or a host's own bookkeeping would silently mis-resolve.
    std::size_t checked = 0;
    surface->vertices().for_each_live([&](mesh::VertexId id, const mesh::DynamicVertex& v) {
        const mesh::DynamicVertex* other = back.vertex(id);
        REQUIRE(other != nullptr);  // the SAME handle resolves, generation and all
        CHECK(other->position.x == v.position.x);
        CHECK(other->position.y == v.position.y);
        CHECK(other->position.z == v.position.z);
        CHECK(other->outgoing.slot == v.outgoing.slot);
        ++checked;
    });
    CHECK(checked == surface->stats().vertices);

    // ...and the exported mesh is identical, which is the property a consumer
    // downstream actually sees.
    const Mesh a = surface->to_mesh();
    const Mesh b = back.to_mesh();
    REQUIRE(a.positions.size() == b.positions.size());
    REQUIRE(a.indices.size() == b.indices.size());
    for (std::size_t i = 0; i < a.indices.size(); ++i) CHECK(a.indices[i] == b.indices[i]);
}

TEST_CASE("dynamic surface: a hostile or truncated encoding is refused") {
    auto surface = DynamicSurface::from_mesh(cube_sphere(3, 1.0f));
    REQUIRE(surface.has_value());
    const std::vector<std::uint8_t> bytes = surface->encode();

    DynamicSurface out;
    CHECK_FALSE(DynamicSurface::decode(nullptr, 0, &out));
    CHECK_FALSE(DynamicSurface::decode(bytes.data(), 8, &out));
    for (std::size_t cut = 24; cut < bytes.size(); cut += 101)
        CHECK_FALSE(DynamicSurface::decode(bytes.data(), cut, &out));

    // A wrong magic and a newer version, each refused rather than read as a
    // prefix of something this build understands.
    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] ^= 0xFF;
    CHECK_FALSE(DynamicSurface::decode(wrong.data(), wrong.size(), &out));
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 42;
    CHECK_FALSE(DynamicSurface::decode(newer.data(), newer.size(), &out));

    // A COUNT larger than the buffer could hold, which is how a reader gets
    // asked to allocate a gigabyte.
    std::vector<std::uint8_t> hostile = bytes;
    hostile[24] = 0xff;
    hostile[25] = 0xff;
    hostile[26] = 0xff;
    hostile[27] = 0x0f;
    CHECK_FALSE(DynamicSurface::decode(hostile.data(), hostile.size(), &out));

    // A live count larger than its own slot count is inconsistent on its face.
    std::vector<std::uint8_t> inconsistent = bytes;
    inconsistent[24] = 0xff;
    inconsistent[25] = 0x00;
    inconsistent[26] = 0x00;
    inconsistent[27] = 0x00;
    CHECK_FALSE(DynamicSurface::decode(inconsistent.data(), inconsistent.size(), &out));
}
