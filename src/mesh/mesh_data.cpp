#include "clay/mesh/mesh_data.h"

#include "clay/bytes.h"

namespace clay {
namespace mesh {

std::size_t Mesh::bytes() const {
    // Capacity, not size. A mesh that was decimated from two million triangles
    // to fifty thousand still holds the two million until someone shrinks it,
    // and the allocator is the one telling the truth about that.
    return sizeof(Mesh) + vector_bytes(positions) + vector_bytes(normals) +
           vector_bytes(colors) + vector_bytes(uvs) + vector_bytes(indices) +
           vector_bytes(quads);
}


std::vector<kernel::cfloat3> vertex_normals(const Mesh& m) {
    std::vector<kernel::cfloat3> out(m.positions.size(), kernel::cf3(0, 0, 0));
    const std::size_t tris = m.indices.size() / 3;
    for (std::size_t t = 0; t < tris; ++t) {
        const std::uint32_t a = m.indices[t * 3], b = m.indices[t * 3 + 1],
                            c = m.indices[t * 3 + 2];
        if (a >= out.size() || b >= out.size() || c >= out.size()) continue;
        // The cross product's LENGTH is twice the triangle's area, so
        // accumulating it unnormalised IS the area weighting — no separate term
        // and no extra square root per face.
        const kernel::cfloat3 n =
            kernel::ccross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
        out[a] = out[a] + n;
        out[b] = out[b] + n;
        out[c] = out[c] + n;
    }
    for (kernel::cfloat3& n : out) {
        const float len = kernel::clength(n);
        // A vertex touched by no triangle, or by triangles that cancel exactly,
        // has no normal to report. +Y rather than zero: a zero normal is not a
        // direction, and a consumer that normalises it gets NaN.
        n = len > 1e-20f ? kernel::cf3(n.x / len, n.y / len, n.z / len)
                         : kernel::cf3(0.0f, 1.0f, 0.0f);
    }
    return out;
}

}  // namespace mesh
}  // namespace clay
