#pragma once

// Flattening a surface onto a plane (sdf-kernels spec, add-sdf-flatten).
//
// Voxel layers have had sculpt_flatten all along; SDF layers had nothing but
// the cut tool, which is global to its prism and has no falloff, so it cannot
// put a facet on one part of a shape and leave the rest.
//
// This is NOT ZBrush's Clip brush, deliberately. Clip moves vertices onto the
// plane instead of deleting them, and the solid that bounds is
// (S n H-) u proj(S n H+) — the second term lies ON the plane, so it has no
// volume. As a solid, Clip is exactly Trim, which the cut tool already does.
// Clip's distinctive look is the flattened polygons surviving as a
// zero-thickness fin, which a field cannot represent and which users remove
// anyway.
//
// The verb means what it means for voxels: material on the plane's + side is
// removed AND hollows on the - side are filled. Two-sided, not a subtract.
//
// WHY THIS SAMPLES RATHER THAN REWRITING SAMPLES. `relax` transforms a
// volume's stored samples in place, which is sound for it because smoothing
// moves the surface by less than a cell — always inside the band. Flatten moves
// the surface by MANY band widths, and a band cannot follow: once the surface
// has walked outside it there are no samples where the surface now is, and the
// isosurface breaks into fragments. Measured directly — eight strokes moved a
// surface 0.20 with a band of 0.05.
//
// So flatten does not edit a volume. It SAMPLES one, from a source field it
// blends with the plane as it goes, and the new band brackets the flattened
// surface by construction. That also makes the blend closed-form: no
// iterations, no step budget, no band to narrow afterwards.
//
//     flat(p) = (1 - w(p)) * source(p) + w(p) * plane(p)
//
// The source should be EXACT where possible — a document's tape rather than
// another volume — because a volume reports a bound rather than a distance
// outside its band, and sampling a field that mixes the two records the
// boundary between them as though it were part of the shape.
//
// Flatten BAKES, for the reason relax does.

#include <cstdint>
#include <functional>

#include "clay/field/volume.h"

namespace clay {
namespace field {

// Which side of the plane the operation acts on.
//
// TwoSided is ZBrush's Flatten: material above the plane goes AND hollows below
// it fill. CutOnly is the hard-surface family — hPolish, Planar, the Trim
// brushes — where cutting WITHOUT filling is the whole brush: it is what leaves
// a crisp facet against untouched surface, and filling the hollows beside a
// facet is precisely what a polish must not do. FillOnly is the dual, free once
// the clamp exists, and is what fills a scanned hole flat without touching the
// surface around it.
//
// The three differ by one clamp on the blend term, which is why this is a mode
// rather than three entry points.
enum class FlattenMode : std::uint8_t {
    TwoSided = 0,
    CutOnly = 1,
    FillOnly = 2,
};

struct FlattenSettings {
    // The plane to flatten onto: a point on it, and a normal whose positive
    // side is the material that goes. The caller supplies both — no camera and
    // no picking enters the engine, the same rule the cut tool follows.
    kernel::cfloat3 plane_point = kernel::cf3(0, 0, 0);
    kernel::cfloat3 plane_normal = kernel::cf3(0, 1, 0);

    // How far to go: 1 puts the surface on the plane, 0 changes nothing.
    float strength = 1.0f;

    // Where it acts. REQUIRED: flatten is local by nature, because where its
    // weight is one the result IS the plane. Blending with no region at full
    // strength does not flatten an object, it replaces it with a half-space —
    // a ball comes back as a box. A radius of zero is therefore refused, and
    // the source is sampled unflattened rather than destroyed.
    kernel::cfloat3 centre = kernel::cf3(0, 0, 0);
    float region_radius = 0.0f;
    // Over what distance the effect tapers to nothing at the region's edge. A
    // taper of zero would step, and a step is a rim on the surface.
    float falloff = 0.0f;

    // Defaults to the two-sided behaviour, so a caller that does not ask for a
    // mode gets exactly what it got before modes existed.
    FlattenMode mode = FlattenMode::TwoSided;
};

// Sample `source` with the flatten applied. The result declares the Lipschitz
// its samples actually have, measured rather than bounded in advance: a region
// blends under a weight that varies across it, and that can make the field
// steeper than the source was.
//
// A degenerate normal describes no plane, and yields a plain sampling of the
// source rather than a field shaped by an arbitrary direction.
FieldVolume flatten(const std::function<float(kernel::cfloat3)>& source,
                    const math::Aabb& region, float cell_size, float band,
                    const FlattenSettings& settings);

// The same, with a volume as the source — which is what an imported mesh
// gives, since there is no document behind it.
//
// Accurate while the surface stays near the band it came from: past that the
// source reports a bound rather than a distance, so the facet is placed against
// a lower bound on where the surface was rather than against the surface. Where
// a document exists, sample from that instead.
FieldVolume flatten(const FieldVolume& v, const FlattenSettings& settings);

}  // namespace field
}  // namespace clay
