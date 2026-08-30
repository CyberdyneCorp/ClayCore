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
#include "clay/math/transform.h"
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
//
// TAKES A LOCAL PLACEMENT. `Deformer::alpha` reads its centre, direction,
// tangent, extent and radius in the ITEM'S OWN space — the deformer chain runs
// on the local point (kernel/tape.h: `lp = cmul_point(inv, p)`), exactly as a
// bend curve's guide and a lattice's box do. `stamp_placement` above returns a
// WORLD frame, because a surface hit is world. The two coincide only on an
// item at the identity, so use the overload below whenever the item has a
// transform.
scene::Deformer stamp_deformer(const StampPlacement& placement, const float* samples, int width,
                               int height, float extent, float radius, float amplitude,
                               std::uint8_t ease = 0);

// A world placement, expressed in `item`'s frame.
//
// THE MISSING HALF OF THIS FILE, and the defect it closes is worth naming: a
// host that did what this header invites — take a surface hit, call
// `stamp_placement`, hand the result to `stamp_deformer` — got a world frame
// read as a local one. On an item at the identity that is the same thing, which
// is why every test and every example passed. On anything else the stamp lands
// somewhere the artist did not click, and the two symptoms are both silent:
// the region weight is zero once the misplaced centre is further than the
// radius (the stamp does NOTHING), and where it is not zero the sample lookup
// clamps u and v, so every point reads the same border texel and the stamp
// SMEARS uniformly instead of leaving its mark.
//
// Measured on a sphere at position (0.4, -0.2, 0.1), rotated 0.7 rad about y,
// scaled 0.35, with a 0.3-amplitude bump stamped at the world hit: the field
// moved 0.000013 at the click. Through this conversion it moves 0.296.
StampPlacement placement_in(const StampPlacement& world, const math::Transform& item);

// `stamp_deformer` with the conversion done for you.
//
// `extent`, `radius` and `amplitude` are divided by the item's scale as well,
// because they are lengths in the same local space: the tape multiplies the
// deformed result back by the scale, so an amplitude authored in world units
// arrives `scale` times too small without this.
scene::Deformer stamp_deformer_in(const StampPlacement& world, const math::Transform& item,
                                  const float* samples, int width, int height, float extent,
                                  float radius, float amplitude, std::uint8_t ease = 0);

}  // namespace brush
}  // namespace clay
