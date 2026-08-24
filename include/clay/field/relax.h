#pragma once

// Relaxing a sampled field (sdf-kernels spec, add-sdf-relax).
//
// The last ZBrush core brush: voxels smooth, SDF layers had nothing. Smoothing
// happens by SAMPLING the field, averaging, and re-sampling — the field-space
// re-blend alternative would have made an edit list mean "shapes plus a rule
// about how they interact", which reaches undo, picking, serialization and the
// C ABI, and still could not smooth a bump in the middle of a single item.
//
// Convolving a distance field destroys EXACTNESS: the result no longer reports
// the true distance to its own surface. It does NOT break the Lipschitz bound,
// and that is the property the evaluator actually depends on. Averaging can
// only shrink the bound — for weights summing to one,
//
//     |f(x) - f(y)| <= sum_i w_i |f(x + d_i) - f(y + d_i)| <= |x - y|
//
// and a 1-Lipschitz field is automatically a conservative bound on the distance
// to its own zero set: if z is the nearest zero to x then
// |f(x)| = |f(x) - f(z)| <= |x - z|. So sphere tracing on a relaxed field
// cannot overstep, which is what makes this small rather than large.
//
// Relax BAKES. What comes back is a volume, not the edit list that went in.
// That is inherent to relaxing a field, not a shortcut taken here.

#include <functional>

#include "clay/field/volume.h"
#include "clay/parallel/cancel.h"

namespace clay {
namespace field {

// An optional freeze, taken as a CALLABLE rather than as a mask type. A sampled
// field is a leaf — it sits below `scene`, while a mask sits above it — so
// naming voxel::MaskField here would make field -> voxel -> scene -> field a
// cycle. What a verb actually needs from a mask is a scalar at a world point,
// and that is what this is: 0 lets an edit through, 1 freezes it. Empty means
// no mask, which costs nothing.
using MaskGate = std::function<float(kernel::cfloat3)>;

struct RelaxSettings {
    // How much of the smoothed value to take, per iteration. 1 replaces the
    // field with the average; 0 changes nothing.
    float strength = 1.0f;

    // The averaging radius, in cells. Larger smooths coarser features. The
    // taps are the cell-aligned neighbourhood within this radius.
    int radius_cells = 1;

    // More passes of a small kernel is not the same shape as one pass of a
    // large one: a small kernel repeated approaches a Gaussian, a large one
    // stays boxy.
    int iterations = 1;

    // Where it acts. A radius of zero means everywhere, which is a filter
    // rather than a brush.
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    float region_radius = 0.0f;
    // Over what distance the effect tapers to nothing at the region's edge.
    // Zero would leave a visible rim; it is clamped to something that will not.
    float falloff = 0.0f;

    // Optional freeze, exactly as the voxel verbs take one: the weight at a
    // sample is scaled by (1 - mask) at that sample's WORLD position, so a
    // fully masked sample keeps its value verbatim. Sampling in world units
    // rather than in this volume's cells costs nothing and is the whole reason
    // the mask lattice is addressed that way — one mask can freeze a voxel
    // layer and an SDF layer at once.
    MaskGate mask;
};

// Smooth `v`, returning a new volume sampled over the same region at the same
// resolution. The input is not modified.
FieldVolume relax(const FieldVolume& v, const RelaxSettings& settings = {},
                  parallel::CancelToken* token = nullptr);

// The same, sampled from an arbitrary SOURCE first — a document's tape is the
// intended one — mirroring flatten's pair of overloads. Deliberately exactly
// sample-then-relax: relax averages cell-aligned taps, and the taps of a
// fresh bake ARE the source at those lattice points, so there is nothing a
// fused form could do better. What the overload buys is one entry point for
// hosts, and a source that is exact rather than a volume carrying a band.
FieldVolume relax(const std::function<float(kernel::cfloat3)>& source,
                  const math::Aabb& region, float cell_size, float band,
                  const RelaxSettings& settings = {});

}  // namespace field
}  // namespace clay
