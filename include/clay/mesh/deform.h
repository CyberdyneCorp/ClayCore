#pragma once

// Whole-form deformers on a mesh layer (add-mesh-deformers).
//
// The Deformation-palette transforms — taper and twist — applied to a mesh
// layer's own vertices, in a frame the caller supplies.
//
// WHY THESE LIVE HERE AND NOT IN kernel/deform.h. A deformer on an SDF item is
// an INVERSE point map: it answers "where did the material at p come from",
// because that is what a field evaluation needs. A deformer on a mesh is the
// FORWARD map: the vertex is already the material, and it moves. These are the
// forward halves, and they are CPU-only — a mesh deform runs once per vertex on
// the host, so putting them in the kernel header would recompile five dialects
// for a path four of them cannot reach.
//
// The forward halves exist at all because both maps compute their amount from
// the coordinate ALONG the axis, and neither moves that coordinate: a taper
// scales x and z from y, a twist rotates x and z about y. So the forward map is
// the same arithmetic with the scale reciprocated or the angle negated, and a
// point round trips through both to within float epsilon.
//
// `bend` is NOT here, and its absence is a measurement rather than an omission:
// `cbend_point` takes its angle from `p.x` and then moves `p.x`, so it has no
// closed-form forward map, and past a gentle angle it has none at all — the
// deformation folds over, mapping distinct rest points onto the same place.
// See the proposal for the numbers.

#include <cstdint>

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

// Which transform. Named rather than a bare enum of ints so a host cannot pass
// a deformer that does not exist and get the first one.
enum class MeshDeform : std::uint8_t {
    Taper = 0,  // cross-section scale ramps across the span
    Twist = 1,  // rotation about the axis ramps across the span
};

// A deformer's own frame — the gizmo, in effect.
//
// The kernel's canonical taper and twist are maps about Y, and an SDF item
// supplies another axis through its own transform. A mesh layer has no item
// transform to borrow, so the frame is the deformer's own: `origin` is where
// the span starts and `axis` is the direction it runs. A caller aligning a
// gizmo to a limb sets these two and nothing else.
struct MeshDeformSettings {
    MeshDeform verb = MeshDeform::Taper;

    kernel::cfloat3 origin = kernel::cf3(0, 0, 0);
    kernel::cfloat3 axis = kernel::cf3(0, 1, 0);  // normalised on use
    // How far along `axis` the ramp runs, from `origin`. Material before the
    // span is untouched and material past it travels rigidly with the end,
    // which is what makes a gizmo box's ends mean something rather than
    // letting the deformation keep going for ever.
    float span = 1.0f;

    // Taper: the cross-section scale at the start and end of the span. 1 and 1
    // is the identity.
    float scale_start = 1.0f;
    float scale_end = 1.0f;

    // Twist: total rotation across the span, in radians. 0 is the identity.
    float angle = 0.0f;

    // Which easing curve ramps the amount across the span; the same curve
    // enumeration the SDF deformers take.
    int ease = 0;

    // Whether these parameters describe no deformation at all. An identity
    // deformer must not be walked: it would rewrite every vertex with itself
    // and fill an undo record with entries that changed nothing.
    bool is_identity() const;
};

// Where `p` goes. Exposed so a host can preview the warp — and draw its gizmo
// — without applying it, exactly as the mesh lattice exposes its displacement.
kernel::cfloat3 deform_point(const MeshDeformSettings& s, kernel::cfloat3 p);

}  // namespace mesh
}  // namespace clay
