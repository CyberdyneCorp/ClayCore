#pragma once

// Move Topological (sdf-kernels spec, add-move-topological).
//
// `grab` weights its falloff by |p - centre| / radius — Euclidean distance
// through SPACE. This weights it by distance along the MATERIAL, so pulling one
// finger does not drag the finger beside it.
//
// Measured on two fingers 0.32 apart, joined only through a palm below them: a
// Euclidean drag at radius 0.50 pulls the neighbouring finger's near edge from
// +0.158 to +0.074, while geodesically it is about 1.5 away — down one finger,
// across the palm and up the other. Euclidean distance cannot tell those apart,
// and that difference is the entire brush.
//
// It BAKES, for the reason relax and flatten do. The weight is a solved grid
// rather than a closed form, and putting one in the tape would mean a deformer
// that reads out-of-line data — which no deformer does, and which every backend
// would have to grow. Baking needs no kernel change.

#include <cstdint>
#include <functional>

#include "clay/field/volume.h"

namespace clay {
namespace field {

struct TopologicalMoveSettings {
    // A point on or near the surface being dragged — what a pick already gives.
    kernel::cfloat3 anchor = kernel::cf3(0, 0, 0);

    // The reach, in world units measured ALONG THE MATERIAL. This is the one
    // parameter that means something different from `grab`'s: a radius of 0.5
    // here reaches half a unit of travel across the surface, not half a unit of
    // straight line, so it cannot step over a gap however narrow.
    float radius = 0.3f;

    kernel::cfloat3 displacement = kernel::cf3(0, 0, 0);
    std::uint8_t ease = 0;
};

// Sample `source` with a topological move applied.
//
// The geodesic distance is solved on a local grid at `cell_size`, over the cells
// the source reports as material. Free space is not part of the graph, which is
// what stops the weight crossing a gap; the solved field is then dilated a few
// cells outward, because the warp acts on space near the surface and a point
// just outside it still needs a weight.
//
// The result declares the Lipschitz its samples actually have, measured rather
// than bounded in advance — a weight that varies along the surface can steepen
// the field, and by how much depends on the form.
//
// A zero displacement, a radius that is not positive, or an anchor with no
// material within reach all yield a plain sampling of the source: a drag that
// touches nothing is not an error.
FieldVolume move_topological(const std::function<float(kernel::cfloat3)>& source,
                             const math::Aabb& region, float cell_size, float band,
                             const TopologicalMoveSettings& settings);

// The same, with a volume as the source — which is what an imported mesh gives,
// since there is no document behind it. The geodesic solve only needs each cell's
// SIGN, which a narrow band reports correctly either side of the surface, so this
// is less lossy here than it is for flatten. The result is still sampled over the
// volume's own region.
FieldVolume move_topological(const FieldVolume& v, const TopologicalMoveSettings& settings);

}  // namespace field
}  // namespace clay
