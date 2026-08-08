// The Tube tool — see include/clay/brush/tube.h for why this is a resolver over
// machinery that already existed rather than a new primitive.

#include "clay/brush/tube.h"

#include <algorithm>
#include <cmath>

#include "clay/scene/curve.h"

namespace clay {
namespace brush {

namespace {

using kernel::cfloat3;

// The control points, as the curve tessellator takes them. Radius is filled in
// afterwards, once the tessellated arc length is known.
std::vector<scene::StrokePoint> control_of(const std::vector<cfloat3>& path,
                                           scene::StrokePointType type) {
    std::vector<scene::StrokePoint> control;
    control.reserve(path.size());
    for (cfloat3 p : path) {
        scene::StrokePoint sp;
        sp.pos = p;
        sp.type = type;
        control.push_back(sp);
    }
    return control;
}

// Nomad's three radius handles: start, middle, end. Two linear pieces rather
// than one curve through three points, because a caller setting all three equal
// means a uniform tube and a quadratic through them would bulge.
float radius_at(const TubeSettings& s, float t) {
    return t <= 0.5f ? s.radius_start + (s.radius_mid - s.radius_start) * (t * 2.0f)
                     : s.radius_mid + (s.radius_end - s.radius_mid) * ((t - 0.5f) * 2.0f);
}

// Distribute the radius by ARC LENGTH along the tessellated path, not by point
// index: a path whose control points bunch at one end would otherwise bunch the
// taper there, so the same drawn shape would taper differently depending on how
// evenly it happened to be tapped.
void apply_radius(std::vector<scene::StrokePoint>& points, const TubeSettings& s) {
    if (points.empty()) return;
    std::vector<float> along(points.size(), 0.0f);
    float total = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        total += kernel::clength(points[i].pos - points[i - 1].pos);
        along[i] = total;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float t = total > 1e-6f ? along[i] / total : 0.0f;
        points[i].radius = radius_at(s, t);
    }
}

bool any_positive(const TubeSettings& s) {
    return s.radius_start > 0.0f || s.radius_mid > 0.0f || s.radius_end > 0.0f;
}

}  // namespace

std::optional<scene::Node> tube(const std::vector<cfloat3>& path,
                                const TubeSettings& settings) {
    if (path.size() < 2 || !any_positive(settings)) return std::nullopt;

    // Tessellated HERE rather than left to compile time, because the radius has
    // to be distributed over the points the field will actually see. A hard
    // chain tessellates to itself, so this is the identity for a sharp tube.
    std::vector<scene::StrokePoint> points =
        scene::tessellate_curve(control_of(path, settings.point_type), settings.closed,
                                settings.tolerance);
    if (points.size() < 2) return std::nullopt;
    apply_radius(points, settings);

    scene::Node node;
    node.prim = scene::Prim::stroke();
    node.stroke = std::move(points);
    // Already tessellated, so the compiler must not tessellate again: hard
    // points pass through the curve step unchanged.
    for (scene::StrokePoint& p : node.stroke) p.type = scene::StrokePointType::Hard;
    node.stroke_closed = settings.closed;
    node.stroke_blend_k = settings.blend_k;
    node.curve_tolerance = settings.tolerance;
    return node;
}

std::optional<scene::Node> tube_with_profile(const std::vector<cfloat3>& path,
                                             const std::vector<scene::Profile>& profiles,
                                             const TubeSettings& settings) {
    if (path.size() < 2 || profiles.empty()) return std::nullopt;

    scene::Node node;
    node.prim = scene::Prim::swept();
    // The guide keeps its control points and its type: a sweep tessellates its
    // own guide at compile time, and the profiles are distributed along the
    // result by arc length there. Tessellating here would only do it twice.
    node.stroke = control_of(path, settings.point_type);
    node.stroke_closed = settings.closed;
    node.curve_tolerance = settings.tolerance;
    // A sweep interpolates between profiles, so one profile is a constant
    // cross-section and needs a second copy of itself to interpolate against.
    node.profiles = profiles;
    if (node.profiles.size() == 1) node.profiles.push_back(node.profiles.front());
    node.profile_polygons.resize(node.profiles.size());
    return node;
}

}  // namespace brush
}  // namespace clay
