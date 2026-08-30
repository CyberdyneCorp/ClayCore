#pragma once

// SPLIT, COLLAPSE, FLIP — the three local operators (dynamic-topology spec,
// add-dynamic-topology).
//
// EVERY ONE OF THEM IS ATOMIC. It either applies completely or leaves the
// surface exactly as it found it; a refused operation changes nothing at all.
// That is not a nicety: a collapse is a dozen rewires, and one that gave up
// halfway would leave a surface that still renders and is quietly wrong in one
// fan — the failure the validator exists to catch and that no artist could
// report usefully.
//
// The way atomicity is achieved here is that every operator DECIDES FIRST and
// WRITES SECOND. All the validity tests run against the untouched surface, and
// only once they have all passed does a single write phase run, which cannot
// fail. There is no rollback path because there is nothing to roll back.
//
// AN OPERATOR HONOURS CONSTRAINTS ITSELF rather than trusting its caller to
// have filtered the input. An operator that is safe only when called correctly
// is a bug waiting for the second caller, and the remesher, the sculptor and a
// host script are three callers already.

#include <cstdint>

#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/topology_delta.h"

namespace clay {
namespace mesh {

// Why an operator declined. Reported rather than collapsed into a bool, because
// a remesher tuning its thresholds and a test pinning a refusal both need to
// know WHICH rule fired.
enum class TopologyResult : std::uint8_t {
    Ok = 0,
    // The handle was dead, or the surface is malformed there.
    InvalidInput,
    // An edge constraint said no: boundary, seam, sharp, material or locked.
    Constrained,
    // The link condition failed — collapsing would pinch the surface, creating
    // a non-manifold edge or a duplicate triangle.
    LinkCondition,
    // The result would turn a triangle inside out.
    WouldInvert,
    // The result would have a triangle of no area.
    WouldDegenerate,
    // A normal would swing further than the caller allowed.
    NormalFlip,
    // The diagonal a flip would create is already an edge.
    DiagonalExists,
    // A flip that does not improve the quality metric. Not an error: the
    // remesher asks about far more edges than it flips.
    NoImprovement,
};

inline bool succeeded(TopologyResult r) { return r == TopologyResult::Ok; }

struct SplitResult {
    TopologyResult result = TopologyResult::InvalidInput;
    // The vertex created at the split point, when one was.
    VertexId vertex;
    // The faces that now exist where the old ones were, for a local normal
    // refresh and a local index update. Up to four: two per side.
    FaceId faces[4];
    int face_count = 0;
};

struct CollapseResult {
    TopologyResult result = TopologyResult::InvalidInput;
    // The vertex that survived, and the one that did not.
    VertexId kept;
    VertexId removed;
    // The faces still incident to `kept` afterwards.
    std::vector<FaceId> faces;
};

struct FlipResult {
    TopologyResult result = TopologyResult::InvalidInput;
    FaceId faces[2];
};

struct TopologyOpOptions {
    // How far a face normal may swing before an operation is refused, in
    // radians. Guards the case the link condition cannot see: a collapse that
    // is topologically fine and geometrically folds the surface over itself.
    float max_normal_swing = 1.2f;  // ~69 degrees
    // Below this, a face has no meaningful normal and every operator downstream
    // would have to guard against it. Compared against twice the area, which is
    // what the cross product gives.
    float min_area_x2 = 1e-12f;
    // Which constraints stop a collapse. Boundary is NOT in the default,
    // because a boundary edge collapsing ALONG the boundary is exactly how a
    // border is allowed to simplify; what is forbidden is collapsing ACROSS
    // one, which the link condition catches on its own.
    std::uint32_t collapse_blockers =
        EdgeConstraint::Sharp | EdgeConstraint::Material | EdgeConstraint::UserLocked;
    // Which stop a flip. Every constraint does: a flip moves the edge itself,
    // so a constrained edge flipping is the constraint being deleted.
    std::uint32_t flip_blockers = EdgeConstraint::Boundary | EdgeConstraint::UvSeam |
                                  EdgeConstraint::Sharp | EdgeConstraint::Material |
                                  EdgeConstraint::UserLocked;
};

// SPLIT `edge` at parameter `t` along it, 0 at its origin and 1 at its target.
//
// Interior: two faces become four. Boundary: one becomes two.
//
// Position, colour, mask and CORNER UVs are interpolated — the corner UVs on
// each side independently, which is what lets a seam survive a split with both
// of its UVs intact. Normals are recomputed locally rather than interpolated,
// because an interpolated normal on a curved surface is not the surface's
// normal and every later operator would inherit the error.
SplitResult split_edge(DynamicSurface& surface, EdgeId edge, float t = 0.5f,
                       const TopologyOpOptions& options = {}, TopologyDelta* delta = nullptr);

// COLLAPSE `edge`, merging its two vertices into one.
//
// The most dangerous of the three, and the one with the most refusals. Validity
// is decided by a TOPOLOGICAL link condition and not by geometry alone: the
// standard test is that the intersection of the two endpoints' one-ring
// neighbourhoods is exactly the vertices opposite the edge, and anything else
// means the collapse would pinch the surface into a non-manifold state that
// looks fine in a render and is unusable afterwards.
//
// Placement is the midpoint, except where a constraint says otherwise: exactly
// one constrained endpoint keeps its position, so the feature does not move.
// See D12 in the change's design for why the decimator's quadric is not used.
CollapseResult collapse_edge(DynamicSurface& surface, EdgeId edge,
                             const TopologyOpOptions& options = {},
                             TopologyDelta* delta = nullptr);

// FLIP `edge` to the other diagonal of the quadrilateral its two faces form.
//
// Refused on a boundary, on any constrained edge, where the new diagonal
// already exists as an edge (which would make two edges between one pair of
// vertices), where either new face would invert, and — unless `force` — where
// the quality metric does not improve.
FlipResult flip_edge(DynamicSurface& surface, EdgeId edge,
                     const TopologyOpOptions& options = {}, TopologyDelta* delta = nullptr,
                     bool force = false);

// The quality of the two triangles an edge separates, as the smallest of their
// minimum angles. Higher is better; a sliver approaches zero. What `flip_edge`
// compares before and after.
float edge_pair_quality(const DynamicSurface& surface, EdgeId edge);

}  // namespace mesh
}  // namespace clay
