#pragma once

// Picking & interaction math (picking spec): CPU-side, called every Pencil
// event. Ray <-> scene raycast with layer/item attribution (analytic tape or
// brick cache, whichever the caller says is fresher), surface snapping by
// gradient descent, voxel cell/face picking, and selection-bounds utilities.

#include <functional>
#include <optional>
#include <vector>

#include "clay/brick/cache.h"
#include "clay/mesh/bvh.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/groups.h"

namespace clay {
namespace pick {

struct RaycastOptions {
    float tmin = 0.0f;
    float tmax = 1e6f;
    float eps = 1e-4f;
    int max_steps = 256;

    // Surface groups the artist has put away (add-surface-groups). Null means
    // the document has none, which is every document that never named a region.
    //
    // A hidden hit is SKIPPED AND THE MARCH CONTINUES, rather than being turned
    // into a miss. Hiding the front of a head is how an artist reaches the
    // inside of it, so a ray that stopped at hidden surface would make the
    // feature useless for the thing it exists to do — the point is to pick what
    // is BEHIND what was hidden.
    //
    // The field is not modified, so the ray still marches the true surface and
    // simply refuses to stop on the parts that are hidden.
    const voxel::GroupField* groups = nullptr;

    // Whether the march may run on a tape culled to the ray's own segment when
    // the document's tape carries a Lipschitz bound above 1 (see raycast_scene
    // for why that is worth a compile). Off, the march takes the whole
    // document's step scale everywhere; the option exists so a test can hold
    // the two marches against each other, not for a caller to tune.
    bool local_tape = true;
};

struct SceneHit {
    bool hit = false;
    float t = 0.0f;
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);
    scene::LayerId layer = 0;   // attribution (0 = none)
    scene::NodeId item = scene::kNoNode;
    // Sphere-march samples taken, summed over hidden-surface restarts. What a
    // step-scale claim is checked against: the same hit in fewer steps IS the
    // win, and there is no other way to see it from outside.
    int steps = 0;
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
//
// `index` is the document's cull index (scene/cull_index.h) and `plan` a
// coarse cull of it over a region containing `cull`; both consulted only with
// a `cull`, both the pure accelerations compile_document takes them as (cached
// bounds and a pre-pruned walk instead of per-node recomputation). They are
// dropped when a ghosted layer forces the compile onto a copy of the document,
// because the index caches by layer address and every lookup against the copy
// would miss.
scene::Tape pickable_tape(const scene::Document& doc, const scene::CullRegion* cull = nullptr,
                          const scene::CullIndex* index = nullptr,
                          const scene::CullPlan* plan = nullptr);

// PROJECT A POINT ONTO THE SURFACE, searching BOTH ways within a distance
// (add-claycore-bridge).
//
// The query a bake cage is, and a snap tool too. Given a point and a direction
// — usually a low-polygon vertex and its normal — find the nearest surface
// within `max_distance`, and report how far it was.
//
// BOTH DIRECTIONS, and that is the part a first implementation gets wrong. A
// cage point produced from a low-polygon mesh may sit INSIDE the high-polygon
// surface or outside it, depending on whether the low-poly bulges or pinches
// there, and the caller cannot know which. Searching only outward silently
// misses every point where the low-poly sits inside — which is most of a
// concave region, and exactly where a bake looks wrong.
//
// THE SIGNED DISTANCE IS RETURNED BY THIS CALL and not computed by the caller,
// because it IS the height-map value and recomputing it from the returned point
// is a second chance to disagree about the sign. Positive means the surface was
// found along `direction`, negative against it.
//
// BOUNDED BY CONSTRUCTION: an unbounded search is not a cage, it is a nearest-
// surface query, and it will happily return a point on the other side of the
// model. A miss within the bound is reported as a miss rather than as a distant
// hit — that distinction is what keeps garbage out of the seams of a bake.
struct Projection {
    bool hit = false;
    float distance = 0.0f;  // SIGNED, along `direction`
    kernel::cfloat3 position = kernel::cf3(0, 0, 0);
    kernel::cfloat3 normal = kernel::cf3(0, 1, 0);
};

Projection project_to_surface(const scene::Tape& tape, kernel::cfloat3 point,
                              kernel::cfloat3 direction, float max_distance);

// The next surface crossing along a ray that is NOT hidden, or a negative t if
// there is none before `tmax`.
//
// WHY A SCAN AND NOT ANOTHER SPHERE-MARCH, which is the obvious thing and does
// not work. Skipping a hidden hit leaves the ray INSIDE the shape it just hit,
// where the distance field is negative and a sphere-march cannot take a step —
// so a re-march simply misses. Stepping forward until the field goes positive
// again does not work either, for a subtler reason: the point where it goes
// positive IS the far surface, the very thing being looked for, so that walk
// overshoots the answer by construction.
//
// What is actually wanted is the next SIGN CHANGE whose point is visible, so
// this scans for one and bisects it. The step should be the group lattice's
// cell size: a feature thinner than the lattice cannot be group-addressed
// anyway, so a scan finer than that resolves detail the hidden set cannot
// describe.
//
// `field` is the signed distance along the ray's own parameterisation.
float next_visible_crossing(const std::function<float(kernel::cfloat3)>& field,
                            const math::Ray& ray, float t_start, float tmax, float step,
                            const voxel::GroupField& groups);

SceneHit raycast_scene(const scene::Document& doc, const math::Ray& ray,
                       const RaycastOptions& options = {});

// The same march against a pickable tape the caller already holds — the C
// ABI's per-revision cached one — so a pick does not compile the document it
// was just handed. `tape` MUST be pickable_tape(doc) for this `doc` (or a
// byte-identical copy): the hit is marched on the tape and attributed on the
// document, and the two disagreeing is a hit on one surface named after
// another. `index` is the document's cull index, or null; it only speeds the
// ray-local compile below, it never changes the answer.
//
// THE STEP SCALE A RAY PAYS. The march steps by the field times the tape's
// safe_step_scale, and that scale is ONE number folded over every visible
// node — the worst Lipschitz bound anywhere in the document. One twisted box
// parked two units from the model drops it from 1 to 0.28, and a ray nowhere
// near the box takes 2.4x the steps it needs, because the bound cannot say
// WHERE the field is steep. A tape culled to the ray's own segment can: an
// item whose influence bound misses the segment is dropped, and its Lipschitz
// contribution with it, so the culled tape's scale is what the field along
// THAT ray allows. The culled tape is exact wherever the march evaluates —
// the per-brick cull's guarantee, applied to a box around the segment instead
// of a brick — so the hit is the same surface; only the step length changes.
// Compiled only when the whole tape's scale is below 1 (there is nothing to
// win otherwise), used only when its scale is larger (a ray straight through
// the steep item gains nothing and marches the tape it already had).
SceneHit raycast_scene(const scene::Document& doc, const scene::Tape& tape,
                       const scene::CullIndex* index, const math::Ray& ray,
                       const RaycastOptions& options = {});

// Raycast against a filled brick cache (trilinear narrow-band samples, brick
// DDA across non-surface bricks). Position/normal only — pass the document
// to attribute() for ids.
SceneHit raycast_bricks(const brick::BrickCache& cache, const math::Ray& ray,
                        const RaycastOptions& options = {});

// Attribute a world position to the nearest (layer, item) of the document.
//
// The layer first, by |field| of each visible, unghosted SDF layer's own tape
// at the position; then the item within it, by |field| of each candidate
// item's own tape (scene::compile_item — the item as an Add, alone, under the
// layer's placement and mirror), over the items whose influence bound reaches
// the position. Subtract items attribute their carved surfaces that way: the
// shape they carved with is the surface the position sits on.
//
// The layer tapes are compiled per call here. The overload below is handed
// the document's pickable tape and, when ONE layer is a candidate, reads the
// winner off that instead of compiling it again: a pick runs per Pencil
// event on a document that has not changed, and at 1,500 items compiling the
// one layer was a third of the attribution. With several candidate layers it
// compiles each, as this one does — the whole tape is their union and cannot
// say which of them the position is nearest.
void attribute(const scene::Document& doc, kernel::cfloat3 position, scene::LayerId* layer,
               scene::NodeId* item);

// The same attribution, given the tape the position was found on. `tape`
// MUST be pickable_tape(doc) for this `doc` (or a byte-identical copy), on
// the terms raycast_scene's tape overload states: it stands in for the one
// candidate layer's own tape, and a tape of some other document would name
// a layer the position is not on. Empty means no layer: nothing is compiled
// on the way to that answer, so the answer is the one the per-call compile
// gives.
void attribute(const scene::Document& doc, const scene::Tape& tape, kernel::cfloat3 position,
               scene::LayerId* layer, scene::NodeId* item);

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
                     const math::Transform& xform = math::Transform::identity(), float tmax = 1e6f);

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
