#pragma once

// REMOVING WHAT AN ARTIST PUT AWAY (add-surface-groups).
//
// `GroupField` could say a surface point was hidden and nothing asked. The
// visibility half of surface groups was a flag with no consumer: `isolate` was
// cheap, correct, and had no effect on anything the artist could see, which
// made the feature a bookkeeping exercise wearing a workflow name.
//
// WHY A TRIANGLE FILTER, and not a change to the field. The obvious
// implementation is to make a hidden region evaluate as empty, and it is wrong
// twice. It would change what the DOCUMENT EVALUATES TO — the invariant the
// layering table exists to protect, since `scene` cannot see `voxel` precisely
// so that a mask's or a group's presence cannot alter the field. And it would
// carve a hard boundary INTO the surface: the mesher would close the hole with
// new geometry along the cut, so showing the group again would not restore the
// original triangles. Hiding would not be reversible, and "hiding is not
// deleting" is the guarantee this whole mechanism rests on.
//
// Filtering the produced mesh instead leaves the field untouched, leaves the
// document untouched, and is exactly reversible: mesh again with nothing hidden
// and the original triangles come back, because they were never not produced.
//
// WHAT IT COSTS. The boundary is quantised to the group lattice and to the
// triangle, so a hidden region's edge is ragged at the scale of whichever is
// coarser — there is no cut, and a triangle is kept or dropped whole. That is
// the price the design already accepted for a lattice, stated again here
// because this is where an artist sees it.

#include <cstddef>

#include "clay/mesh/mesh_data.h"
#include "clay/voxel/groups.h"

namespace clay {
namespace voxel {

// Drop every face whose CENTROID falls in a hidden group, and compact the
// vertices nothing surviving references.
//
// A QUAD MESH IS FILTERED BY QUAD, and that is not a refinement — it is what
// keeps the function usable at all on the quad export path. mesh/mesh_data.h
// makes it a RULE that rewriting `indices` must clear `quads`, so a
// triangle-wise filter would hand back a quad export carrying no quads. Quad q
// owns corners quads[4q..4q+3] and triangles indices[6q..6q+5], so dropping
// both together leaves the arrays in lockstep by construction rather than by
// a check afterwards.
//
// The centroid rather than any vertex, and rather than all of them: a face
// spanning the boundary has to go one way or the other, and the centroid is the
// only choice that makes the kept region agree with the group's own extent
// rather than eroding or dilating it by one face.
//
// Returns how many faces went — quads where the mesh has them, triangles
// otherwise. Zero when nothing is hidden, and in that
// case the mesh is not touched at all — a document with no hidden group meshes
// to the bytes it always did, which is what makes this safe to call
// unconditionally on every meshing path.
std::size_t drop_hidden(mesh::Mesh& m, const GroupField& groups);

}  // namespace voxel
}  // namespace clay
