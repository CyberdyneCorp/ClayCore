#pragma once

// MASKS DERIVED FROM THE SURFACE, not painted onto it.
//
// The mask verbs were paint, fill, expand, contract, smooth, invert and
// to_field — every one of them something an artist DOES. Nothing derived a mask
// from the shape, so "mask the crevices", "mask the hard edges" and "mask
// everything facing up" — which is how a sculptor actually reaches for a mask
// most of the time — had to be painted by hand or invented by the host.
//
// WHY THIS IS CHEAP HERE, and expensive in a mesh engine. A mesh has to
// estimate curvature from a vertex ring, which is a discrete approximation with
// a valence-dependent error. A distance field's curvature is its LAPLACIAN, and
// the sign is unambiguous: for f = |p| - R (a sphere of radius R), the Laplacian
// at the surface is 2/R, POSITIVE for a convex surface. So a six-point stencil
// gives cavity and convexity directly, with no topology to be careful about.
//
// WHAT IS HERE, and what deliberately is not. Curvature, cavity, convexity and
// normal-direction are all a finite difference away — one stencil, no marching.
// THICKNESS and AMBIENT OCCLUSION are not: both need rays cast from the
// surface, which is a different cost class and a different set of parameters
// (ray count, length, falloff), so they are their own change rather than two
// more enumerators here pretending to be as cheap as the rest.

#include <functional>

#include "clay/math/geom.h"
#include "clay/parallel/cancel.h"
#include "clay/brush/surface_measure.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace brush {

// SurfaceMeasure and MeasureSettings now live in surface_measure.h, with the
// per-point form of the same measures. This file keeps only the LATTICE form:
// the same numbers, walked over a region and banded to the surface.
//
// One implementation, two shapes — which is what makes "the mask and a baked
// map agree about this surface" a construction rather than a claim.

struct ProceduralMaskSettings {
    // The lattice the mask is built on. 0 derives one from the region, which is
    // a guess — a caller that has a resolution in mind should say so.
    float cell_size = 0.0f;
    // Where to look. A procedural mask is bounded by the caller because a field
    // has no inherent extent, and walking an unbounded region is not an
    // operation with an end.
    math::Aabb region;
    // How close to the surface a cell must be to be considered part of it, in
    // world units. 0 derives two cells. Cells outside the band are left at
    // zero: a mask is about the SURFACE, and a measure taken deep inside a
    // solid describes nothing an artist can see.
    //
    // THE BANDING IS THIS FILE'S JOB and not measure_at's — a caller sampling a
    // mesh's vertices already has surface points and wants no band at all.
    float band = 0.0f;

    // Everything about the measure itself. Shared with the per-point form, so
    // the two cannot drift: `scale`, the normal direction and its threshold,
    // and the ray parameters ambient occlusion and thickness need.
    MeasureSettings measure;
};
// Build a mask from a field.
//
// The source is a CALLABLE rather than a document or a layer, for the reason
// `mask_to_field` gives in reverse: a mask sits above `scene` and a sampled
// field below it, so naming either type here would decide a layering question
// this function has no business deciding. A host passes its tape's eval.
//
// CANCELLABLE, because it walks a lattice and a fine one over a large region is
// not a frame's work. A cancelled call returns an EMPTY mask and sets
// `out_cancelled` — which is how a caller tells it apart from "the region
// contained no surface", since both come back empty.
voxel::MaskField mask_from_surface(const std::function<float(kernel::cfloat3)>& source,
                                   SurfaceMeasure measure,
                                   const ProceduralMaskSettings& settings,
                                   parallel::CancelToken* token = nullptr,
                                   bool* out_cancelled = nullptr);

}  // namespace brush
}  // namespace clay
