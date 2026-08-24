#include "clay/brush/procedural_mask.h"

#include "clay/brush/surface_measure.h"

#include <algorithm>
#include <cmath>

#include "clay/kernel/field.h"

namespace clay {
namespace brush {

using kernel::cfloat3;
using voxel::MaskField;
using voxel::VoxelCoord;

MaskField mask_from_surface(const std::function<float(cfloat3)>& source, SurfaceMeasure measure,
                            const ProceduralMaskSettings& settings, parallel::CancelToken* token,
                            bool* out_cancelled) {
    if (out_cancelled) *out_cancelled = false;

    const math::Aabb& region = settings.region;
    if (!source || region.empty() || region.is_infinite()) return MaskField(0.05f);

    float cell = settings.cell_size;
    if (!(cell > 0.0f)) {
        // A guess, and it is documented as one: a hundredth of the region's
        // longest side, which puts a few thousand cells across it.
        const cfloat3 span = region.max - region.min;
        cell = std::max({span.x, span.y, span.z}) * 0.01f;
        if (!(cell > 0.0f)) return MaskField(0.05f);
    }
    const float band = settings.band > 0.0f ? settings.band : cell * 2.0f;

    // The shared measure settings, with the lattice's own defaults filled in
    // where the caller left them at zero. A CELL is the natural stencil step
    // here: smaller measures noise the lattice cannot represent, larger blurs
    // the feature being measured. The per-point form has no lattice and
    // derives its step from `scale` instead, which is the one place the two
    // legitimately differ — and it is a default, not a formula.
    MeasureSettings m = settings.measure;
    if (!(m.h > 0.0f)) m.h = cell;
    if (!(m.scale > 0.0f)) m.scale = cell;

    MaskField out(cell);
    const VoxelCoord lo = out.cell_at(region.min);
    const VoxelCoord hi = out.cell_at(region.max);

    parallel::ProgressScope progress(token, 1);
    const std::int64_t planes = static_cast<std::int64_t>(hi.z) - lo.z + 1;
    std::int64_t done = 0;

    for (std::int32_t z = lo.z; z <= hi.z; ++z, ++done) {
        // The checkpoint is the outer plane: one relaxed load per z-slice
        // rather than one per cell, which is the same granularity the voxel
        // extrude uses.
        if (parallel::cancelled(token)) {
            if (out_cancelled) *out_cancelled = true;
            return MaskField(cell);  // empty; the caller keeps nothing partial
        }
        progress.advance(static_cast<std::uint64_t>(done),
                         planes > 0 ? static_cast<float>(done) / static_cast<float>(planes) : 1.0f);

        for (std::int32_t y = lo.y; y <= hi.y; ++y)
            for (std::int32_t x = lo.x; x <= hi.x; ++x) {
                const VoxelCoord c{x, y, z};
                const cfloat3 p = out.cell_centre(c);
                const float d = source(p);
                // Outside the band is left at zero. A mask is about the
                // SURFACE, and a measure taken deep inside a solid describes
                // nothing an artist can see.
                if (std::fabs(d) > band) continue;

                // ONE implementation of the measure, shared with the
                // per-point form. A second stencil here would be a second
                // chance for a mask and a baked map to disagree about the
                // same surface.
                const float value = measure_at(source, measure, p, m);
                if (value > 0.0f) out.set(c, value);
            }
    }
    return out;
}

}  // namespace brush
}  // namespace clay
