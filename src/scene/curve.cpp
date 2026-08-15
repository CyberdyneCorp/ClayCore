// Curve tessellation (scene-model spec). See include/clay/scene/curve.h for
// why this lowers into the stroke opcode instead of adding one.

#include "clay/scene/curve.h"

#include <algorithm>
#include <cmath>

namespace clay {
namespace scene {

using kernel::cf3;
using kernel::cfloat3;

namespace {

// The four control points a cubic span needs, clamping at the ends of an open
// curve and wrapping on a closed one. Clamping rather than extrapolating keeps
// an open curve's first and last spans inside the region the artist can see.
struct Span {
    cfloat3 p0, p1, p2, p3;  // p1 -> p2 is the span; p0 and p3 shape it
    float r1, r2;
};

std::size_t wrap(std::ptrdiff_t i, std::size_t n, bool closed) {
    if (closed) {
        std::ptrdiff_t m = static_cast<std::ptrdiff_t>(n);
        return static_cast<std::size_t>(((i % m) + m) % m);
    }
    return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
        i, 0, static_cast<std::ptrdiff_t>(n) - 1));
}

Span span_at(const std::vector<StrokePoint>& pts, std::size_t i, bool closed) {
    const std::size_t n = pts.size();
    auto at = [&](std::ptrdiff_t k) { return pts[wrap(k, n, closed)]; };
    std::ptrdiff_t si = static_cast<std::ptrdiff_t>(i);
    Span s;
    s.p0 = at(si - 1).pos;
    s.p1 = at(si).pos;
    s.p2 = at(si + 1).pos;
    s.p3 = at(si + 2).pos;
    s.r1 = at(si).radius;
    s.r2 = at(si + 1).radius;
    return s;
}

cfloat3 catmull_rom(const Span& s, float t) {
    float t2 = t * t, t3 = t2 * t;
    return (s.p1 * 2.0f + (s.p2 - s.p0) * t +
            (s.p0 * 2.0f - s.p1 * 5.0f + s.p2 * 4.0f - s.p3) * t2 +
            (s.p1 * 3.0f - s.p0 - s.p2 * 3.0f + s.p3) * t3) *
           0.5f;
}

cfloat3 bspline(const Span& s, float t) {
    float t2 = t * t, t3 = t2 * t;
    float b0 = (1.0f - 3.0f * t + 3.0f * t2 - t3) / 6.0f;
    float b1 = (4.0f - 6.0f * t2 + 3.0f * t3) / 6.0f;
    float b2 = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) / 6.0f;
    float b3 = t3 / 6.0f;
    return s.p0 * b0 + s.p1 * b1 + s.p2 * b2 + s.p3 * b3;
}

cfloat3 bezier(cfloat3 a, cfloat3 ca, cfloat3 cb, cfloat3 b, float t) {
    float u = 1.0f - t;
    return a * (u * u * u) + ca * (3.0f * u * u * t) + cb * (3.0f * u * t * t) + b * (t * t * t);
}

// The point at parameter t along the span starting at `i`. The span's shape is
// decided by the type of the point it STARTS at: a type says how this point
// joins the next one, so an artist changing one point changes one span.
cfloat3 eval_span(const std::vector<StrokePoint>& pts, std::size_t i, bool closed, float t) {
    const std::size_t n = pts.size();
    const StrokePoint& a = pts[i];
    const StrokePoint& b = pts[wrap(static_cast<std::ptrdiff_t>(i) + 1, n, closed)];
    switch (a.type) {
        case StrokePointType::Spline:
            return catmull_rom(span_at(pts, i, closed), t);
        case StrokePointType::BSpline:
            return bspline(span_at(pts, i, closed), t);
        case StrokePointType::Bezier:
            // The span is shaped by this point's outgoing handle and the next
            // point's incoming one, so a Bezier span is well defined even when
            // its neighbour is some other type.
            return bezier(a.pos, a.pos + a.out_handle, b.pos + b.in_handle, b.pos, t);
        case StrokePointType::Hard:
        default:
            return a.pos + (b.pos - a.pos) * t;
    }
}

// Bisect while the midpoint of a sub-span sits further from its chord than the
// tolerance allows. Recursive rather than a fixed sample count so the
// tolerance is honoured rather than approximated, and depth-bounded so a tiny
// tolerance cannot grow the tape without limit.
void emit_span(const std::vector<StrokePoint>& pts, std::size_t i, bool closed, float tolerance,
               float t0, float t1, cfloat3 x0, cfloat3 x1, int depth,
               std::vector<float>* out_t) {
    if (depth < kMaxCurveDepth) {
        float tm = (t0 + t1) * 0.5f;
        cfloat3 xm = eval_span(pts, i, closed, tm);
        cfloat3 chord = (x0 + x1) * 0.5f;
        if (kernel::clength(xm - chord) > tolerance) {
            emit_span(pts, i, closed, tolerance, t0, tm, x0, xm, depth + 1, out_t);
            out_t->push_back(tm);
            emit_span(pts, i, closed, tolerance, tm, t1, xm, x1, depth + 1, out_t);
            return;
        }
    }
}

}  // namespace

bool curve_is_polyline(const std::vector<StrokePoint>& points, bool closed) {
    if (closed && points.size() > 2) return false;
    for (const StrokePoint& p : points)
        if (p.type != StrokePointType::Hard) return false;
    return true;
}

std::vector<StrokePoint> tessellate_curve(const std::vector<StrokePoint>& points, bool closed,
                                          float tolerance) {
    // Returned unchanged rather than rebuilt: the same points in the same
    // order is what makes an all-hard list compile to a bit-identical tape.
    if (curve_is_polyline(points, closed)) return points;
    if (points.size() < 2) return points;

    const float tol = std::max(tolerance, kMinCurveTolerance);
    const std::size_t n = points.size();
    const std::size_t spans = closed ? n : n - 1;

    std::vector<StrokePoint> out;
    out.reserve(n * 4);
    std::vector<float> ts;
    for (std::size_t i = 0; i < spans; ++i) {
        const StrokePoint& a = points[i];
        const StrokePoint& b = points[wrap(static_cast<std::ptrdiff_t>(i) + 1, n, closed)];

        StrokePoint start = a;
        start.type = StrokePointType::Hard;  // the output is a plain chain
        start.in_handle = start.out_handle = cf3(0, 0, 0);
        start.pos = eval_span(points, i, closed, 0.0f);
        out.push_back(start);

        if (a.type != StrokePointType::Hard) {
            ts.clear();
            emit_span(points, i, closed, tol, 0.0f, 1.0f, eval_span(points, i, closed, 0.0f),
                      eval_span(points, i, closed, 1.0f), 0, &ts);
            for (float t : ts) {
                StrokePoint mid;
                mid.pos = eval_span(points, i, closed, t);
                mid.radius = a.radius + (b.radius - a.radius) * t;
                out.push_back(mid);
            }
        }
    }
    // The final point: the last span's end for an open curve, and a repeat of
    // the first for a closed one so the ring actually joins.
    StrokePoint last = closed ? points[0] : points[n - 1];
    last.type = StrokePointType::Hard;
    last.in_handle = last.out_handle = cf3(0, 0, 0);
    last.pos = eval_span(points, spans - 1, closed, 1.0f);
    out.push_back(last);
    return out;
}

float guide_arc_length(const std::vector<StrokePoint>& guide) {
    float total = 0.0f;
    for (std::size_t i = 1; i < guide.size(); ++i)
        total += kernel::clength(guide[i].pos - guide[i - 1].pos);
    return total;
}

float guide_bend_radius(const std::vector<StrokePoint>& guide) {
    // The circle through each consecutive triple: r = |u||v||w| / (4 * area),
    // written with 2*area because the cross product gives that directly.
    float tightest = 1e6f;
    for (std::size_t i = 1; i + 1 < guide.size(); ++i) {
        kernel::cfloat3 u = guide[i].pos - guide[i - 1].pos;
        kernel::cfloat3 v = guide[i + 1].pos - guide[i].pos;
        kernel::cfloat3 w = guide[i + 1].pos - guide[i - 1].pos;
        float lu = kernel::clength(u), lv = kernel::clength(v), lw = kernel::clength(w);
        float area2 = kernel::clength(kernel::ccross(u, v));  // 2 * triangle area
        if (area2 < 1e-9f) continue;                          // collinear: straight
        tightest = kernel::cmin(tightest, lu * lv * lw / (2.0f * area2));
    }
    return tightest;
}

}  // namespace scene
}  // namespace clay
