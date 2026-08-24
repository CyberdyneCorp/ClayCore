#include "clay/brush/procedural_mask.h"

#include <algorithm>
#include <cmath>

#include "clay/kernel/field.h"

namespace clay {
namespace brush {

using kernel::cfloat3;
using voxel::MaskField;
using voxel::VoxelCoord;

namespace {

// The Laplacian of a distance field, by the six-point stencil.
//
// For f = |p| - R, a sphere of radius R, this is 2/R at the surface — POSITIVE
// for a convex surface, and the magnitude is the curvature. That unambiguous
// sign is why cavity and convexity are one subtraction apart here and an
// error-prone vertex-ring estimate in a mesh engine.
float laplacian(const std::function<float(cfloat3)>& f, cfloat3 p, float h) {
    const float c = f(p);
    const float sum = f(kernel::cf3(p.x + h, p.y, p.z)) + f(kernel::cf3(p.x - h, p.y, p.z)) +
                      f(kernel::cf3(p.x, p.y + h, p.z)) + f(kernel::cf3(p.x, p.y - h, p.z)) +
                      f(kernel::cf3(p.x, p.y, p.z + h)) + f(kernel::cf3(p.x, p.y, p.z - h));
    return (sum - 6.0f * c) / (h * h);
}

float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

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
    // The stencil step. A cell is the natural scale: smaller measures noise the
    // lattice cannot represent, larger blurs the feature being measured.
    const float h = cell;
    const float scale = settings.scale > 0.0f ? settings.scale : cell;

    MaskField out(cell);
    const VoxelCoord lo = out.cell_at(region.min);
    const VoxelCoord hi = out.cell_at(region.max);

    const cfloat3 dir = kernel::clength(settings.direction) > 0.0f
                            ? kernel::cnormalize(settings.direction)
                            : kernel::cf3(0.0f, 1.0f, 0.0f);

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

                float value = 0.0f;
                if (measure == SurfaceMeasure::NormalDirection) {
                    const cfloat3 n = kernel::cnormal(source, p, h);
                    const float agreement = kernel::cdot(n, dir);
                    if (agreement <= settings.threshold) continue;
                    // Remap [threshold, 1] onto [0, 1], so raising the
                    // threshold narrows the cone rather than dimming the whole
                    // mask — which is what a caller means by a threshold.
                    const float span = 1.0f - settings.threshold;
                    value = span > 0.0f ? (agreement - settings.threshold) / span : 1.0f;
                } else {
                    const float lap = laplacian(source, p, h);
                    // scale is the RADIUS that reads as fully masked, and
                    // curvature is 1/radius, so the product is the fraction.
                    const float k = lap * scale;
                    switch (measure) {
                        case SurfaceMeasure::Curvature: value = std::fabs(k); break;
                        case SurfaceMeasure::Cavity: value = -k; break;      // concave
                        case SurfaceMeasure::Convexity: value = k; break;    // convex
                        case SurfaceMeasure::NormalDirection: break;         // handled above
                    }
                }
                value = saturate(value);
                if (value > 0.0f) out.set(c, value);
            }
    }
    return out;
}

}  // namespace brush
}  // namespace clay
