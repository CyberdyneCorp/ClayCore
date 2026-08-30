#pragma once

// Merging coincident vertices and dropping what that collapses (add-mesh-weld).
//
// WHY THIS IS NOT `Adjacency`'s WELD, which already exists and is a different
// verb. `Adjacency` groups vertices into weld CLASSES and leaves every one of
// them in the mesh: the classes exist so a brush moves a seam's duplicates
// together, and the whole point is that the mesh is unchanged — `indices` comes
// out byte-identical, which is the fixed-topology contract. This MERGES them:
// one vertex survives, the rest are gone, and any triangle whose corners
// collapsed onto each other is gone with it.
//
// WHAT NEEDED IT. The default mesher emits zero-area triangles — measured at
// 1458 of 70,140 on a plain sphere, two per cent, with two corners at
// bit-identical positions. Nothing downstream had noticed, because everything
// downstream tolerates them: an exporter writes them, a BVH holds them, the
// decimator drops them on its own. `mesh::DynamicSurface` cannot. A half-edge
// surface has no way to express a face with two of the same vertex, and the
// conversion refuses rather than silently dropping it — correctly, and with the
// result that NO mesh this library marches could be converted to an adaptive
// surface at all. That was true for `mesh_lattice`, for `mesh_tape` and for
// `voxel_remesh`, and no caller had ever tried it.
//
// The fix belongs here rather than inside that conversion. A conversion that
// repaired its input would fix one caller and leave the marcher still emitting
// degenerate faces into every other one; a named verb fixes it once, keeps the
// conversion's "refuse rather than repair" rule intact, and makes the cleanup
// visible at the call site — `from_mesh(weld(m))` reads as what it is.

#include <cstddef>

#include "clay/mesh/adjacency.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace mesh {

struct WeldOptions {
    // As a fraction of the bounding-box diagonal, the same convention and the
    // same default `Adjacency` uses — relative so it means the same thing on a
    // model authored in millimetres and one authored in metres. Zero welds only
    // BIT-IDENTICAL positions, which is what a marched mesh needs and all it
    // needs.
    float epsilon = kDefaultWeldEpsilon;

    // Refuse to merge two vertices whose UVs or colours disagree, even when
    // their positions coincide. ON by default, and the default is the whole
    // safety of this verb.
    //
    // A UV SEAM IS DUPLICATED POSITIONS WITH DIFFERENT UVS. That is not a
    // defect to be cleaned up, it is how a flat mesh represents a seam at all —
    // and merging across one silently destroys the layout, which is exactly the
    // loss `mesh/transfer.h` spends a paragraph explaining it cannot refund.
    // With this on, a seam survives welding untouched and only genuine
    // duplicates merge.
    bool preserve_attribute_splits = true;

    // How far two UVs or colours may differ and still count as agreeing. Zero
    // means exactly.
    float attribute_epsilon = 1e-6f;
};

// What welding actually did. Returned rather than inferred, for the reason
// `TransferReport` is: a weld that merged half the mesh and one that merged
// nothing are otherwise indistinguishable from the outside, and the caller who
// most needs to know is the one who did not expect either.
struct WeldReport {
    std::size_t vertices_before = 0;
    std::size_t vertices_after = 0;
    std::size_t triangles_before = 0;
    std::size_t triangles_after = 0;
    // Vertices that merged into another. `vertices_before - vertices_after` also
    // counts vertices dropped for being referenced by nothing, so the two
    // numbers are not the same and both are worth having.
    std::size_t vertices_merged = 0;
    // Triangles removed because two of their corners became one vertex. These
    // had no area to lose: removing one cannot open a hole, because a triangle
    // whose corners coincide bounds nothing.
    std::size_t triangles_collapsed = 0;
    // Triangles removed for naming a vertex that does not exist. Counted apart
    // from the collapsed because they are a different fault — the first is
    // geometry this verb is for, the second is a malformed index array — and a
    // caller seeing a non-zero count here has a bug somewhere upstream.
    std::size_t triangles_invalid = 0;
    // Vertices no triangle referenced, before or after.
    std::size_t vertices_unreferenced = 0;
    // The threshold actually used, in WORLD units — the relative epsilon
    // resolved against this mesh's own size, so a caller can see what it meant.
    float epsilon = 0.0f;
    bool quads_dropped = false;
};

// Merge coincident vertices in `m` and remove the triangles that collapses.
//
// WATERTIGHTNESS SURVIVES, and that is a property rather than a hope: merging
// two coincident vertices and dropping the triangle between them is an edge
// collapse of a zero-length edge, and the surrounding triangles still bound
// exactly the region they bounded before. A mesh that was watertight before is
// watertight after; one that was not is no worse.
//
// QUADS ARE DROPPED when anything changes, because this rewrites `indices` and
// mesh_data.h's rule is that a rewrite must clear them rather than leave a quad
// list describing triangles that no longer exist. A weld that changed nothing
// leaves the mesh — quads included — byte-identical.
//
// EVERY INDEX IS IN RANGE AFTERWARDS. A triangle naming a vertex that does not
// exist is removed, and that counts as work — so the byte-identical fast path
// below is taken only when there is genuinely nothing to do, rather than
// leaving a malformed mesh malformed because nothing else needed merging.
//
// DETERMINISTIC. The surviving vertex of a merged group is the one with the
// lowest index, and the output order is the input's with the merged-away
// removed, so the same mesh welds to the same bytes on every run and platform.
WeldReport weld(Mesh* m, const WeldOptions& options = {});

}  // namespace mesh
}  // namespace clay
