// A voxel sculpt going back into the document as an operand (#90).
//
// The bridge ran one way. SDF to voxel is direct — rasterize_tape — and voxel
// back was a DETOUR: mesh the grid, run mesh::to_field over the triangles,
// place the result. That resamples twice, builds a BVH to do it, drops the
// palette, and hands back something that is no longer being sculpted.
//
// Direct instead. The grid already knows where its surface is; what it does
// not have is a DISTANCE to it. FieldVolume::sample takes a callable, and
// field::redistance rewrites stored samples as the distance to their own zero
// set — so the conversion is: say where the surface is, then measure to it.
//
// WHERE THE SURFACE IS. Occupancy read through trilinear interpolation between
// cell CENTRES rather than as cells. That is what makes the isosurface a
// smooth surface rather than a staircase, and it is the same choice the smooth
// mesher makes (#108): the value between an occupied and an empty cell varies
// continuously, so the 0.5 crossing lands somewhere real instead of always on
// a cell boundary. Nothing is filtered by default, so nothing can vanish.

#include <algorithm>
#include <cmath>
#include <optional>

#include "clay/field/redistance.h"
#include "clay/voxel/grid.h"

#include "occupancy_box.h"

namespace clay {
namespace voxel {

using kernel::cf3;
using kernel::cfloat3;

std::optional<field::FieldVolume> VoxelGrid::to_field(std::size_t level,
                                                      FieldOptions options) const {
    if (level >= levels_.size()) return std::nullopt;
    const float vs = levels_[level].voxel_size;
    if (!(vs > 0.0f)) return std::nullopt;

    const int blur = std::clamp(options.blur, 0, 8);
    const float band = options.band > 0.0f ? options.band : 3.0f * vs;
    // Enough apron for the interpolation, the band the volume keeps, and one
    // cell per blur pass, so a sample near the edge reads the same zeros a
    // sample well outside would.
    const int pad = 2 + blur + static_cast<int>(std::ceil(band / vs));

    detail::OccupancyBox box;
    if (!detail::build_occupancy(*this, level, pad, options.index, &box)) return std::nullopt;
    for (int i = 0; i < blur; ++i) detail::blur_occupancy(box);

    // Negative inside. Scaled by the cell size so the input already varies at
    // roughly the right rate — redistance replaces the magnitudes with true
    // distances regardless, but a sane input keeps its sign decisions sane.
    auto signed_at = [&](cfloat3 p) {
        const float occ = box.trilinear(p.x / vs - 0.5f, p.y / vs - 0.5f, p.z / vs - 0.5f);
        return (0.5f - occ) * vs;
    };

    const math::Aabb region{
        cf3((static_cast<float>(box.min[0]) + 0.5f) * vs, (static_cast<float>(box.min[1]) + 0.5f) * vs,
            (static_cast<float>(box.min[2]) + 0.5f) * vs),
        cf3((static_cast<float>(box.min[0] + box.dim[0]) - 0.5f) * vs,
            (static_cast<float>(box.min[1] + box.dim[1]) - 0.5f) * vs,
            (static_cast<float>(box.min[2] + box.dim[2]) - 0.5f) * vs)};

    field::FieldVolume volume = field::FieldVolume::sample(signed_at, region, vs, band);
    if (volume.sample_count() == 0) return std::nullopt;

    // The palette, per sample, so the whole sculpt converts to ONE volume
    // instead of one per entry. Colour is read from the NEAREST cell rather
    // than interpolated: the palette is a set of authored entries and a blend
    // between two of them is a colour nobody chose. The interpolation that
    // makes a boundary gradate happens later, in the field's own eval_color,
    // between samples that each carry an entry.
    //
    // A converted-index run (options.index != 0) is one colour by definition
    // and carries it on the item instead, so there is nothing to store.
    if (options.index == 0) {
        volume.fill_colors([&](cfloat3 p) {
            const VoxelCoord c{static_cast<std::int32_t>(std::floor(p.x / vs)),
                               static_cast<std::int32_t>(std::floor(p.y / vs)),
                               static_cast<std::int32_t>(std::floor(p.z / vs))};
            std::uint8_t idx = cell_at(level, c);
            if (idx == 0) {
                // Just outside the surface: take the nearest occupied
                // neighbour's colour rather than a default, or every silhouette
                // would carry a colour the sculpt does not contain.
                for (int dz = -1; dz <= 1 && idx == 0; ++dz)
                    for (int dy = -1; dy <= 1 && idx == 0; ++dy)
                        for (int dx = -1; dx <= 1 && idx == 0; ++dx)
                            idx = cell_at(level, {c.x + dx, c.y + dy, c.z + dz});
            }
            return idx != 0 ? palette_color(idx) : cf3(0.7f, 0.7f, 0.7f);
        });
    }

    // The half that makes the result an OPERAND rather than a picture. Without
    // it the stored values are an occupancy ramp, which crosses zero in the
    // right place and says nothing truthful about distance — so every marcher
    // and every blend downstream would be working from a field whose Lipschitz
    // is a guess. redistance measures to the zero set the interpolation just
    // defined and re-declares the bound from what it actually finds.
    field::redistance(volume);
    return volume;
}

}  // namespace voxel
}  // namespace clay
