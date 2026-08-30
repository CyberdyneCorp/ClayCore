#pragma once

// The fixtures the voxel remesher is judged on (add-voxel-remesher).
//
// Built analytically rather than loaded, so a test says what it is testing and
// a failure names a shape rather than a file. Every one is deterministic: the
// same vertices in the same order on every platform, which is what lets the
// determinism tests compare bytes.
//
// The set is the one the change's tasks name — primitives, the cases that
// exist to break a remesher (overlaps, self-intersection, open surfaces,
// reversed winding, thin material), and the two that exist to measure it
// (a stretched surface, a long thin body).

#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdint>
#include <vector>

#include "clay/mesh/mesh_data.h"

namespace clay_test {

using clay::kernel::cf3;
using clay::kernel::cfloat3;
using clay::mesh::Mesh;

// A UV sphere. Poles are single vertices, so the triangle fan at each is one
// degenerate-free ring; every other row is quads split into two triangles.
inline Mesh sphere(float radius = 1.0f, cfloat3 centre = cf3(0, 0, 0), int rings = 24,
                   int segments = 48) {
    Mesh m;
    const float pi = 3.14159265358979323846f;
    m.positions.push_back(centre + cf3(0, radius, 0));
    for (int r = 1; r < rings; ++r) {
        const float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        const float y = std::cos(phi), rr = std::sin(phi);
        for (int s = 0; s < segments; ++s) {
            const float th = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            m.positions.push_back(centre +
                                  cf3(rr * std::cos(th), y, rr * std::sin(th)) * radius);
        }
    }
    m.positions.push_back(centre + cf3(0, -radius, 0));

    const std::uint32_t top = 0;
    const std::uint32_t bottom = static_cast<std::uint32_t>(m.positions.size() - 1);
    auto at = [&](int r, int s) {
        return static_cast<std::uint32_t>(1 + (r - 1) * segments + (s % segments));
    };
    for (int s = 0; s < segments; ++s) {
        m.indices.push_back(top);
        m.indices.push_back(at(1, s + 1));
        m.indices.push_back(at(1, s));
    }
    for (int r = 1; r < rings - 1; ++r)
        for (int s = 0; s < segments; ++s) {
            const std::uint32_t a = at(r, s), b = at(r, s + 1);
            const std::uint32_t c = at(r + 1, s), d = at(r + 1, s + 1);
            m.indices.push_back(a);
            m.indices.push_back(b);
            m.indices.push_back(c);
            m.indices.push_back(b);
            m.indices.push_back(d);
            m.indices.push_back(c);
        }
    for (int s = 0; s < segments; ++s) {
        m.indices.push_back(bottom);
        m.indices.push_back(at(rings - 1, s));
        m.indices.push_back(at(rings - 1, s + 1));
    }
    return m;
}

// An axis-aligned box, eight vertices, twelve triangles, outward wound.
inline Mesh box(cfloat3 half_extent, cfloat3 centre = cf3(0, 0, 0)) {
    Mesh m;
    for (int i = 0; i < 8; ++i)
        m.positions.push_back(centre + cf3((i & 1) ? half_extent.x : -half_extent.x,
                                           (i & 2) ? half_extent.y : -half_extent.y,
                                           (i & 4) ? half_extent.z : -half_extent.z));
    // Corner i is (i&1 ? +x : -x, i&2 ? +y : -y, i&4 ? +z : -z), and every
    // triple below has cross(b - a, c - a) pointing OUT of the box. Checked
    // rather than eyeballed: the signed volume of `cube(0.5f)` is 1, and a
    // fixture whose winding is wrong makes the operation under test look wrong.
    static const std::uint32_t faces[12][3] = {
        {0, 2, 3}, {0, 3, 1},  // -z
        {4, 5, 7}, {4, 7, 6},  // +z
        {0, 1, 5}, {0, 5, 4},  // -y
        {2, 6, 7}, {2, 7, 3},  // +y
        {0, 4, 6}, {0, 6, 2},  // -x
        {1, 3, 7}, {1, 7, 5},  // +x
    };
    for (const auto& f : faces)
        for (std::uint32_t v : f) m.indices.push_back(v);
    return m;
}

inline Mesh cube(float half = 0.5f) { return box(cf3(half, half, half)); }

// A torus: the fixture whose Euler characteristic is not a sphere's, so a
// remesh that quietly filled the hole would be visible in the report rather
// than only in a render.
inline Mesh torus(float major = 0.7f, float minor = 0.25f, int major_segments = 48,
                  int minor_segments = 24) {
    Mesh m;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < major_segments; ++i) {
        const float u = 2.0f * pi * static_cast<float>(i) / static_cast<float>(major_segments);
        for (int j = 0; j < minor_segments; ++j) {
            const float v = 2.0f * pi * static_cast<float>(j) / static_cast<float>(minor_segments);
            const float r = major + minor * std::cos(v);
            m.positions.push_back(cf3(r * std::cos(u), minor * std::sin(v), r * std::sin(u)));
        }
    }
    auto at = [&](int i, int j) {
        return static_cast<std::uint32_t>((i % major_segments) * minor_segments +
                                          (j % minor_segments));
    };
    for (int i = 0; i < major_segments; ++i)
        for (int j = 0; j < minor_segments; ++j) {
            const std::uint32_t a = at(i, j), b = at(i + 1, j);
            const std::uint32_t c = at(i, j + 1), d = at(i + 1, j + 1);
            m.indices.push_back(a);
            m.indices.push_back(c);
            m.indices.push_back(b);
            m.indices.push_back(b);
            m.indices.push_back(c);
            m.indices.push_back(d);
        }
    return m;
}

// Two meshes as one input. The remesher's job on this is to produce ONE body
// where the interiors overlap, not two intersecting shells.
inline Mesh combine(const Mesh& a, const Mesh& b) {
    Mesh m = a;
    const std::uint32_t base = static_cast<std::uint32_t>(a.positions.size());
    m.positions.insert(m.positions.end(), b.positions.begin(), b.positions.end());
    for (std::uint32_t i : b.indices) m.indices.push_back(base + i);
    if (!a.colors.empty() && a.colors.size() == a.positions.size() &&
        b.colors.size() == b.positions.size())
        m.colors.insert(m.colors.end(), b.colors.begin(), b.colors.end());
    else
        m.colors.clear();
    return m;
}

inline Mesh overlapping_spheres(float radius = 0.6f, float separation = 0.7f) {
    return combine(sphere(radius, cf3(-separation * 0.5f, 0, 0), 20, 40),
                   sphere(radius, cf3(separation * 0.5f, 0, 0), 20, 40));
}

// A sphere with a cube pushed into it: two very different tessellations whose
// union the field has to fuse.
inline Mesh sphere_and_cube() {
    return combine(sphere(0.6f, cf3(0, 0, 0), 20, 40), box(cf3(0.3f, 0.3f, 0.3f), cf3(0.5f, 0, 0)));
}

// A small sphere entirely inside a large one. The winding number counts it
// twice, and the remesh must still produce one exterior.
inline Mesh nested_shells() {
    return combine(sphere(0.9f, cf3(0, 0, 0), 20, 40), sphere(0.4f, cf3(0, 0, 0), 16, 32));
}

// A plate of a chosen thickness: the fixture that decides whether a resolution
// is fine enough to keep thin material.
inline Mesh plate(float thickness, float side = 1.0f) {
    return box(cf3(side * 0.5f, thickness * 0.5f, side * 0.5f));
}

// A long thin body. Its bounding box grows with the length and its surface
// grows with the length too, which is what makes it the fixture that separates
// O(surface) work from O(bounding box) work.
inline Mesh long_bar(float length, float thickness = 0.12f) {
    return box(cf3(length * 0.5f, thickness * 0.5f, thickness * 0.5f));
}

// A closed sphere with `count` triangles deleted: an open surface with a
// genuine hole, not merely an inconsistent winding.
inline Mesh open_sphere(std::size_t count = 24) {
    Mesh m = sphere(1.0f, cf3(0, 0, 0), 20, 40);
    const std::size_t keep = m.triangle_count() > count ? m.triangle_count() - count : 0;
    m.indices.resize(keep * 3);
    return m;
}

// Every triangle of one component wound the other way. The generalized winding
// number is the reason this is expected to survive at all.
inline Mesh reversed_winding(const Mesh& in) {
    Mesh m = in;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3)
        std::swap(m.indices[t + 1], m.indices[t + 2]);
    return m;
}

// A large body and a small island far from it, with no shared vertices.
inline Mesh body_and_island(float island_radius = 0.09f) {
    return combine(sphere(0.7f, cf3(0, 0, 0), 20, 40),
                   sphere(island_radius, cf3(1.4f, 0, 0), 10, 20));
}

// A sphere whose vertices have been pulled along one axis by a smooth bump —
// the shape a snakehook leaves, with triangles stretched far past uniformity.
inline Mesh stretched_sphere(float pull = 1.6f) {
    Mesh m = sphere(0.6f, cf3(0, 0, 0), 24, 48);
    for (cfloat3& p : m.positions) {
        const float t = std::max(0.0f, p.y / 0.6f);
        p.y += pull * t * t * t * 0.6f;
    }
    return m;
}

// A colour ramp along x over [-1, 1], so a transferred colour can be checked
// against the position it landed at rather than against another array.
inline Mesh with_color_ramp(Mesh m) {
    m.colors.resize(m.positions.size());
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const float t = std::clamp(m.positions[i].x * 0.5f + 0.5f, 0.0f, 1.0f);
        m.colors[i] = cf3(t, 1.0f - t, 0.25f);
    }
    return m;
}

inline Mesh with_uvs(Mesh m) {
    m.uvs.assign(m.positions.size(), clay::kernel::cf2(0.5f, 0.5f));
    return m;
}

// Two parallel sheets closer together than a projection clamp would reach:
// the shape that makes a nearest-point projection jump onto the wrong sheet
// unless the normal test rejects it.
inline Mesh narrow_gap(float gap = 0.05f, float thickness = 0.25f) {
    const float half = thickness * 0.5f;
    const float offset = gap * 0.5f + half;
    return combine(box(cf3(0.5f, 0.5f, half), cf3(0, 0, -offset)),
                   box(cf3(0.5f, 0.5f, half), cf3(0, 0, offset)));
}


// A sheet folded back through itself: a genuine SELF-intersection inside one
// surface, which is a different fixture from two shells that overlap. The fold
// brings the surface within a fraction of a voxel of itself, so a
// reconstructed vertex near it has source geometry on BOTH sides — and the
// side its own normal disagrees with is the one a nearest-point projection
// would happily drag it onto.
//
// One connected grid, so `component_count` is 1 and nothing about this is two
// objects meeting.
inline Mesh folded_sheet(int n = 60, float fold = 0.30f, float depth = 0.35f) {
    Mesh m;
    const float pi = 3.14159265358979323846f;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const float u = -0.5f + static_cast<float>(i) / static_cast<float>(n);
            const float v = -0.5f + static_cast<float>(j) / static_cast<float>(n);
            // The y displacement doubles back over itself while z sweeps, which
            // is what makes the surface pass through the region it already
            // occupies rather than merely bending sharply.
            m.positions.push_back(cf3(u, v + fold * std::sin(2.0f * pi * v),
                                      depth * std::sin(2.0f * pi * v)));
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j * (n + 1) + i);
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + static_cast<std::uint32_t>(n + 1);
            const std::uint32_t d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// A cube with its twelve edges and eight corners cut off at 45 degrees: six
// squares, twelve chamfer rectangles and eight corner triangles, 24 vertices.
//
// THE fixture for sharp output. The chamfer's edges are exactly the lines a
// dual-contouring pass is supposed to reproduce as creases and a marching pass
// is supposed to round off, so this is what tells the two surface modes apart —
// a plain cube would not, because its edges are axis-aligned and land on the
// lattice.
//
// The solid is CONVEX and centred on the origin, which is what makes the
// winding trivially correct rather than carefully hand-checked: outward at any
// face is the direction from the origin to that face, so each triangle is
// emitted and then flipped if its normal disagrees.
inline Mesh chamfered_cube(float half = 0.5f, float chamfer = 0.16f) {
    const float c = half - chamfer;
    Mesh m;
    // Vertex (corner, axis): `half` on `axis`, `c` on the other two, signs from
    // the corner's bits. 8 corners x 3 axes = 24.
    auto vid = [](int corner, int axis) { return static_cast<std::uint32_t>(corner * 3 + axis); };
    for (int corner = 0; corner < 8; ++corner) {
        const float sign[3] = {(corner & 1) ? 1.0f : -1.0f, (corner & 2) ? 1.0f : -1.0f,
                               (corner & 4) ? 1.0f : -1.0f};
        for (int axis = 0; axis < 3; ++axis) {
            float p[3];
            for (int a = 0; a < 3; ++a) p[a] = sign[a] * (a == axis ? half : c);
            m.positions.push_back(cf3(p[0], p[1], p[2]));
        }
    }

    std::vector<std::vector<std::uint32_t>> faces;
    // Six axis squares: the four vertices that are `half` on this axis with
    // this sign.
    for (int axis = 0; axis < 3; ++axis)
        for (int s = 0; s < 2; ++s) {
            std::vector<std::uint32_t> loop;
            for (int corner = 0; corner < 8; ++corner)
                if (((corner >> axis) & 1) == s) loop.push_back(vid(corner, axis));
            faces.push_back(loop);
        }
    // Twelve edge chamfers: the edge running along `axis` at fixed signs on the
    // other two. Its rectangle is, at each end, the two vertices extreme on
    // those other two axes.
    for (int axis = 0; axis < 3; ++axis) {
        const int b = (axis + 1) % 3, d = (axis + 2) % 3;
        for (int sb = 0; sb < 2; ++sb)
            for (int sd = 0; sd < 2; ++sd) {
                std::vector<std::uint32_t> loop;
                for (int s = 0; s < 2; ++s) {
                    const int corner = (s << axis) | (sb << b) | (sd << d);
                    loop.push_back(vid(corner, b));
                    loop.push_back(vid(corner, d));
                }
                faces.push_back(loop);
            }
    }
    // Eight corner triangles.
    for (int corner = 0; corner < 8; ++corner)
        faces.push_back({vid(corner, 0), vid(corner, 1), vid(corner, 2)});

    for (const std::vector<std::uint32_t>& loop : faces) {
        // Order the loop around its own centroid so a fan is a simple polygon —
        // the collection order above is not a ring.
        cfloat3 centre = cf3(0, 0, 0);
        for (std::uint32_t v : loop) centre = centre + m.positions[v];
        centre = centre * (1.0f / static_cast<float>(loop.size()));
        const cfloat3 out = clay::kernel::cnormalize(centre);
        // Two in-plane axes from the first vertex, to sort by angle.
        const cfloat3 ex = clay::kernel::cnormalize(m.positions[loop[0]] - centre);
        const cfloat3 ey = clay::kernel::ccross(out, ex);
        std::vector<std::pair<float, std::uint32_t>> ring;
        for (std::uint32_t v : loop) {
            const cfloat3 r = m.positions[v] - centre;
            ring.emplace_back(std::atan2(clay::kernel::cdot(r, ey), clay::kernel::cdot(r, ex)), v);
        }
        std::sort(ring.begin(), ring.end(),
                  [](const std::pair<float, std::uint32_t>& a,
                     const std::pair<float, std::uint32_t>& b) { return a.first < b.first; });
        for (std::size_t i = 1; i + 1 < ring.size(); ++i) {
            std::uint32_t t[3] = {ring[0].second, ring[i].second, ring[i + 1].second};
            const cfloat3 n = clay::kernel::ccross(m.positions[t[1]] - m.positions[t[0]],
                                                   m.positions[t[2]] - m.positions[t[0]]);
            if (clay::kernel::cdot(n, out) < 0.0f) std::swap(t[1], t[2]);
            m.indices.insert(m.indices.end(), {t[0], t[1], t[2]});
        }
    }
    return m;
}

// A big, irregular surface for the cases that only appear at scale. `subdiv`
// is the sphere's tessellation; 512 x 1024 is about a million triangles.
//
// NOISY on purpose, and deterministically so: a smooth sphere at a million
// triangles exercises the triangle count and nothing else, while displaced
// vertices give the tree an uneven distribution and the band an uneven
// thickness, which is what a real sculpt looks like to this operation.
inline Mesh noisy_sphere(int rings = 512, int segments = 1024, float amplitude = 0.06f) {
    Mesh m = sphere(1.0f, cf3(0, 0, 0), rings, segments);
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const cfloat3 p = m.positions[i];
        const float n = std::sin(9.0f * p.x) * std::sin(11.0f * p.y) * std::sin(7.0f * p.z);
        const float len = clay::kernel::clength(p);
        if (len > 1e-6f) m.positions[i] = p * (1.0f + amplitude * n / len);
    }
    return m;
}

}  // namespace clay_test
