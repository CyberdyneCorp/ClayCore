#pragma once

// Mask extrude (sdf-kernels and voxel-engine specs, add-mask-extrude).
//
// Mask a patch of a surface and pull it off as a solid of a chosen thickness.
// ZBrush calls it Extract, 3DCoat reaches it through Extrude from a frozen
// area, and it is how plates, panels, straps, pockets and shells get made — the
// thing a mask is FOR, once it can do more than freeze.
//
// NOT a new mechanism. `op_shell_union`'s operand is already the shell of a
// field, `FieldVolume` is already a narrow-band sampled field that rides in the
// tape's blob and evaluates the same on every backend, and `flatten` already
// established what a verb does when it cannot rewrite samples in place: sample a
// fresh volume and hand it back.
//
// WHAT WAS ACTUALLY MISSING is that a MaskField is a [0,1] scalar on a lattice
// and not a distance field. Composing one into a field expression directly puts
// a near-vertical step in the result, and the Lipschitz bound the evaluator
// depends on becomes a fiction. So `mask_to_field` measures the mask instead —
// distance to the boundary of the masked region — and after that the extrude is
// ordinary op composition.
//
// THE MASK IS THE REGION. Relax and flatten both need a `region_radius` because
// they have no other way to know where to act. This does not: the painted region
// bounds itself, which is why there is no region parameter here and why the
// volume it samples is smaller than either of theirs.
//
// It lives in `brush` rather than in `field` because it is the join of a mask
// and a field, and `field` is a LEAF: a sampled field sits below `scene`, while
// a mask sits above it. Putting this in `field` would have made
// field -> voxel -> scene -> field a cycle. `brush` already sits above voxel and
// scene, so the only new edge is brush -> field, and nothing in field knows
// about brush.

#include <cstdint>
#include <functional>
#include <optional>

#include "clay/field/volume.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace brush {

// Signed distance to the boundary of { mask >= threshold }, negative inside,
// as an ordinary sampled volume — 1-Lipschitz, tape-expressible, blob-carried.
//
// `pad` widens the sampled region past the masked one. An extrude reaches
// outside the mask by its own thickness, and a band clipped at the mask's
// border is no use to it.
//
// Zero `band` means three cells; zero `cell_size` means the mask's own.
//
// Nothing comes back for an empty mask: a volume built anyway would read as
// empty space everywhere, which is harder to notice than a failure.
std::optional<field::FieldVolume> mask_to_field(const voxel::MaskField& mask, float threshold = 0.5f,
                                         float band = 0.0f, float pad = 0.0f,
                                         float cell_size = 0.0f);

// Which side of the source surface the new material sits on.
enum class ExtrudeSide : std::uint8_t {
    Outward = 0,  // 0 <= d <= t: the plate sits ON the surface
    Inward = 1,   // -t <= d <= 0: the pocket
    Centred = 2,  // |d| <= t/2: straddles it, which is op_shell_union's operand
};

struct MaskExtrudeSettings {
    // Wall thickness in world units. Must be > 0.
    float thickness = 0.1f;
    ExtrudeSide side = ExtrudeSide::Outward;

    // What counts as masked.
    float threshold = 0.5f;

    // Rounding radius on the intersection with the masked region: the soft rim,
    // ZBrush's "S Smt". Zero gives a hard edge at the mask's border.
    float border_round = 0.0f;
    // Smoothing passes applied to a COPY of the mask before it is measured, for
    // a border that follows the paint less literally. The caller's mask is never
    // modified.
    int border_smooth = 0;

    // Sampling of the result. Zero cell_size means the mask's own; zero band
    // means three cells.
    float cell_size = 0.0f;
    float band = 0.0f;
};

// Extrude the masked patch of `source` into a new volume.
//
// `source` should be EXACT where possible — a document's tape rather than
// another volume — for the reason flatten's should: a volume reports a bound
// rather than a distance outside its band, and sampling a field that mixes the
// two records the boundary between them as though it were part of the shape.
//
// Returns nothing, rather than something that is not what was asked for, when
// the mask is empty, the thickness is not positive, the requested cell size is
// finer than the mask can describe, or the masked region never reaches the
// source's surface — that last one is the common mistake, and an empty volume
// would look like a bug in the caller's mask rather than in their aim.
//
// The mask is not modified.
std::optional<field::FieldVolume> mask_extrude(const std::function<float(kernel::cfloat3)>& source,
                                        const voxel::MaskField& mask,
                                        const MaskExtrudeSettings& settings);

// The same verb on a voxel grid, in CELL space rather than by sampling a field.
// A grid already knows which of its cells are on its surface, so going through a
// volume would cost a conversion and lose the palette for nothing.
//
// The two agree to within a voxel, which is the point: what a document means
// must not depend on which representation it is stored in.
//
// `cell_size` and `band` are ignored here — the grid's own resolution is the
// only one available. Neither the source nor the mask is modified.
std::optional<voxel::VoxelGrid> mask_extrude(const voxel::VoxelGrid& grid,
                                             const voxel::MaskField& mask,
                                             const MaskExtrudeSettings& settings);

}  // namespace brush
}  // namespace clay
