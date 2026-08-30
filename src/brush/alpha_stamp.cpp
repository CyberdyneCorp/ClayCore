#include "clay/brush/alpha_stamp.h"

#include "clay/kernel/deform.h"

namespace clay {
namespace brush {

using kernel::cf3;
using kernel::cfloat3;

StampPlacement stamp_placement(cfloat3 point, cfloat3 normal, cfloat3 up, float roll) {
    StampPlacement out;
    out.centre = point;

    const float nlen = kernel::clength(normal);
    out.direction = nlen > 1e-9f ? normal * (1.0f / nlen) : cf3(0, 0, 1);
    const cfloat3 n = out.direction;

    // Project the requested up into the stamp's plane. Using it directly would
    // shear the stamp; the kernel re-orthogonalises anyway, and doing it here
    // too means the placement a host DRAWS matches the one that evaluates.
    cfloat3 t = up - n * kernel::cdot(up, n);
    float tlen = kernel::clength(t);
    if (tlen <= 1e-6f) {
        // The up was parallel to the normal. Cross with whichever axis the
        // normal leans on least: deterministic for a given normal, so a stamp
        // does not spin when the camera happens to line up with the surface.
        const cfloat3 axis = kernel::cabs(n.x) < 0.9f ? cf3(1, 0, 0) : cf3(0, 1, 0);
        t = kernel::ccross(n, axis);
        tlen = kernel::clength(t);
    }
    t = t * (1.0f / kernel::cmax(tlen, 1e-9f));

    if (roll != 0.0f) {
        // Rodrigues about the normal, which for a vector already perpendicular
        // to it is just the plane rotation.
        const cfloat3 b = kernel::ccross(n, t);
        t = t * kernel::ccos(roll) + b * kernel::csin(roll);
    }
    out.tangent = t;
    return out;
}

scene::Deformer stamp_deformer(const StampPlacement& placement, const float* samples, int width,
                               int height, float extent, float radius, float amplitude,
                               std::uint8_t ease) {
    return scene::Deformer::alpha(placement.centre, placement.direction, placement.tangent, samples,
                                  width, height, extent, radius, amplitude, ease);
}

StampPlacement placement_in(const StampPlacement& world, const math::Transform& item) {
    StampPlacement out;
    out.centre = item.apply_inverse(world.centre);
    // Directions, not points: the rotation's conjugate and no translation. The
    // scale divides out of a direction entirely, and re-normalising is what
    // keeps a non-uniform-looking float error from tilting the stamp plane.
    const math::Quat inv = item.rotation.conjugate();
    const kernel::cfloat3 d = inv.rotate(world.direction);
    const kernel::cfloat3 t = inv.rotate(world.tangent);
    const float dlen = kernel::clength(d);
    const float tlen = kernel::clength(t);
    out.direction = dlen > 1e-9f ? d * (1.0f / dlen) : cf3(0, 0, 1);
    out.tangent = tlen > 1e-9f ? t * (1.0f / tlen) : cf3(1, 0, 0);
    return out;
}

scene::Deformer stamp_deformer_in(const StampPlacement& world, const math::Transform& item,
                                  const float* samples, int width, int height, float extent,
                                  float radius, float amplitude, std::uint8_t ease) {
    // A scale of zero is not a frame. Refusing here would need an error
    // channel this file does not have, so the degenerate case falls back to 1
    // and leaves the placement where the caller put it — which is wrong in an
    // obvious way rather than a silent one.
    const float scale = item.scale > 1e-9f ? item.scale : 1.0f;
    const float inv_scale = 1.0f / scale;
    return stamp_deformer(placement_in(world, item), samples, width, height, extent * inv_scale,
                          radius * inv_scale, amplitude * inv_scale, ease);
}

}  // namespace brush
}  // namespace clay
