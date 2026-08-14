#pragma once

// Picking & interaction math (picking spec): CPU-side, called every Pencil
// event. Ray <-> scene raycast with layer/item attribution (analytic tape or
// brick cache, whichever the caller says is fresher), surface snapping by
// gradient descent, voxel cell/face picking, and selection-bounds utilities.

#include <optional>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/mesh/bvh.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"

namespace clay {
namespace pick {

struct RaycastOptions {
    float tmin = 0.0f;
    float tmax = 1e6f;
    float eps = 1e-4f;
    int max_steps = 256;
};

struct SceneHit {
    bool hit = false;
    float t = 0.0f;
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);
    scene::LayerId layer = 0;   // attribution (0 = none)
    scene::NodeId item = scene::kNoNode;
};

// Analytic raycast against the document's compiled tape, with hit
// attribution to the closest layer and edit item (by field proximity at the
// hit point; subtract items attribute their carved surfaces).
// The tape every picking path should evaluate: the document, with ghosted
// layers taken out. Ghost means "show me this but stay out of my way", so a
// ghosted layer is still evaluated and still drawn — it is only unpickable and
// unedited. Locked layers stay in: locking protects against edits, not
// against selection.
//
// Exposed rather than kept private because a host doing its own ray marching
// or snapping needs the same tape, and picking that disagreed with itself
// depending on which entry point ran would be worse than no ghosting at all.
scene::Tape pickable_tape(const scene::Document& doc, const scene::CullRegion* cull = nullptr);

SceneHit raycast_scene(const scene::Document& doc, const math::Ray& ray,
                       const RaycastOptions& options = {});

// Raycast against a filled brick cache (trilinear narrow-band samples, brick
// DDA across non-surface bricks). Position/normal only — pass the document
// to attribute() for ids.
SceneHit raycast_bricks(const brick::BrickCache& cache, const math::Ray& ray,
                        const RaycastOptions& options = {});

// Attribute a world position to the nearest (layer, item) of the document.
void attribute(const scene::Document& doc, kernel::cfloat3 position, scene::LayerId* layer,
               scene::NodeId* item);

// -- mesh picking ------------------------------------------------------------
//
// A mesh layer carries triangles that never enter a tape, so raycast_scene
// cannot see one and never will — that is the layer's whole design. A brush on
// a mesh layer still needs a surface point and a normal, and this is where it
// gets them.
//
// The BVH is the caller's, not built here: building it is the expensive part
// and a stroke does hundreds of these. mesh::MeshSculptor keeps one.

struct MeshHit {
    bool hit = false;
    float t = 0.0f;
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);  // world space, unit
    std::uint32_t triangle = 0;
    float u = 0.0f, v = 0.0f;  // barycentrics on that triangle
};

// Raycast a mesh held under `xform`. The conversion is done HERE — the ray into
// layer space, the hit back into world — because a caller doing it by hand gets
// a brush whose radius changes when a layer is scaled, and gets it wrong
// silently.
//
// The normal is interpolated from the mesh's own vertex normals when it has
// them and is the geometric face normal when it does not, so a model imported
// without normals is still pickable.
MeshHit raycast_mesh(const mesh::Mesh& m, const mesh::Bvh& bvh, const math::Ray& ray,
                     const math::Transform& xform = math::Transform::identity(),
                     float tmax = 1e6f);

// -- surface snapping --------------------------------------------------------

struct SnapResult {
    bool ok = false;
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);  // outward
};

// Gradient-descent closest-point-on-surface; converges for points within a
// few band-widths of the surface. Position and position+normal modes share
// this call — the normal is always filled.
SnapResult snap_to_surface(const scene::Tape& tape, kernel::cfloat3 p, int max_iters = 12,
                           float tolerance = 1e-4f);

// -- voxel picking -----------------------------------------------------------

// Entry-face ids: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z (the face the ray
// entered the cell through — its neighbor is where a new voxel goes).
struct VoxelHit {
    bool hit = false;
    voxel::VoxelCoord cell;
    int face = 0;
    float t = 0.0f;
};

VoxelHit raycast_voxels(const voxel::VoxelGrid& grid, const math::Ray& ray, float tmax = 1e6f);

// The neighbor cell across the entry face (placement target).
voxel::VoxelCoord adjacent_cell(const VoxelHit& hit);

// Build-plane resolution (delegates to the grid; here so all interaction
// queries live in one module).
std::optional<voxel::VoxelCoord> pick_build_plane(const voxel::VoxelGrid& grid,
                                                  const math::Ray& ray,
                                                  std::int32_t plane_cell);

// -- bounds utilities --------------------------------------------------------

// World-space shape bounds (no blend/rounding dilation — the tight box for
// zoom-to-selection) of a set of nodes in a layer.
math::Aabb selection_bounds(const scene::Document& doc, scene::LayerId layer,
                            const std::vector<scene::NodeId>& nodes);

// Whole-layer shape bounds.
math::Aabb layer_bounds(const scene::Layer& layer);

}  // namespace pick
}  // namespace clay
