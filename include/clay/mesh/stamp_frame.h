#pragma once

// THE ORTHONORMAL BASIS ONE STAMP IS ORIENTED IN (brush-engine spec,
// add-shared-brush-runtime).
//
// WHAT THIS IS FOR. A directional brush — Rake, Chisel, Clay Strips, a
// scratched alpha, anything with a grain — needs an in-plane axis to be
// directional ALONG. The tree had exactly one such basis and it was
// `AlphaFrame`, which exists only when `settings.has_alpha()` and is named for
// the one consumer it had. A second directional family written against it would
// have had to either fake an alpha to get a frame, or build its own basis — and
// two re-orthogonalisations of the same normal are two chances to disagree
// about which way "along" points on the same stamp. This is that basis, built
// once per stamp, with the alpha demoted to one reading of it.
//
// THERE ARE THREE THINGS CALLED A FRAME IN `mesh`, AND THEY ARE DIFFERENT.
// A reader who meets one deserves to be told about the other two:
//
//   - `mesh::BrushFrame` (`brush_model.h`) is an ENUM naming the DIRECTION a
//     kernel displaces along — the region's normal, the vertex's own normal,
//     the stroke's direction, the plane. It keeps its name because it is
//     serialized at `BrushPreset` version 1 and mirrored in the public C ABI as
//     `clay_brush_frame`; renaming the C++ half alone would give one axis two
//     names that differ across the ABI boundary, which is the worst outcome
//     available.
//   - `mesh::StampFrame` (this file) is an orthonormal basis built FRESH for
//     every stamp, from where the brush landed.
//   - `mesh::SurfaceFrame` (`surface_frame.h`) is the frame a multiresolution
//     detail coefficient is measured in. Same shape as this one, opposite
//     contract: it is TRANSPORTED and never rebuilt, because rebuilding it
//     rotates the detail stored in it. Sharing one struct between the two would
//     put two opposite rules on one type.

#include "clay/kernel/shim.h"

namespace clay {
namespace mesh {

// Where the stamp is, which way it faces, and which way is "along".
//
// `normal` is the stamp's facing; `tangent` and `bitangent` span the plane it
// covers, right-handed with it (`bitangent == normal x tangent`). `rotation` is
// the angle actually applied, kept so a consumer can tell an unrotated frame
// from a rotated one without comparing axes.
struct StampFrame {
    kernel::cfloat3 origin = kernel::cf3(0, 0, 0);
    kernel::cfloat3 normal = kernel::cf3(0, 0, 1);
    kernel::cfloat3 tangent = kernel::cf3(1, 0, 0);
    kernel::cfloat3 bitangent = kernel::cf3(0, 1, 0);
    float rotation = 0.0f;
};

// The ONE angle a stamp frame is built at.
//
// An explicit rotation — a preset that pins its grain to a fixed direction —
// REPLACES the azimuth rather than composing with it, so that there is one
// rotation path and not the product of two. Composing them would mean a host
// that set both got an angle neither of them asked for, and would make the
// zero-azimuth identity below depend on two numbers instead of one.
inline float stamp_rotation_of(float azimuth, bool has_explicit, float explicit_rotation) {
    return has_explicit ? explicit_rotation : azimuth;
}

// Build the basis.
//
// `direction` is where the stamp pushes; it is normalized here and falls back
// to +Z when it is degenerate. `tangent_hint` is any rough "up": it is
// re-orthogonalised against the direction, and a hint parallel to it (or absent)
// is replaced by an axis the direction leans on least. Both of those happen
// inside `kernel::calpha_frame`, which is called EXACTLY ONCE with exactly
// those two arguments — the same function the SDF alpha uses, so one stamp
// reads identically on a mesh and on a field rather than through two
// re-orthogonalisations that could drift apart.
//
// A ZERO AZIMUTH TAKES NO ROTATION AT ALL, and that is a correctness rule
// rather than a micro-optimisation. See the note on the implementation: `x +
// 0.0f` is not the identity when `x` is `-0.0f`.
StampFrame make_stamp_frame(kernel::cfloat3 origin, kernel::cfloat3 direction,
                            kernel::cfloat3 tangent_hint, float azimuth);

// Where `p` falls on the stamp, in [0,1] across a square of side `extent`
// centred on the frame's origin.
//
// The arithmetic is `alpha_at`'s, unchanged and deliberately so: an alpha
// sample and a directional brush's grain must agree about where a point is
// under the stamp, and the way to guarantee that is one expression rather than
// two that look alike.
struct StampUv {
    float u = 0.0f;
    float v = 0.0f;
};
StampUv stamp_uv(const StampFrame& frame, kernel::cfloat3 p, float extent);

}  // namespace mesh
}  // namespace clay
