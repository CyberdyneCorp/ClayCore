#pragma once

// 3D primitive signed distance functions (docs/01 §1.1–1.2, after Quilez).
// Exact unless the name says _bound; bound primitives never overestimate
// (safe to sphere-trace) but underestimate away from the surface.
// Conventions: negative inside, zero on surface, positive outside.

#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

CLAY_FN float sd_sphere(cfloat3 p, float r) { return clength(p) - r; }

CLAY_FN float sd_box(cfloat3 p, cfloat3 b) {
    cfloat3 q = cabs(p) - b;
    return clength(cmax(q, 0.0f)) + cmin(cmax(q.x, cmax(q.y, q.z)), 0.0f);
}

CLAY_FN float sd_round_box(cfloat3 p, cfloat3 b, float r) {
    cfloat3 q = cabs(p) - b + cf3(r, r, r);
    return clength(cmax(q, 0.0f)) + cmin(cmax(q.x, cmax(q.y, q.z)), 0.0f) - r;
}

CLAY_FN float sd_box_frame(cfloat3 p, cfloat3 b, float e) {
    cfloat3 a = cabs(p) - b;
    cfloat3 q = cabs(a + cf3(e, e, e)) - cf3(e, e, e);
    float d0 = clength(cmax(cf3(a.x, q.y, q.z), 0.0f)) + cmin(cmax(a.x, cmax(q.y, q.z)), 0.0f);
    float d1 = clength(cmax(cf3(q.x, a.y, q.z), 0.0f)) + cmin(cmax(q.x, cmax(a.y, q.z)), 0.0f);
    float d2 = clength(cmax(cf3(q.x, q.y, a.z), 0.0f)) + cmin(cmax(q.x, cmax(q.y, a.z)), 0.0f);
    return cmin(cmin(d0, d1), d2);
}

// R = major radius (ring), r = minor radius (tube).
CLAY_FN float sd_torus(cfloat3 p, float R, float r) {
    cfloat2 q = cf2(clength(cf2(p.x, p.z)) - R, p.y);
    return clength(q) - r;
}

// sc = (sin, cos) of the aperture half-angle.
CLAY_FN float sd_capped_torus(cfloat3 p, cfloat2 sc, float ra, float rb) {
    float px = cabs(p.x);
    cfloat2 pxy = cf2(px, p.y);
    float k = (sc.y * px > sc.x * p.y) ? cdot(pxy, sc) : clength(pxy);
    return csqrt(cdot2(cf3(px, p.y, p.z)) + ra * ra - 2.0f * ra * k) - rb;
}

CLAY_FN float sd_link(cfloat3 p, float le, float r1, float r2) {
    cfloat3 q = cf3(p.x, cmax(cabs(p.y) - le, 0.0f), p.z);
    return clength(cf2(clength(cf2(q.x, q.y)) - r1, q.z)) - r2;
}

CLAY_FN float sd_capsule(cfloat3 p, cfloat3 a, cfloat3 b, float r) {
    cfloat3 pa = p - a, ba = b - a;
    float h = cclamp(cdot(pa, ba) / cdot(ba, ba), 0.0f, 1.0f);
    return clength(pa - ba * h) - r;
}

// Infinite cylinder along Y, cross-section center c, radius r.
CLAY_FN float sd_cylinder_inf(cfloat3 p, cfloat2 c, float r) {
    return clength(cf2(p.x, p.z) - c) - r;
}

// Vertical capped cylinder: radius r, half-height h.
CLAY_FN float sd_capped_cylinder(cfloat3 p, float r, float h) {
    cfloat2 d = cabs(cf2(clength(cf2(p.x, p.z)), p.y)) - cf2(r, h);
    return cmin(cmax(d.x, d.y), 0.0f) + clength(cmax(d, 0.0f));
}

// Capped cylinder between arbitrary endpoints a, b.
CLAY_FN float sd_capped_cylinder_ab(cfloat3 p, cfloat3 a, cfloat3 b, float r) {
    cfloat3 ba = b - a, pa = p - a;
    float baba = cdot(ba, ba), paba = cdot(pa, ba);
    float x = clength(pa * baba - ba * paba) - r * baba;
    float y = cabs(paba - baba * 0.5f) - baba * 0.5f;
    float x2 = x * x, y2 = y * y * baba;
    float d = (cmax(x, y) < 0.0f) ? -cmin(x2, y2)
                                  : (((x > 0.0f) ? x2 : 0.0f) + ((y > 0.0f) ? y2 : 0.0f));
    return csign(d) * csqrt(cabs(d)) / baba;
}

CLAY_FN float sd_rounded_cylinder(cfloat3 p, float ra, float rb, float h) {
    cfloat2 d = cf2(clength(cf2(p.x, p.z)) - ra + rb, cabs(p.y) - h + rb);
    return cmin(cmax(d.x, d.y), 0.0f) + clength(cmax(d, 0.0f)) - rb;
}

// Exact cone: c = (sin, cos) of the half-angle, apex at origin opening down to height h.
CLAY_FN float sd_cone(cfloat3 p, cfloat2 c, float h) {
    cfloat2 q = cf2(h * c.x / c.y, -h);
    cfloat2 w = cf2(clength(cf2(p.x, p.z)), p.y);
    cfloat2 a = w - q * cclamp(cdot(w, q) / cdot(q, q), 0.0f, 1.0f);
    cfloat2 b = w - q * cf2(cclamp(w.x / q.x, 0.0f, 1.0f), 1.0f);
    float k = csign(q.y);
    float d = cmin(cdot(a, a), cdot(b, b));
    float s = cmax(k * (w.x * q.y - w.y * q.x), k * (w.y - q.y));
    return csqrt(d) * csign(s);
}

// Capped cone: half-height h, bottom radius r1, top radius r2.
CLAY_FN float sd_capped_cone(cfloat3 p, float h, float r1, float r2) {
    cfloat2 q = cf2(clength(cf2(p.x, p.z)), p.y);
    cfloat2 k1 = cf2(r2, h);
    cfloat2 k2 = cf2(r2 - r1, 2.0f * h);
    cfloat2 ca = cf2(q.x - cmin(q.x, (q.y < 0.0f) ? r1 : r2), cabs(q.y) - h);
    cfloat2 cb = q - k1 + k2 * cclamp(cdot(k1 - q, k2) / cdot2(k2), 0.0f, 1.0f);
    float s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
    return s * csqrt(cmin(cdot2(ca), cdot2(cb)));
}

// Sphere-swept cone along Y: base sphere radius r1 at origin, top r2 at height h.
CLAY_FN float sd_round_cone(cfloat3 p, float r1, float r2, float h) {
    // When the radii differ by more than the height, one end sphere CONTAINS
    // the other: there is no conical flank to measure against, and the shape is
    // simply the larger sphere. Falling through computes csqrt of a negative
    // radicand and returns NaN, which every combine op then propagates — one
    // such item turns the whole document into NaN.
    if (cabs(r1 - r2) >= h)
        return (r1 >= r2) ? clength(p) - r1 : clength(p - cf3(0.0f, h, 0.0f)) - r2;
    float b = (r1 - r2) / h;
    float a = csqrt(1.0f - b * b);
    cfloat2 q = cf2(clength(cf2(p.x, p.z)), p.y);
    float k = cdot(q, cf2(-b, a));
    if (k < 0.0f) return clength(q) - r1;
    if (k > a * h) return clength(q - cf2(0.0f, h)) - r2;
    return cdot(q, cf2(a, b)) - r1;
}

// Sphere-swept cone between arbitrary endpoints (also the stroke-segment kernel).
CLAY_FN float sd_round_cone_ab(cfloat3 p, cfloat3 a, cfloat3 b, float r1, float r2) {
    cfloat3 ba = b - a;
    float l2 = cdot(ba, ba);
    float rr = r1 - r2;
    float a2 = l2 - rr * rr;
    // Same containment case as sd_round_cone, and the same reason it matters:
    // this is the stroke-segment kernel, so a tapered two-point stroke whose
    // radii differ by more than its length lands here. It also covers coincident
    // endpoints, where l2 is zero and il2 below would be infinite.
    if (a2 <= 0.0f)
        return (r1 >= r2) ? clength(p - a) - r1 : clength(p - b) - r2;
    float il2 = 1.0f / l2;
    cfloat3 pa = p - a;
    float y = cdot(pa, ba);
    float z = y - l2;
    cfloat3 xv = pa * l2 - ba * y;
    float x2 = cdot(xv, xv);
    float y2 = y * y * l2;
    float z2 = z * z * l2;
    float k = csign(rr) * rr * rr * x2;
    if (csign(z) * a2 * z2 > k) return csqrt(x2 + z2) * il2 - r2;
    if (csign(y) * a2 * y2 < k) return csqrt(x2 + y2) * il2 - r1;
    return (csqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

// n must be normalized; h = plane offset (distance from origin along -n).
CLAY_FN float sd_plane(cfloat3 p, cfloat3 n, float h) { return cdot(p, n) + h; }

// h = (face radius, half-depth), prism along Z.
CLAY_FN float sd_hex_prism(cfloat3 p, cfloat2 h) {
    const cfloat3 k = cf3(-0.8660254f, 0.5f, 0.57735f);
    cfloat3 q = cabs(p);
    cfloat2 xy = cf2(q.x, q.y);
    xy = xy - 2.0f * cmin(cdot(cf2(k.x, k.y), xy), 0.0f) * cf2(k.x, k.y);
    cfloat2 d = cf2(
        clength(xy - cf2(cclamp(xy.x, -k.z * h.x, k.z * h.x), h.x)) * csign(xy.y - h.x),
        q.z - h.y);
    return cmin(cmax(d.x, d.y), 0.0f) + clength(cmax(d, 0.0f));
}

CLAY_FN float sd_octahedron(cfloat3 p, float s) {
    cfloat3 q = cabs(p);
    float m = q.x + q.y + q.z - s;
    cfloat3 t;
    if (3.0f * q.x < m)
        t = cf3(q.x, q.y, q.z);
    else if (3.0f * q.y < m)
        t = cf3(q.y, q.z, q.x);
    else if (3.0f * q.z < m)
        t = cf3(q.z, q.x, q.y);
    else
        return m * 0.57735027f;
    float k = cclamp(0.5f * (t.z - t.y + s), 0.0f, s);
    return clength(cf3(t.x, t.y - s + k, t.z - k));
}

// Unit square base at y=0, apex at height h.
CLAY_FN float sd_pyramid(cfloat3 p, float h) {
    float m2 = h * h + 0.25f;
    cfloat2 xz = cabs(cf2(p.x, p.z));
    if (xz.y > xz.x) xz = cf2(xz.y, xz.x);
    xz = xz - cf2(0.5f, 0.5f);
    cfloat3 q = cf3(xz.y, h * p.y - 0.5f * xz.x, h * xz.x + 0.5f * p.y);
    float s = cmax(-q.x, 0.0f);
    float t = cclamp((q.y - 0.5f * xz.y) / (m2 + 0.25f), 0.0f, 1.0f);
    float a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
    float b = m2 * (q.x + 0.5f * t) * (q.x + 0.5f * t) + (q.y - m2 * t) * (q.y - m2 * t);
    float d2 = cmin(q.y, -q.x * m2 - q.y * 0.5f) > 0.0f ? 0.0f : cmin(a, b);
    return csqrt((d2 + q.z * q.z) / m2) * csign(cmax(q.z, -p.y));
}

// Sphere of radius r cut by the plane y = h (keeps the part above).
CLAY_FN float sd_cut_sphere(cfloat3 p, float r, float h) {
    float w = csqrt(r * r - h * h);
    cfloat2 q = cf2(clength(cf2(p.x, p.z)), p.y);
    float s = cmax((h - r) * q.x * q.x + w * w * (h + r - 2.0f * q.y), h * q.x - w * q.y);
    if (s < 0.0f) return clength(q) - r;
    if (q.x < w) return h - q.y;
    return clength(q - cf2(w, h));
}

CLAY_FN float sd_cut_hollow_sphere(cfloat3 p, float r, float h, float t) {
    float w = csqrt(r * r - h * h);
    cfloat2 q = cf2(clength(cf2(p.x, p.z)), p.y);
    float d = (h * q.x < w * q.y) ? clength(q - cf2(w, h)) : cabs(clength(q) - r);
    return d - t;
}

// c = (sin, cos) of the cone half-angle, ra = radius.
CLAY_FN float sd_solid_angle(cfloat3 p, cfloat2 c, float ra) {
    cfloat2 q = cf2(clength(cf2(p.x, p.z)), p.y);
    float l = clength(q) - ra;
    float m = clength(q - c * cclamp(cdot(q, c), 0.0f, ra));
    return cmax(l, m * csign(c.y * q.x - c.x * q.y));
}

CLAY_FN float sd_tetrahedron(cfloat3 p, float r) {
    return (cmax(cabs(p.x + p.y) - p.z, cabs(p.x - p.y) + p.z) - r) * 0.57735027f;
}

// Dodecahedron / icosahedron via plane folds (golden ratio). Face distances
// are exact; edge/vertex neighborhoods carry the usual max-CSG seam caveat.
CLAY_FN float sd_dodecahedron(cfloat3 p, float r) {
    const float phi = 1.6180339887f;
    cfloat3 n = cnormalize(cf3(phi, 1.0f, 0.0f));
    cfloat3 q = cabs(p);
    float d = cdot(q, cf3(n.x, n.y, n.z));
    d = cmax(d, cdot(q, cf3(n.z, n.x, n.y)));
    d = cmax(d, cdot(q, cf3(n.y, n.z, n.x)));
    return d - r;
}

CLAY_FN float sd_icosahedron(cfloat3 p, float r) {
    const float phi = 1.6180339887f;
    cfloat3 n1 = cnormalize(cf3(1.0f, 1.0f, 1.0f));
    cfloat3 n2 = cnormalize(cf3(0.0f, 1.0f / phi, phi));
    cfloat3 q = cabs(p);
    float d = cdot(q, n1);
    d = cmax(d, cdot(q, cf3(n2.x, n2.y, n2.z)));
    d = cmax(d, cdot(q, cf3(n2.z, n2.x, n2.y)));
    d = cmax(d, cdot(q, cf3(n2.y, n2.z, n2.x)));
    return d - r;
}

// -- bound-only primitives (underestimate; never overestimate) ---------------

// No closed-form exact ellipsoid SDF exists (quartic). Hybrid bound:
// outside, IQ's gradient-scaled estimate (exact on the axes); inside, the
// provably safe (k0-1)*min(r) — |p-q| >= min(r)*|1-k0| for any surface
// point q, so it never overestimates depth (IQ's estimate does).
CLAY_FN float sd_ellipsoid_bound(cfloat3 p, cfloat3 r) {
    float rmin = cmin(r.x, cmin(r.y, r.z));
    float k0 = clength(p / r);
    if (k0 < 1e-6f) return -rmin;
    float k1 = clength(p / (r * r));
    float grad_est = k0 * (k0 - 1.0f) / k1;
    if (k0 < 1.0f) return cmax(grad_est, (k0 - 1.0f) * rmin);
    return grad_est;
}

// h = (width, half-depth), prism along Z.
CLAY_FN float sd_tri_prism_bound(cfloat3 p, cfloat2 h) {
    cfloat3 q = cabs(p);
    return cmax(q.z - h.y, cmax(q.x * 0.866025f + p.y * 0.5f, -p.y) - h.x * 0.5f);
}

CLAY_FN float sd_octahedron_bound(cfloat3 p, float s) {
    cfloat3 q = cabs(p);
    return (q.x + q.y + q.z - s) * 0.57735027f;
}

// L-n norm sphere (superellipsoid family). Safe bound for n >= 2 only:
// the n-norm is 1-Lipschitz in the Euclidean metric iff n >= 2.
CLAY_FN float sd_lnorm_sphere_bound(cfloat3 p, float r, float n) {
    cfloat3 q = cabs(p);
    return cpow(cpow(q.x, n) + cpow(q.y, n) + cpow(q.z, n), 1.0f / n) - r;
}

CLAY_NS_END
