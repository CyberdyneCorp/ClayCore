#include "clay/mesh/deform.h"

#include <cmath>

#include "clay/kernel/deform.h"

namespace clay {
namespace mesh {

namespace {

using kernel::cf3;
using kernel::cfloat3;

// An orthonormal frame whose LOCAL Y is the deform axis, so the kernel's maps —
// which are all about Y — apply unchanged once a point is rotated into it.
struct Frame {
    cfloat3 axis;      // local +Y, unit
    cfloat3 tangent;   // local +X, unit
    cfloat3 binormal;  // local +Z, unit
    cfloat3 origin;

    cfloat3 to_local(cfloat3 p) const {
        const cfloat3 v = p - origin;
        return cf3(kernel::cdot(v, tangent), kernel::cdot(v, axis), kernel::cdot(v, binormal));
    }
    cfloat3 to_world(cfloat3 l) const {
        return origin + tangent * l.x + axis * l.y + binormal * l.z;
    }
};

Frame frame_of(const MeshDeformSettings& s) {
    Frame f;
    f.origin = s.origin;
    const float len = kernel::clength(s.axis);
    f.axis = len > 1e-20f ? s.axis / len : cf3(0, 1, 0);
    // WHICH tangent does not matter; the HANDEDNESS does, and only for twist.
    // A taper is rotationally symmetric about the axis, so any basis gives the
    // same answer — which is exactly why a left-handed frame passed the taper
    // test and reversed the twist. (tangent, axis, binormal) is built as a
    // right-handed (X, Y, Z) so that a positive angle turns the same way here
    // as it does in the kernel's own map.
    //
    // The seed is the more distant world axis, which keeps the cross product
    // well conditioned when the deform axis is near one of them.
    const cfloat3 seed =
        std::fabs(f.axis.y) < 0.9f ? cf3(0, 1, 0) : cf3(1, 0, 0);
    cfloat3 t = kernel::ccross(seed, f.axis);
    const float tl = kernel::clength(t);
    f.tangent = tl > 1e-20f ? t / tl : cf3(1, 0, 0);
    f.binormal = kernel::ccross(f.tangent, f.axis);  // X x Y = Z
    return f;
}

}  // namespace

bool MeshDeformSettings::is_identity() const {
    if (!(span > 0.0f)) return true;  // no span is nothing to ramp across
    if (verb == MeshDeform::Taper) return scale_start == 1.0f && scale_end == 1.0f;
    return angle == 0.0f;
}

cfloat3 deform_point(const MeshDeformSettings& s, cfloat3 p) {
    if (s.is_identity()) return p;
    const Frame f = frame_of(s);
    const cfloat3 l = f.to_local(p);

    cfloat3 warped;
    if (s.verb == MeshDeform::Taper) {
        // The FORWARD taper: multiply the cross-section, where ctaper_point —
        // an inverse map — divides it. Written out rather than reusing the
        // kernel with reciprocal scales, because the ease interpolates BETWEEN
        // the two scales and the mix of two reciprocals is not the reciprocal
        // of their mix.
        const float t =
            kernel::cease(s.ease, kernel::cclamp(l.y / s.span, 0.0f, 1.0f));
        const float scale = kernel::cmix(s.scale_start, s.scale_end, t);
        warped = cf3(l.x * scale, l.y, l.z * scale);
    } else {
        // The FORWARD twist is the kernel's own map with the angle negated,
        // which is exact because a twist does not move the coordinate its
        // angle is measured from. `ctwist_range_point` takes radians PER UNIT
        // and multiplies by the span, so the total angle divides back out.
        const float per_unit = -s.angle / s.span;
        warped = kernel::ctwist_range_point(l, per_unit, 0.0f, s.span, s.ease);
    }
    return f.to_world(warped);
}

}  // namespace mesh
}  // namespace clay
