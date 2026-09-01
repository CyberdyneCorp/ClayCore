#pragma once

// THE FRAME A DETAIL COEFFICIENT IS MEASURED IN (mesh-multires spec,
// add-mesh-multires).
//
// A wrinkle is not a world-space vector. Store it as one and it is correct
// until the moment the feature exists for: bend the cheek at a coarse level and
// the stored `(0, 0.1, 0)` still points at the ceiling, so the wrinkle shears
// off the surface that carried it. Store it as "0.1 away from the surface" and
// bending the cheek carries it along, because the surface is what the number
// refers to.
//
// So detail is three coefficients against an orthonormal frame — tangent,
// bitangent, normal — and this file is where the frame comes from.
//
// THE FRAME IS TRANSPORTED, NOT REBUILT, and that distinction is the whole of
// D3. A tangent re-derived from "whichever neighbour comes first geometrically"
// flips under a deformation that barely moves the surface, and a flipped frame
// ROTATES the detail stored in it — a swimming, smearing artefact that appears
// in a render and in no numeric test that only checks magnitudes. So a frame is
// built ONCE at the base from data that cannot flip (a deterministic
// index-chosen neighbour, or the UV parametrization where one exists), and every
// level above it receives its parent's tangent rotated by the SHORTEST ARC that
// takes the parent's normal onto the child's. Nothing is re-derived, so nothing
// can disagree with what it was a frame ago.
//
// ONE OF THREE THINGS CALLED A FRAME IN `mesh`, and the distinction is the
// contract rather than the shape. `mesh::BrushFrame` (`brush_model.h`) is an
// ENUM naming the direction a kernel displaces along. `mesh::StampFrame`
// (`stamp_frame.h`) has the same three axes this one has and the OPPOSITE rule:
// it is rebuilt from scratch for every stamp, which is exactly what a detail
// frame may never be. Sharing one struct between the two would put two opposite
// rules on one type.
//
// THE FRAME IS BUILT FROM THE PURE SUBDIVISION SURFACE, never from the surface
// with detail already applied. That is what makes a coefficient invariant: if
// the frame moved when the detail did, writing a detail would change the
// meaning of the detail already there.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clay/kernel/shim.h"
#include "clay/mesh/subdivide.h"

namespace clay {
namespace mesh {

// Orthonormal and right-handed: `bitangent == cross(normal, tangent)`.
struct SurfaceFrame {
    kernel::cfloat3 tangent = kernel::cf3(1, 0, 0);
    kernel::cfloat3 bitangent = kernel::cf3(0, 0, 1);
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);
};

// A world offset from three coefficients, and back. Inverse of each other to
// float precision for any orthonormal frame, which `test_surface_frame` pins.
inline kernel::cfloat3 frame_to_world(const SurfaceFrame& f, float t, float b, float n) {
    return f.tangent * t + f.bitangent * b + f.normal * n;
}
inline void world_to_frame(const SurfaceFrame& f, kernel::cfloat3 d, float* t, float* b, float* n) {
    *t = kernel::cdot(d, f.tangent);
    *b = kernel::cdot(d, f.bitangent);
    *n = kernel::cdot(d, f.normal);
}

// -- normals ------------------------------------------------------------------

// Area-weighted vertex normals over a level, from the NEWELL normal of each
// face.
//
// Newell rather than a corner cross product because a subdivided quad is not
// planar — nothing makes the four points of a child quad coplanar, and a normal
// taken from one corner's cross product would depend on which corner. Newell's
// sum is the area vector of the polygon whatever it does out of plane, and its
// LENGTH is twice the area, so summing the unnormalized face normals over a
// vertex's faces is the area weighting for free.
void level_normals(const LevelTopology& topology, const LevelConnectivity& conn,
                   const std::vector<kernel::cfloat3>& positions,
                   std::vector<kernel::cfloat3>* out);

// The same for a subset. `inout` must already be sized to the level; entries
// outside `vertices` are neither read nor written.
void level_normals_partial(const LevelTopology& topology, const LevelConnectivity& conn,
                           const std::vector<kernel::cfloat3>& positions,
                           const std::vector<std::uint32_t>& vertices,
                           std::vector<kernel::cfloat3>* inout);

// -- frames -------------------------------------------------------------------

// The base level's frames.
//
// `uvs`, when given, must be parallel to `positions` and is used for the
// tangent — the artist-meaningful choice, because a detail authored against a
// texture direction stays aligned to it. It is IGNORED when the cage has UV
// seams, and that is deliberate rather than a limitation: a seam's two sides
// carry different UVs for the same geometric point, so a UV tangent there is
// discontinuous across the seam and the detail on either side of it would sit
// at different angles. The geometric tangent is continuous everywhere and
// costs only that the tangential direction is arbitrary — which for a
// coefficient triple is no cost at all, since the frame is the same one on
// every evaluation.
//
// The geometric tangent points from the vertex toward its LOWEST-INDEXED ring
// neighbour, projected into the tangent plane. Chosen by INDEX rather than by
// geometry, so it cannot change under a deformation.
void build_base_frames(const LevelTopology& topology, const LevelConnectivity& conn,
                       const std::vector<kernel::cfloat3>& positions,
                       const std::vector<kernel::cfloat3>& normals,
                       const std::vector<kernel::cfloat2>* uvs,
                       std::vector<SurfaceFrame>* out);

// The same for a subset of the base's vertices, bit-identical to the full call
// for the entries it writes. What a cage edit needs: a level-0 sculpt moves the
// surface under its own frames, and rebuilding every frame on the cage per dab
// would make the coarse level the expensive one to work at.
void build_base_frames_partial(const LevelTopology& topology, const LevelConnectivity& conn,
                               const std::vector<kernel::cfloat3>& positions,
                               const std::vector<kernel::cfloat3>& normals,
                               const std::vector<kernel::cfloat2>* uvs,
                               const std::vector<std::uint32_t>& vertices,
                               std::vector<SurfaceFrame>* inout);

// The child level's frames, from the parent's. `child_normals` is the child
// level's normals over the PURE subdivision positions.
//
// A vertex point inherits its own parent's frame; an edge point the mean of its
// two endpoints'; a face point the mean of its face's corners'. Each is then
// rotated by the shortest arc from the source normal onto the child's and
// re-orthonormalized, so the tangent stays as close to the parent's as the
// change in normal allows.
void transport_frames(const LevelTopology& parent, const LevelConnectivity& conn,
                      const std::vector<SurfaceFrame>& parent_frames,
                      const std::vector<kernel::cfloat3>& child_normals,
                      std::vector<SurfaceFrame>* out);

// The same for a subset, bit-identical to the full call for the entries it
// writes.
void transport_frames_partial(const LevelTopology& parent, const LevelConnectivity& conn,
                              const std::vector<SurfaceFrame>& parent_frames,
                              const std::vector<kernel::cfloat3>& child_normals,
                              const std::vector<std::uint32_t>& child_vertices,
                              std::vector<SurfaceFrame>* inout);

// Rotate `v` by the shortest arc taking `from` onto `to`, both unit. Exposed
// because the transport above is not the only caller that has to move a
// direction with a surface, and a second implementation would be a second
// answer.
kernel::cfloat3 rotate_shortest_arc(kernel::cfloat3 v, kernel::cfloat3 from, kernel::cfloat3 to);

}  // namespace mesh
}  // namespace clay
