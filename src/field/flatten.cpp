// Flattening a surface onto a plane (sdf-kernels spec, add-sdf-flatten). See
// include/clay/field/flatten.h for why this is not the Clip brush and why it
// samples rather than rewriting a volume's samples.

#include "clay/field/flatten.h"

#include <algorithm>
#include <cmath>


namespace clay {
namespace field {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// How much of the plane to take at `p`. Outside the region this is zero and
// the source is left exactly as it was.
float region_weight(const FlattenSettings& s, cfloat3 p) {
    const float distance = kernel::clength(p - s.centre);
    if (distance <= s.region_radius) return 1.0f;
    // A hard edge would leave a rim where the flattened field meets the
    // untouched one, so the effect tapers.
    const float t = (distance - s.region_radius) / std::max(s.falloff, 1e-4f);
    if (t >= 1.0f) return 0.0f;
    return 1.0f - t * t * (3.0f - 2.0f * t);  // smoothstep
}

// The freeze: a fully masked sample takes weight zero, so the source's value
// comes back untouched and the surface there stays where the source put it
// rather than being drawn onto the plane.
float mask_gate(const MaskGate& mask, cfloat3 p) {
    if (!mask) return 1.0f;
    return 1.0f - std::clamp(mask(p), 0.0f, 1.0f);
}

}  // namespace

FieldVolume flatten(const std::function<float(cfloat3)>& source, const math::Aabb& region,
                    float cell_size, float band, const FlattenSettings& settings) {
    const float length = kernel::clength(settings.plane_normal);
    const float strength = std::clamp(settings.strength, 0.0f, 1.0f);

    // A zero normal describes no plane. Sampling the source unchanged beats
    // shaping the field by an arbitrary direction, which would look like it
    // worked.
    //
    // A missing REGION is refused for a less obvious reason. Flatten is local
    // by nature: where its weight is one the result IS the plane, so blending
    // with no region at full strength does not flatten an object, it REPLACES
    // it with a half-space — a ball comes back as a box. The interesting
    // behaviour all lives in the taper, so the region is what makes this a
    // brush rather than a very slow trim.
    if (!(length > 1e-6f) || strength <= 0.0f || !(settings.region_radius > 0.0f))
        return FieldVolume::sample(source, region, cell_size, band);

    const cfloat3 normal = settings.plane_normal * (1.0f / length);
    const cfloat3 point = settings.plane_point;

    FieldVolume out = FieldVolume::sample(
        [&source, &settings, normal, point, strength](cfloat3 p) {
            const float weight = region_weight(settings, p) * strength * mask_gate(settings.mask, p);
            const float here = source(p);
            if (weight <= 0.0f) return here;
            // The plane is itself a signed distance function, so blending
            // toward it is what makes this two-sided: above the plane the value
            // rises and material goes, below it the value falls and a hollow
            // fills. At full weight the field IS the half-space, so the surface
            // IS the plane.
            const float plane = kernel::cdot(normal, p - point);
            return here + (plane - here) * weight;
        },
        region, cell_size, band);

    // Measured, not bounded in advance. Inside the region the result is a
    // convex combination of two 1-Lipschitz fields and so is 1-Lipschitz; it is
    // the TAPER that can steepen it, by the movement times the taper's slope.
    // Reading back what the samples actually do beats guessing an envelope
    // generous enough for every region and falloff a caller might pick.
    out.set_sample_lipschitz(out.measure_sample_lipschitz());
    return out;
}

FieldVolume flatten(const FieldVolume& v, const FlattenSettings& settings) {
    if (v.empty()) return v;
    FieldVolume out = flatten([&v](cfloat3 p) { return v.eval(p); }, v.bounds(), v.cell_size(),
                              v.band(), settings);
    // A volume that was already steepened stays at least that steep: sampling
    // it cannot make its own source gentler than it was.
    out.set_sample_lipschitz(std::max(out.sample_lipschitz(), v.sample_lipschitz()));
    return out;
}

}  // namespace field
}  // namespace clay
