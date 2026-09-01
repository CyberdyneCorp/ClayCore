#include "clay/mesh/detail_stamp.h"

#include <cmath>

#include "clay/kernel/deform.h"  // calpha_sample, calpha_frame

namespace clay {
namespace mesh {
namespace {

// One channel of the planar image at (u, v). The plane offset is the only thing
// this adds over the scalar alpha's read, and it is deliberately the whole
// difference — the interpolation, the clamp and the addressing are
// `calpha_sample`'s.
float sample_plane(const DetailStampSettings& s, int channel, float u, float v) {
    const std::size_t plane = static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height);
    return kernel::calpha_sample(s.image + plane * static_cast<std::size_t>(channel), s.width,
                                 s.height, u, v);
}

}  // namespace

AlphaFrame detail_stamp_frame(const DetailStampSettings& settings,
                              kernel::cfloat3 fallback_direction) {
    AlphaFrame frame;
    if (!settings.valid()) return frame;
    frame.centre = settings.center;
    frame.extent = settings.extent;
    kernel::cfloat3 dir = settings.direction;
    if (kernel::clength(dir) < 1e-9f) dir = fallback_direction;
    kernel::cfloat3 n, t, b;
    kernel::calpha_frame(dir, settings.tangent, &n, &t, &b);
    frame.tangent = t;
    frame.binormal = b;
    return frame;
}

DetailStampSample detail_stamp_sample(const DetailStampSettings& settings, const AlphaFrame& frame,
                                      const SurfaceFrame& vertex_frame,
                                      kernel::cfloat3 position) {
    DetailStampSample out;
    if (!settings.valid()) return out;

    const kernel::cfloat3 rel = position - frame.centre;
    const float u = kernel::cdot(rel, frame.tangent) / frame.extent + 0.5f;
    const float v = kernel::cdot(rel, frame.binormal) / frame.extent + 0.5f;
    // OUTSIDE THE SQUARE IS NOTHING, not the clamped border. See
    // `DetailStampSample::inside`: a clamped border is right for a scalar alpha
    // and smears a map's edge row for a displacement.
    out.inside = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
    if (!out.inside) return out;

    if (settings.mode == DetailStampMode::Weight) {
        out.weight = sample_plane(settings, 0, u, v);
        return out;
    }
    if (settings.mode == DetailStampMode::Height) {
        // ALONG THE VERTEX'S OWN NORMAL, which in frame coefficients is the
        // third component and nothing else. The frame is orthonormal, so this
        // is exactly `amplitude * (h - bias)` of motion along the surface
        // normal with no tangential drift to accumulate over a stroke.
        out.offset.normal = settings.amplitude * (sample_plane(settings, 0, u, v) - settings.bias);
        return out;
    }

    // VECTOR. The three channels are read as tangent, bitangent and normal
    // components OF THE VERTEX'S FRAME. Nothing is rotated into world space and
    // back: the coefficients this produces are the same quantity the hierarchy
    // already stores, in the same frame, so a stamp and a stroke deposit into
    // one representation rather than two that must agree.
    (void)vertex_frame;
    out.offset.tangent = settings.amplitude * (sample_plane(settings, 0, u, v) - settings.bias);
    out.offset.bitangent = settings.amplitude * (sample_plane(settings, 1, u, v) - settings.bias);
    out.offset.normal = settings.amplitude * (sample_plane(settings, 2, u, v) - settings.bias);
    return out;
}

DetailStampReport detail_stamp_report(const DetailStampSettings& settings, float vertex_spacing) {
    DetailStampReport report;
    if (!settings.valid() || vertex_spacing <= 0.0f) return report;
    const int samples = settings.width > settings.height ? settings.width : settings.height;
    report.sample_size = settings.extent / static_cast<float>(samples);
    report.vertex_spacing = vertex_spacing;
    report.oversampling = vertex_spacing / report.sample_size;
    // ONE SAMPLE PER VERTEX is the Nyquist-free statement of the obvious: a
    // level cannot hold a feature narrower than the gap between two of its
    // vertices. Reported as a ratio rather than a boolean alone, because a host
    // showing "this map is 4x finer than this level" can offer the one fix that
    // works — subdivide — while a bare warning cannot.
    report.under_resolved = report.oversampling > 1.0f;
    return report;
}

}  // namespace mesh
}  // namespace clay
