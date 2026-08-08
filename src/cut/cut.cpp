// The cut tool (cut-tool spec). See include/clay/cut/cut.h for why the cut is
// a prism and why no camera enters the engine.

#include "clay/cut/cut.h"

#include <algorithm>
#include <cmath>

#include "clay/scene/curve.h"

namespace clay {
namespace cut {

using kernel::cf2;
using kernel::cf3;
using kernel::cfloat2;
using kernel::cfloat3;

namespace {

bool near_zero(float v, float tolerance) { return std::abs(v) <= tolerance; }

// Half-extent of the shape on the frame plane, which is what the sweep has to
// clear laterally and what "a shape with no area" is measured against.
cfloat2 shape_half_extent(const CutShape& shape) {
    switch (shape.type) {
        case CutShapeType::Circle:
            return cf2(shape.radius, shape.radius);
        case CutShapeType::Polygon: {
            if (shape.polygon.empty()) return cf2(0, 0);
            float lo_x = shape.polygon[0].x, hi_x = lo_x;
            float lo_y = shape.polygon[0].y, hi_y = lo_y;
            for (const cfloat2& v : shape.polygon) {
                lo_x = std::min(lo_x, v.x);
                hi_x = std::max(hi_x, v.x);
                lo_y = std::min(lo_y, v.y);
                hi_y = std::max(hi_y, v.y);
            }
            return cf2((hi_x - lo_x) * 0.5f, (hi_y - lo_y) * 0.5f);
        }
        case CutShapeType::Rect:
        default:
            return cf2(shape.half_width, shape.half_height);
    }
}

bool shape_has_area(const CutShape& shape) {
    if (shape.type == CutShapeType::Polygon && shape.polygon.size() < 3) return false;
    cfloat2 half = shape_half_extent(shape);
    return half.x > 0.0f && half.y > 0.0f;
}

// How far the region reaches along the sweep, measured from the frame origin.
// Every corner is projected because the region is an axis-aligned box and the
// sweep is not: taking only min and max corners would miss the extreme along a
// diagonal direction.
void region_span(const CutFrame& frame, const math::Aabb& region, float* out_near,
                 float* out_far) {
    float lo = 0.0f, hi = 0.0f;
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        cfloat3 corner = cf3(i & 1 ? region.max.x : region.min.x,
                             i & 2 ? region.max.y : region.min.y,
                             i & 4 ? region.max.z : region.min.z);
        float t = kernel::cdot(corner - frame.origin, frame.forward);
        lo = first ? t : std::min(lo, t);
        hi = first ? t : std::max(hi, t);
        first = false;
    }
    *out_near = -lo;  // how far BACK along forward the region reaches
    *out_far = hi;
}

}  // namespace

bool CutFrame::is_orthonormal(float tolerance) const {
    auto unit = [&](cfloat3 v) { return near_zero(kernel::clength(v) - 1.0f, tolerance); };
    if (!unit(right) || !unit(up) || !unit(forward)) return false;
    return near_zero(kernel::cdot(right, up), tolerance) &&
           near_zero(kernel::cdot(right, forward), tolerance) &&
           near_zero(kernel::cdot(up, forward), tolerance);
}

CutShape CutShape::rect(float half_width, float half_height) {
    CutShape s;
    s.type = CutShapeType::Rect;
    s.half_width = half_width;
    s.half_height = half_height;
    return s;
}

CutShape CutShape::circle(float radius) {
    CutShape s;
    s.type = CutShapeType::Circle;
    s.radius = radius;
    return s;
}

CutShape CutShape::from_polygon(std::vector<cfloat2> vertices) {
    CutShape s;
    s.type = CutShapeType::Polygon;
    s.polygon = std::move(vertices);
    return s;
}

CutShape CutShape::from_open_curve(const std::vector<scene::StrokePoint>& control_points,
                                   Side side, cfloat2 extent, float tolerance) {
    if (control_points.size() < 2) return from_polygon({});

    // OPEN, which is the whole difference from from_curve: a trim stroke's
    // endpoints must stay apart so the closing edge can run along the frame.
    std::vector<scene::StrokePoint> tess =
        scene::tessellate_curve(control_points, /*closed=*/false, tolerance);
    if (tess.size() < 2) return from_polygon({});

    std::vector<cfloat2> verts;
    verts.reserve(tess.size() + 2);
    for (const scene::StrokePoint& p : tess) verts.push_back(cf2(p.pos.x, p.pos.y));

    // Close it against the frame bound on the side being covered. The stroke is
    // assumed to span the region it divides, which is how a trim is drawn; one
    // that stops short leaves this edge visible, and that is the caller's to see.
    const cfloat2& first = verts.front();
    const cfloat2& last = verts.back();
    const float ex = kernel::cabs(extent.x), ey = kernel::cabs(extent.y);
    switch (side) {
        case Side::Below:
            verts.push_back(cf2(last.x, -ey));
            verts.push_back(cf2(first.x, -ey));
            break;
        case Side::Above:
            verts.push_back(cf2(last.x, ey));
            verts.push_back(cf2(first.x, ey));
            break;
        case Side::Left:
            verts.push_back(cf2(-ex, last.y));
            verts.push_back(cf2(-ex, first.y));
            break;
        case Side::Right:
            verts.push_back(cf2(ex, last.y));
            verts.push_back(cf2(ex, first.y));
            break;
    }
    return from_polygon(std::move(verts));
}

CutShape CutShape::from_curve(const std::vector<scene::StrokePoint>& control_points,
                              float tolerance) {
    // Flattened through the curve tessellator rather than a private routine:
    // a spline lasso then follows the same curve a spline item would, and the
    // tolerance means the same thing in both places.
    std::vector<scene::StrokePoint> tess =
        scene::tessellate_curve(control_points, /*closed=*/true, tolerance);
    std::vector<cfloat2> verts;
    verts.reserve(tess.size());
    for (const scene::StrokePoint& p : tess) verts.push_back(cf2(p.pos.x, p.pos.y));
    // The tessellator repeats the first point to close the ring; a profile
    // polygon closes implicitly, so the repeat would be a zero-length edge.
    if (verts.size() > 1) {
        const cfloat2& a = verts.front();
        const cfloat2& b = verts.back();
        if (near_zero(a.x - b.x, 1e-6f) && near_zero(a.y - b.y, 1e-6f)) verts.pop_back();
    }
    return from_polygon(std::move(verts));
}

std::optional<scene::Node> cut_item(const CutFrame& frame, const CutShape& shape,
                                    const math::Aabb& region, const CutOptions& options) {
    // Refused rather than repaired: a frame that is not orthonormal means the
    // shape the user saw was drawn in a frame they do not actually have, and
    // silently orthonormalizing would cut somewhere they did not draw.
    if (!frame.is_orthonormal()) return std::nullopt;
    if (!shape_has_area(shape)) return std::nullopt;

    float near_extent = 0.0f, far_extent = 0.0f;
    region_span(frame, region, &near_extent, &far_extent);
    if (options.near_extent) near_extent = *options.near_extent;
    if (options.far_extent) far_extent = *options.far_extent;
    // Padded past the region so a derived sweep leaves the solid rather than
    // stopping flush with it, where the cut face and the surface would
    // coincide. An explicit extent is a deliberate partial cut and is padded
    // too — the caller asked for a depth, not for a coincident face.
    const float pad = std::max(options.rounding, 1e-3f) + 1e-3f;
    near_extent += pad;
    far_extent += pad;

    const float half_depth = (near_extent + far_extent) * 0.5f;
    if (!(half_depth > 0.0f)) return std::nullopt;

    scene::Node node;
    node.prim = scene::Prim::extrude(half_depth);
    node.rounding = options.rounding;

    switch (shape.type) {
        case CutShapeType::Circle:
            node.profile = scene::Profile::circle(shape.radius);
            break;
        case CutShapeType::Polygon:
            node.profile = scene::Profile::polygon();
            node.profile_points = shape.polygon;
            break;
        case CutShapeType::Rect:
        default:
            node.profile = scene::Profile::box(shape.half_width, shape.half_height);
            break;
    }

    // An extrusion is authored in local space with its profile on XY and its
    // depth along Z, so the frame's basis is exactly the rotation that places
    // it: x -> right, y -> up, z -> forward.
    node.xform.rotation = math::Quat::from_basis(frame.right, frame.up, frame.forward);
    // Centred on the sweep, which is not the frame origin whenever the region
    // is lopsided about the plane the shape was drawn on.
    node.xform.position = frame.origin + frame.forward * ((far_extent - near_extent) * 0.5f);
    return node;
}

}  // namespace cut
}  // namespace clay
