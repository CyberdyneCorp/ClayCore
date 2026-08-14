#pragma once

// Triangle mesh interchange type shared by every producer (SDF meshing,
// voxel greedy meshing) and consumer (decimation, validation, io, C ABI).
// Flat buffers, indices into positions; normals/colors/uvs are optional and
// vertex-aligned when present.
//
// A mesh from a quad mesher (mesh/quad_mesh.h) additionally carries the quads
// it was built from, PARALLEL to the triangles rather than instead of them —
// see `quads` below.

#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

struct Mesh {
    std::vector<kernel::cfloat3> positions;
    std::vector<kernel::cfloat3> normals;  // empty or positions.size()
    std::vector<kernel::cfloat3> colors;   // empty or positions.size()
    std::vector<kernel::cfloat2> uvs;      // empty or positions.size()
    std::vector<std::uint32_t> indices;    // triangle list

    // The quads the mesher built, four indices per face — empty on every mesh
    // this library produced before quad meshing existed and on every mesh a
    // triangle producer returns.
    //
    // THE INVARIANT, which is why this is an addition rather than a
    // replacement: when `quads` is non-empty, `indices` holds exactly the
    // triangulation of those same quads over those same positions. Quad q with
    // corners (a, b, c, d) is triangles (a, b, c) and (a, c, d) at
    // indices[6q .. 6q+5], so indices.size() == quads.size() / 4 * 6. A
    // consumer that ignores this array sees the complete, correct triangle
    // mesh it saw before — which is what keeps decimation, the BVH,
    // validation, every exporter, the C accessors and the mesh stream working
    // byte-identically without being modified.
    //
    // The corollary is a rule, not a preference: any operation that REWRITES
    // `indices` must clear this array (mesh::drop_quads), or it leaves a quad
    // list describing triangles that no longer exist — a lie that survives
    // into a saved document. mesh::quads_consistent (mesh/quad_mesh.h) is what
    // the boundaries and the tests assert instead of trusting.
    std::vector<std::uint32_t> quads;

    std::size_t triangle_count() const { return indices.size() / 3; }
    std::size_t quad_count() const { return quads.size() / 4; }
    bool has_quads() const { return !quads.empty(); }
    bool empty() const { return indices.empty(); }
};

}  // namespace mesh
}  // namespace clay
