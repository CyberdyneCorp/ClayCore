#include "clay/mesh/stamp_frame.h"

#include <cmath>

#include "clay/kernel/deform.h"  // calpha_frame

namespace clay {
namespace mesh {

StampFrame make_stamp_frame(kernel::cfloat3 origin, kernel::cfloat3 direction,
                            kernel::cfloat3 tangent_hint, float azimuth) {
    StampFrame frame;
    frame.origin = origin;
    frame.rotation = azimuth;

    kernel::cfloat3 n, t, b;
    kernel::calpha_frame(direction, tangent_hint, &n, &t, &b);
    frame.normal = n;

    // THE ZERO-AZIMUTH BRANCH, AND WHY IT IS NOT A MICRO-OPTIMISATION.
    //
    // The default azimuth is zero and the overwhelming majority of stamps pass
    // zero, so the "obviously equivalent" unconditional `t*cos 0 + b*sin 0`
    // would run on nearly every stamp in the library. It is not equivalent.
    // `1.0f * x == x` and `0.0f * y == 0` both hold, but `x + 0.0f` is NOT the
    // identity when `x` is `-0.0f`: it yields `+0.0f`. One flipped sign bit
    // moves `stamp_uv`'s dot product, which can land a bilinear alpha sample on
    // the other side of a texel boundary — and that moves a golden.
    //
    // This is the same class of argument `compose_weight` already makes about
    // multiplying an identical 1.0 in LAST rather than earlier, taken one step
    // further because addition is involved rather than multiplication. The
    // acceptance gate for this change forbids a moved golden outright, so the
    // branch is the contract and not a preference.
    if (azimuth == 0.0f) {
        frame.tangent = t;
        frame.bitangent = b;
        return frame;
    }

    const float c = std::cos(azimuth);
    const float s = std::sin(azimuth);
    // Rotated IN THE PLANE the two axes already span, so the result is still
    // orthonormal and still right-handed with `normal` without a second
    // re-orthogonalisation to disagree with the first.
    frame.tangent = t * c + b * s;
    frame.bitangent = b * c - t * s;
    return frame;
}

StampUv stamp_uv(const StampFrame& frame, kernel::cfloat3 p, float extent) {
    StampUv uv;
    if (extent <= 1e-9f) return uv;
    const kernel::cfloat3 rel = p - frame.origin;
    uv.u = kernel::cdot(rel, frame.tangent) / extent + 0.5f;
    uv.v = kernel::cdot(rel, frame.bitangent) / extent + 0.5f;
    return uv;
}

}  // namespace mesh
}  // namespace clay
