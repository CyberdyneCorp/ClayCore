#pragma once

// Placing an alpha stamp where a user clicked (brush-engine spec,
// add-sdf-alphas).
//
// The deformer takes a centre, a direction and a tangent. A host that has a
// surface hit has the first two immediately — but the TANGENT is the one a
// host would derive itself, and deriving it differently from the engine is
// exactly how a stamp ends up rotated a few degrees off between the preview
// and the commit. So it is derived once, here.
//
// Small on purpose. This is not a stroke engine for alphas; it is the frame,
// and the reason it is a function rather than a comment is that "any
// perpendicular will do" is true of the MATH and false of the workflow — a
// stamp whose rotation jitters as the camera moves is unusable even though
// every individual frame is valid.

#include "clay/kernel/shim.h"
#include "clay/scene/types.h"

namespace clay {
namespace brush {

// Where a stamp sits and how it is turned.
struct StampPlacement {
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    kernel::cfloat3 direction = kernel::cf3(0, 0, 1);  // where it pushes; the surface normal
    kernel::cfloat3 tangent = kernel::cf3(1, 0, 0);    // its "up" in the stamp plane
};

// The placement for a hit at `point` on a surface whose outward normal is
// `normal`.
//
// `up` is the direction the artist wants the stamp's +v to lean toward — a
// camera's up vector, or a fixed world axis for a stamp that should not spin
// as the view orbits. It is projected into the stamp's plane rather than used
// directly, so any rough value works; when it is parallel to the normal (a
// surface faced straight on with a camera up along it) the tangent falls back
// to a derived axis, which is stable for a given normal rather than arbitrary.
//
// `roll` turns the stamp in its own plane afterwards, in radians, which is the
// control an artist actually reaches for.
StampPlacement stamp_placement(kernel::cfloat3 point, kernel::cfloat3 normal,
                               kernel::cfloat3 up = kernel::cf3(0, 1, 0), float roll = 0.0f);

// The deformer that stamp places on an item, so a caller does not re-spell the
// argument order. `samples` are copied, as they are by Deformer::alpha.
scene::Deformer stamp_deformer(const StampPlacement& placement, const float* samples, int width,
                               int height, float extent, float radius, float amplitude,
                               std::uint8_t ease = 0);

}  // namespace brush
}  // namespace clay
