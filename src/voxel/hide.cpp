#include "clay/voxel/hide.h"

#include <vector>



namespace clay {
namespace voxel {

namespace {

kernel::cfloat3 centroid_of(const mesh::Mesh& m, const std::uint32_t* idx, std::size_t n) {
    kernel::cfloat3 c = kernel::cf3(0, 0, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const kernel::cfloat3 p = m.positions[idx[i]];
        c = kernel::cf3(c.x + p.x, c.y + p.y, c.z + p.z);
    }
    const float inv = 1.0f / static_cast<float>(n);
    return kernel::cf3(c.x * inv, c.y * inv, c.z * inv);
}

bool indices_in_range(const mesh::Mesh& m, const std::uint32_t* idx, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (idx[i] >= m.positions.size()) return false;
    return true;
}

// Rebuild from a kept index list, compacting the vertices nothing references —
// an unreferenced vertex is not merely dead weight, it is memory a host uploads
// to the GPU for nothing, and on a mostly hidden model that is most of it.
void compact(mesh::Mesh& m, const std::vector<std::uint32_t>& kept_tris,
             const std::vector<std::uint32_t>& kept_quads) {
    std::vector<std::uint32_t> remap(m.positions.size(), 0xffffffffu);
    mesh::Mesh out;
    const bool has_normals = m.normals.size() == m.positions.size();
    const bool has_colors = m.colors.size() == m.positions.size();
    const bool has_uvs = m.uvs.size() == m.positions.size();
    auto emit = [&](std::uint32_t i) {
        if (remap[i] == 0xffffffffu) {
            remap[i] = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(m.positions[i]);
            if (has_normals) out.normals.push_back(m.normals[i]);
            if (has_colors) out.colors.push_back(m.colors[i]);
            if (has_uvs) out.uvs.push_back(m.uvs[i]);
        }
        return remap[i];
    };
    out.indices.reserve(kept_tris.size());
    for (std::uint32_t i : kept_tris) out.indices.push_back(emit(i));
    out.quads.reserve(kept_quads.size());
    for (std::uint32_t i : kept_quads) out.quads.push_back(emit(i));
    m = std::move(out);
}

}  // namespace

std::size_t drop_hidden(mesh::Mesh& m, const GroupField& groups) {
    // Nothing hidden: leave the mesh strictly alone, so an unfiltered document
    // meshes to the bytes it always did.
    if (!groups.any_hidden() || m.indices.empty()) return 0;

    // A QUAD MESH IS FILTERED BY QUAD, not by triangle, and this is the whole
    // reason this function knows about quads at all. mesh/mesh_data.h makes it a
    // rule that rewriting `indices` must clear `quads` — so a triangle-wise
    // filter would hand back a quad export with no quads in it, which defeats
    // the one thing that export is for.
    //
    // Filtering by quad keeps the invariant instead of breaking it: quad q's
    // corners are quads[4q..4q+3] and its triangles are indices[6q..6q+5], so
    // dropping both together leaves the two arrays in lockstep by construction.
    if (!m.quads.empty() && m.indices.size() == m.quads.size() / 4 * 6) {
        const std::size_t quad_count = m.quads.size() / 4;
        std::vector<std::uint32_t> kept_tris, kept_quads;
        std::size_t dropped = 0;
        for (std::size_t q = 0; q < quad_count; ++q) {
            const std::uint32_t* corners = &m.quads[q * 4];
            if (!indices_in_range(m, corners, 4)) continue;
            if (groups.point_hidden(centroid_of(m, corners, 4))) {
                ++dropped;
                continue;
            }
            for (std::size_t k = 0; k < 4; ++k) kept_quads.push_back(corners[k]);
            for (std::size_t k = 0; k < 6; ++k) kept_tris.push_back(m.indices[q * 6 + k]);
        }
        if (dropped == 0) return 0;
        compact(m, kept_tris, kept_quads);
        return dropped;
    }

    const std::size_t tris = m.indices.size() / 3;
    std::vector<std::uint32_t> kept_tris;
    kept_tris.reserve(m.indices.size());
    std::size_t dropped = 0;
    for (std::size_t t = 0; t < tris; ++t) {
        const std::uint32_t* tri = &m.indices[t * 3];
        if (!indices_in_range(m, tri, 3)) continue;  // malformed: dropped, not dereferenced
        if (groups.point_hidden(centroid_of(m, tri, 3))) {
            ++dropped;
            continue;
        }
        for (std::size_t k = 0; k < 3; ++k) kept_tris.push_back(tri[k]);
    }
    if (dropped == 0) return 0;
    compact(m, kept_tris, {});
    return dropped;
}

}  // namespace voxel
}  // namespace clay
