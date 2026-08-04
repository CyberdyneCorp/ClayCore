#pragma once

// 2D profile SDFs for extrude/revolve (docs/01 §1.3). All exact.
// sd_bezier2 is an UNSIGNED curve distance (used for strokes and as a
// building block); closed profiles carry their own sign.
// Cubic Béziers are ruled out in-kernel (quintic roots): the host converts
// them to quadratic chains (clay/math/bezier.h) and kernels evaluate those.

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

CLAY_FN float sd_circle2(cfloat2 p, float r) { return clength(p) - r; }

CLAY_FN float sd_box2(cfloat2 p, cfloat2 b) {
    cfloat2 d = cabs(p) - b;
    return clength(cmax(d, 0.0f)) + cmin(cmax(d.x, d.y), 0.0f);
}

CLAY_FN float sd_segment2(cfloat2 p, cfloat2 a, cfloat2 b) {
    cfloat2 pa = p - a, ba = b - a;
    float h = cclamp(cdot(pa, ba) / cdot(ba, ba), 0.0f, 1.0f);
    return clength(pa - ba * h);
}

CLAY_FN float sd_hexagon2(cfloat2 p, float r) {
    const cfloat3 k = cf3(-0.866025404f, 0.5f, 0.577350269f);
    cfloat2 q = cabs(p);
    q = q - 2.0f * cmin(cdot(cf2(k.x, k.y), q), 0.0f) * cf2(k.x, k.y);
    q = q - cf2(cclamp(q.x, -k.z * r, k.z * r), r);
    return clength(q) * csign(q.y);
}

CLAY_FN float sd_equilateral_triangle2(cfloat2 p, float r) {
    const float k = 1.7320508f;
    cfloat2 q = cf2(cabs(p.x) - r, p.y + r / k);
    if (q.x + k * q.y > 0.0f) q = cf2(q.x - k * q.y, -k * q.x - q.y) * 0.5f;
    q.x -= cclamp(q.x, -2.0f * r, 0.0f);
    return -clength(q) * csign(q.y);
}

// r1 = bottom half-width, r2 = top half-width, he = half-height.
CLAY_FN float sd_trapezoid2(cfloat2 p, float r1, float r2, float he) {
    cfloat2 k1 = cf2(r2, he);
    cfloat2 k2 = cf2(r2 - r1, 2.0f * he);
    cfloat2 q = cf2(cabs(p.x), p.y);
    cfloat2 ca = cf2(q.x - cmin(q.x, (q.y < 0.0f) ? r1 : r2), cabs(q.y) - he);
    cfloat2 cb = q - k1 + k2 * cclamp(cdot(k1 - q, k2) / cdot2(k2), 0.0f, 1.0f);
    float s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
    return s * csqrt(cmin(cdot2(ca), cdot2(cb)));
}

// Lens of two circles radius r whose centers are 2d apart (d < r).
CLAY_FN float sd_vesica2(cfloat2 p, float r, float d) {
    cfloat2 q = cabs(p);
    float b = csqrt(r * r - d * d);
    return ((q.y - b) * d > q.x * b) ? clength(q - cf2(0.0f, b))
                                     : clength(q - cf2(-d, 0.0f)) - r;
}

// Exact arbitrary polygon: per-edge closest point, even-odd crossing sign.
// Handles concave and self-intersecting polygons (even-odd rule).
CLAY_FN float sd_polygon2(CLAY_DEVICE const cfloat2* v, int n, cfloat2 p) {
    float d = cdot2(p - v[0]);
    float s = 1.0f;
    int j = n - 1;
    for (int i = 0; i < n; j = i, i++) {
        cfloat2 e = v[j] - v[i];
        cfloat2 w = p - v[i];
        cfloat2 b = w - e * cclamp(cdot(w, e) / cdot(e, e), 0.0f, 1.0f);
        d = cmin(d, cdot2(b));
        bool c0 = p.y >= v[i].y;
        bool c1 = p.y < v[j].y;
        bool c2 = e.x * w.y > e.y * w.x;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
    }
    return s * csqrt(d);
}

// Same even-odd polygon, over a raw (x, y) float pool — the form the tape
// uses, since its out-of-line payload is a flat float array.
CLAY_FN float sd_polygon2_raw(CLAY_DEVICE const float* v, int n, cfloat2 p) {
    cfloat2 v0 = cf2(v[0], v[1]);
    float d = cdot2(p - v0);
    float s = 1.0f;
    int j = n - 1;
    for (int i = 0; i < n; j = i, i++) {
        cfloat2 vi = cf2(v[i * 2], v[i * 2 + 1]);
        cfloat2 vj = cf2(v[j * 2], v[j * 2 + 1]);
        cfloat2 e = vj - vi;
        cfloat2 w = p - vi;
        cfloat2 b = w - e * cclamp(cdot(w, e) / cdot(e, e), 0.0f, 1.0f);
        d = cmin(d, cdot2(b));
        bool c0 = p.y >= vi.y;
        bool c1 = p.y < vj.y;
        bool c2 = e.x * w.y > e.y * w.x;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) s = -s;
    }
    return s * csqrt(d);
}

// Unsigned distance to a quadratic Bézier A-B-C (closed-form cubic solve).
// Degenerate (collinear) control points fall back to the segment distance.
CLAY_FN float sd_bezier2(cfloat2 pos, cfloat2 A, cfloat2 B, cfloat2 C) {
    cfloat2 a = B - A;
    cfloat2 b = A - B * 2.0f + C;
    cfloat2 c = a * 2.0f;
    cfloat2 d = A - pos;
    float bb = cdot(b, b);
    if (bb < 1e-12f) return sd_segment2(pos, A, C);
    float kk = 1.0f / bb;
    float kx = kk * cdot(a, b);
    float ky = kk * (2.0f * cdot(a, a) + cdot(d, b)) / 3.0f;
    float kz = kk * cdot(d, a);
    float res;
    float pq = ky - kx * kx;
    float p3 = pq * pq * pq;
    float q = kx * (2.0f * kx * kx - 3.0f * ky) + kz;
    float h = q * q + 4.0f * p3;
    if (h >= 0.0f) {
        h = csqrt(h);
        cfloat2 x = cf2((h - q) * 0.5f, (-h - q) * 0.5f);
        cfloat2 uv = cf2(csign(x.x) * cpow(cabs(x.x), 1.0f / 3.0f),
                         csign(x.y) * cpow(cabs(x.y), 1.0f / 3.0f));
        float t = cclamp(uv.x + uv.y - kx, 0.0f, 1.0f);
        res = cdot2(d + (c + b * t) * t);
    } else {
        float z = csqrt(-pq);
        float v = cacos(q / (pq * z * 2.0f)) / 3.0f;
        float m = ccos(v);
        float nn = csin(v) * 1.732050808f;
        float t0 = cclamp((m + m) * z - kx, 0.0f, 1.0f);
        float t1 = cclamp((-nn - m) * z - kx, 0.0f, 1.0f);
        res = cmin(cdot2(d + (c + b * t0) * t0), cdot2(d + (c + b * t1) * t1));
    }
    return csqrt(res);
}

CLAY_NS_END
