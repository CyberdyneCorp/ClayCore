#include "clay/kernel/ease.h"
#include "clay/scene/bounds.h"

#include <bit>
#include <cmath>

#include "clay/scene/curve.h"

#include "clay/kernel/exactness.h"

namespace clay {
namespace scene {

using kernel::cf3;
using kernel::cfloat3;
using math::Aabb;

kernel::cfloat2 profile_extent_of(const Profile& profile,
                                  const std::vector<kernel::cfloat2>& points) {
    float ex = 0.0f, ey = 0.0f;
    const float* pp = profile.params;
    switch (profile.type) {
        case kernel::cprofile_circle: ex = ey = pp[0]; break;
        case kernel::cprofile_box: ex = pp[0]; ey = pp[1]; break;
        // face radius -> vertex radius
        case kernel::cprofile_hexagon: ex = ey = pp[0] * 1.1547006f; break;
        case kernel::cprofile_triangle: ex = ey = pp[0] * 2.0f; break;
        case kernel::cprofile_trapezoid:
            ex = kernel::cmax(pp[0], pp[1]);
            ey = pp[2];
            break;
        case kernel::cprofile_vesica: ex = ey = pp[0]; break;
        case kernel::cprofile_polygon:
            for (const kernel::cfloat2& v : points) {
                ex = kernel::cmax(ex, kernel::cabs(v.x));
                ey = kernel::cmax(ey, kernel::cabs(v.y));
            }
            break;
        default: break;
    }
    return kernel::cf2(ex, ey);
}

Aabb prim_local_bounds(const Node& item) {
    const float* q = item.prim.params;
    Aabb b;
    auto sym = [&](float x, float y, float z) {
        b.expand(cf3(-x, -y, -z));
        b.expand(cf3(x, y, z));
    };
    switch (item.prim.type) {
        case PrimType::Sphere: sym(q[0], q[0], q[0]); break;
        case PrimType::Box: sym(q[0], q[1], q[2]); break;
        case PrimType::RoundBox: sym(q[0], q[1], q[2]); break;
        case PrimType::BoxFrame: sym(q[0], q[1], q[2]); break;
        case PrimType::Torus: sym(q[0] + q[1], q[1], q[0] + q[1]); break;
        case PrimType::Capsule: {
            cfloat3 a = cf3(q[0], q[1], q[2]), c = cf3(q[3], q[4], q[5]);
            float r = q[6];
            b.expand(kernel::cmin(a, c) - cf3(r, r, r));
            b.expand(kernel::cmax(a, c) + cf3(r, r, r));
            break;
        }
        case PrimType::CappedCylinder: sym(q[0], q[1], q[0]); break;
        case PrimType::RoundedCylinder: sym(q[0], q[2], q[0]); break;
        case PrimType::CappedCone: sym(kernel::cmax(q[1], q[2]), q[0], kernel::cmax(q[1], q[2])); break;
        case PrimType::RoundCone: {
            float r = kernel::cmax(q[0], q[1]);
            b.expand(cf3(-r, -q[0], -r));
            b.expand(cf3(r, q[2] + q[1], r));
            break;
        }
        case PrimType::Ellipsoid: sym(q[0], q[1], q[2]); break;
        case PrimType::Octahedron: sym(q[0], q[0], q[0]); break;
        case PrimType::HexPrism: sym(q[0] * 1.1547005f, q[0] * 1.1547005f, q[1]); break;
        case PrimType::Pyramid: {
            b.expand(cf3(-0.5f, 0.0f, -0.5f));
            b.expand(cf3(0.5f, q[0], 0.5f));
            break;
        }
        // -- backfill ------------------------------------------------------
        case PrimType::CappedTorus: sym(q[2] + q[3], q[2] + q[3], q[3]); break;
        case PrimType::Link: sym(q[1] + q[2], q[0] + q[1] + q[2], q[2]); break;
        case PrimType::Cone: {
            // apex at the origin, opening downward to height h
            float radius = q[2] * q[0] / kernel::cmax(q[1], 1e-6f);
            b.expand(cf3(-radius, -q[2], -radius));
            b.expand(cf3(radius, 0.0f, radius));
            break;
        }
        case PrimType::CutSphere: sym(q[0], q[0], q[0]); break;
        case PrimType::CutHollowSphere: sym(q[0] + q[2], q[0] + q[2], q[0] + q[2]); break;
        case PrimType::SolidAngle: sym(q[2], q[2], q[2]); break;
        case PrimType::Tetrahedron: sym(q[0], q[0], q[0]); break;
        case PrimType::Dodecahedron:
        case PrimType::Icosahedron: sym(q[0] * 1.9f, q[0] * 1.9f, q[0] * 1.9f); break;
        case PrimType::TriPrism: sym(q[0], q[0], q[1]); break;
        case PrimType::OctahedronCheap: sym(q[0], q[0], q[0]); break;
        case PrimType::LNormSphere: sym(q[0], q[0], q[0]); break;
        // no finite extent — item_influence_bound reports infinite instead
        case PrimType::Plane:
        case PrimType::CylinderInfinite: return Aabb::infinite();
        case PrimType::Volume: {
            // The sampled box: the surface is inside it by construction.
            if (item.volume) {
                math::Aabb vb = item.volume->bounds();
                if (!vb.empty()) {
                    b.expand(vb.min);
                    b.expand(vb.max);
                }
            }
            break;
        }
        case PrimType::Swept: {
            // The guide's own extent, dilated by the widest profile: the
            // swept surface never leaves that, whatever the frame does.
            std::vector<StrokePoint> tess =
                curve_is_polyline(item.stroke, false)
                    ? item.stroke
                    : tessellate_curve(item.stroke, false, item.curve_tolerance);
            float widest = 0.0f;
            for (std::size_t i = 0; i < item.profiles.size(); ++i) {
                const std::vector<kernel::cfloat2>& pts =
                    i < item.profile_polygons.size() ? item.profile_polygons[i]
                                                     : std::vector<kernel::cfloat2>{};
                kernel::cfloat2 e = profile_extent_of(item.profiles[i], pts);
                widest = kernel::cmax(widest, kernel::cmax(e.x, e.y));
            }
            for (const StrokePoint& p : tess) {
                b.expand(p.pos - cf3(widest, widest, widest));
                b.expand(p.pos + cf3(widest, widest, widest));
            }
            break;
        }
        case PrimType::Loft: {
            // The union of every profile's extent, lifted to the slab: the
            // interpolated cross-section never leaves the widest of them.
            float ex = 0.0f, ey = 0.0f;
            for (std::size_t i = 0; i < item.profiles.size(); ++i) {
                const std::vector<kernel::cfloat2>& pts =
                    i < item.profile_polygons.size() ? item.profile_polygons[i]
                                                     : std::vector<kernel::cfloat2>{};
                kernel::cfloat2 e = profile_extent_of(item.profiles[i], pts);
                ex = kernel::cmax(ex, e.x);
                ey = kernel::cmax(ey, e.y);
            }
            b.expand(cf3(-ex, -ey, -q[0]));
            b.expand(cf3(ex, ey, q[0]));
            break;
        }
        case PrimType::Extrude:
        case PrimType::Revolve: {
            // 2D extent of the profile, then lifted: a slab for extrusion,
            // a full circular sweep for revolution
            kernel::cfloat2 e = profile_extent_of(item.profile, item.profile_points);
            float ex = e.x, ey = e.y;
            if (item.prim.type == PrimType::Extrude) {
                float h = q[0];
                b.expand(cf3(-ex, -ey, -h));
                b.expand(cf3(ex, ey, h));
            } else {
                float radius = kernel::cabs(q[0]) + ex;  // offset plus profile reach
                b.expand(cf3(-radius, -ey, -radius));
                b.expand(cf3(radius, ey, radius));
            }
            break;
        }
        case PrimType::Armature: {
            // Nodes only, and no tessellation: an armature's links are straight
            // sphere-swept segments between a node and its parent, so the union
            // of the node spheres already contains every link. A stroke needs
            // the tessellated curve because a spline bulges outside its control
            // polygon; an armature has no curve to bulge.
            for (const StrokePoint& n : item.stroke) {
                b.expand(n.pos - cf3(n.radius, n.radius, n.radius));
                b.expand(n.pos + cf3(n.radius, n.radius, n.radius));
            }
            break;
        }
        case PrimType::Stroke: {
            // Tessellated, not the control points: a spline can pass outside
            // the polygon its control points form, and bounds that missed
            // that would make per-brick culling drop the bulge and picking
            // miss it.
            std::vector<StrokePoint> tess;
            const std::vector<StrokePoint>& pts =
                curve_is_polyline(item.stroke, item.stroke_closed)
                    ? item.stroke
                    : (tess = tessellate_curve(item.stroke, item.stroke_closed,
                                               item.curve_tolerance));
            for (const StrokePoint& p : pts) {
                b.expand(p.pos - cf3(p.radius, p.radius, p.radius));
                b.expand(p.pos + cf3(p.radius, p.radius, p.radius));
            }
            // within-stroke smoothing widens the surface by its support
            if (item.stroke_blend_k > 0.0f)
                b = b.dilated(kernel::csmin_quadratic_support(item.stroke_blend_k));
            break;
        }
    }
    return b;
}

// Conservative widening of a local bound under one domain warp. Twist and
// bend are rotations, so the warped shape stays inside the axis-aligned hull
// of the original's rotational sweep about the relevant axis; taper scales
// the cross-section; displacement moves the surface by at most its amplitude.
Aabb deformed_local_bounds(const Aabb& local, const std::vector<Deformer>& deformers,
                           float curve_tolerance) {
    Aabb b = local;
    for (const Deformer& d : deformers) {
        if (b.empty()) break;
        switch (d.type) {
            case kernel::cdeform_twist_range:
            case kernel::cdeform_twist: {
                // rotation about Y: |(x,z)| preserved -> hull is the cylinder.
                // The RANGED form rotates by at most what the unranged one
                // does over the same span, so the same cylinder contains it.
                float r = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    float x = (i & 1) ? b.max.x : b.min.x;
                    float z = (i & 2) ? b.max.z : b.min.z;
                    r = kernel::cmax(r, kernel::clength(kernel::cf2(x, z)));
                }
                b = Aabb{cf3(-r, b.min.y, -r), cf3(r, b.max.y, r)};
                break;
            }
            case kernel::cdeform_bend_range:
            case kernel::cdeform_bend: {
                // rotation of (x,y) about the origin: |(x,y)| preserved; the
                // ranged form is contained by the unranged hull, as above.
                float r = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    float x = (i & 1) ? b.max.x : b.min.x;
                    float y = (i & 2) ? b.max.y : b.min.y;
                    r = kernel::cmax(r, kernel::clength(kernel::cf2(x, y)));
                }
                b = Aabb{cf3(-r, -r, b.min.z), cf3(r, r, b.max.z)};
                break;
            }
            case kernel::cdeform_lattice_xform:
            case kernel::cdeform_lattice: {
                // A Bernstein combination of the offsets is a convex
                // combination, so no point moves further than the largest of
                // them. Dilating by that is tight and needs no evaluation.
                //
                // The offsets are in CAGE space, so a transformed cage's
                // displacement is that over the transform's scale once it is
                // back in the item's frame.
                float reach = 0.0f;
                for (const kernel::cfloat3& o : d.cage)
                    reach = kernel::cmax(reach, kernel::clength(o));
                if (d.type == kernel::cdeform_lattice_xform) {
                    const float s = kernel::cabs(d.cage_xform.scale);
                    reach = s > 1e-6f ? reach / s : reach;
                }
                b = b.dilated(reach);
                break;
            }
            case kernel::cdeform_bend_curve: {
                // NOT contained by the undeformed item's own neighbourhood, as
                // the rotations above are: a curve can carry material anywhere
                // the guide goes. The hull is the guide's own extent grown by
                // how far the item reaches from its axis.
                //
                // Tessellated at the SAME tolerance the compiler uses, because
                // the field is defined by the compiled polyline rather than by
                // the ideal curve — a coarser sampling here would cut inside
                // the bulge of a spline and understate the bound.
                std::vector<StrokePoint> tess =
                    curve_is_polyline(d.guide, false)
                        ? d.guide
                        : tessellate_curve(d.guide, false, curve_tolerance);
                if (tess.size() < 2) break;  // no guide: the kernel passes the point through
                float r = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    float y = (i & 1) ? b.max.y : b.min.y;
                    float z = (i & 2) ? b.max.z : b.min.z;
                    r = kernel::cmax(r, kernel::clength(kernel::cf2(y, z)));
                }
                Aabb g;
                for (const StrokePoint& sp : tess) g.expand(sp.pos);
                b = g.dilated(r);
                break;
            }
            case kernel::cdeform_taper: {
                // evaluating at p/s scales the shape by s; take the largest
                float s = kernel::cmax(kernel::cmax(d.b, d.c), 1.0f);
                b = Aabb{cf3(b.min.x * s, b.min.y, b.min.z * s),
                         cf3(b.max.x * s, b.max.y, b.max.z * s)};
                break;
            }
            case kernel::cdeform_wrap: {
                // The interval becomes a full turn about Z, so the content's
                // radial band [r+ymin, r+ymax] sweeps a disc. Conservative for
                // an item covering only part of the interval, which is the
                // safe direction.
                float r = kernel::cabs(d.a - d.k) * 0.15915494f;  // per / 2pi
                float outer = kernel::cmax(kernel::cabs(r + b.min.y),
                                           kernel::cabs(r + b.max.y));
                b = Aabb{cf3(-outer, -outer, b.min.z), cf3(outer, outer, b.max.z)};
                break;
            }
            case kernel::cdeform_grab: {
                // Material inside the radius moves by at most the whole
                // displacement, so dilating the bound by it is conservative.
                kernel::cfloat3 disp = cf3(kernel::cabs(d.ext[0]), kernel::cabs(d.ext[1]),
                                           kernel::cabs(d.ext[2]));
                b = Aabb{b.min - disp, b.max + disp};
                break;
            }
            case kernel::cdeform_pose_line: {
                // The weight clamps, so material past the end is fully rotated:
                // the bound is the hull of the original corners and their
                // rotated images, dilated by the arc's sagitta so a large angle
                // cannot bulge outside the chord between them.
                kernel::cfloat3 anchor = cf3(d.k, d.a, d.b);
                kernel::cfloat3 axis = cf3(d.ext[2], d.ext[3], d.ext[4]);
                float axis_len = kernel::clength(axis);
                if (axis_len < 1e-9f) break;
                kernel::cfloat3 unit = axis * (1.0f / axis_len);
                float angle = d.ext[5];

                Aabb swept = b;
                float extent = 0.0f;
                for (int i = 0; i < 8; ++i) {
                    kernel::cfloat3 corner = cf3((i & 1) ? b.max.x : b.min.x,
                                                 (i & 2) ? b.max.y : b.min.y,
                                                 (i & 4) ? b.max.z : b.min.z);
                    kernel::cfloat3 v = corner - anchor;
                    // distance from the rotation axis, which is what sweeps
                    kernel::cfloat3 along = unit * kernel::cdot(unit, v);
                    extent = kernel::cmax(extent, kernel::clength(v - along));
                    float s = kernel::csin(angle), c = kernel::ccos(angle);
                    kernel::cfloat3 rotated =
                        v * c + kernel::ccross(unit, v) * s + unit * (kernel::cdot(unit, v) * (1.0f - c));
                    swept.expand(anchor + rotated);
                }
                // sagitta of the chord across the whole swept angle
                float half = kernel::cmin(kernel::cabs(angle), 3.14159265f) * 0.5f;
                swept = swept.dilated(extent * (1.0f - kernel::ccos(half)));
                b = swept;
                break;
            }
            case kernel::cdeform_noise:
                // The surface can move by the amplitude either way, so the
                // bound grows by it. The fractal is normalized to [-1, 1], so
                // the amplitude really is the whole excursion.
                b = b.dilated(kernel::cabs(d.k));
                break;
            case kernel::cdeform_blob:
                // The same excursion, but the amplitude lives in ext[0] here —
                // k is the region's centre. Sharing noise's case would have
                // dilated by a COORDINATE, which is the kind of wrong bound
                // that shows as culled-away geometry rather than as a crash.
                b = b.dilated(kernel::cabs(d.ext[0]));
                break;
            case kernel::cdeform_alpha:
                // The surface moves by amplitude * stamp, and the stamp's own
                // largest value is what it actually reaches — a stamp that
                // peaks at 0.4 cannot displace by the full amplitude. Charging
                // the peak rather than the amplitude alone keeps the bound
                // honest without loosening it; an all-zero stamp dilates by
                // nothing, which is what makes it exactly the identity.
                b = b.dilated(kernel::cabs(d.stamp.amplitude) * d.stamp.peak);
                break;
            case kernel::cdeform_magnify: {
                // The inverse map samples at centre + v * scale, so material at
                // q appears at centre + (q - centre) / scale: MAGNIFYING pushes
                // it outward by |q - centre| * (1/scale - 1), and inside the
                // support that offset is at most the radius. Pinching moves
                // material inward, which cannot grow the bound, so it dilates by
                // nothing rather than by a negative amount.
                float scale = kernel::cmax(1.0f - d.ext[0], 0.05f);
                float growth = kernel::cmax(kernel::cabs(d.c) * (1.0f / scale - 1.0f), 0.0f);
                b = b.dilated(growth);
                break;
            }
            case kernel::cdeform_pose: {
                // Rotation about the centre keeps a point's distance from it,
                // so the chord 2r|sin(theta/2)| bounds how far anything moves.
                float chord = 2.0f * kernel::cabs(d.c) *
                              kernel::cabs(kernel::csin(d.ext[3] * 0.5f));
                b = b.dilated(chord);
                break;
            }
            case kernel::cdeform_bend_linear:
                // displaces by at most the full vector
                b = Aabb{b.min - cf3(kernel::cabs(d.ext[2]), kernel::cabs(d.ext[3]),
                                     kernel::cabs(d.ext[4])),
                         b.max + cf3(kernel::cabs(d.ext[2]), kernel::cabs(d.ext[3]),
                                     kernel::cabs(d.ext[4]))};
                break;
            case kernel::cdeform_bend_radial:
                // displaces along Y by at most dz
                b = Aabb{cf3(b.min.x, b.min.y - kernel::cabs(d.b), b.min.z),
                         cf3(b.max.x, b.max.y + kernel::cabs(d.b), b.max.z)};
                break;
            case kernel::cdeform_elongate_axis:
            case kernel::cdeform_elongate: {
                // The flat sections are inserted symmetrically, so the shape
                // grows by h on each side of every axis.
                kernel::cfloat3 h = cf3(kernel::cabs(d.k), kernel::cabs(d.a),
                                        kernel::cabs(d.b));
                b = Aabb{b.min - h, b.max + h};
                break;
            }
            case kernel::cdeform_displace:
                b = b.dilated(kernel::cabs(d.k));
                break;
            default:
                break;
        }
    }
    return b;
}

float ease_max_slope(std::uint8_t ease) {
    const int kSamples = 512;
    float worst = 1.0f;
    float prev = kernel::cease(ease, 0.0f);
    for (int i = 1; i <= kSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSamples);
        float v = kernel::cease(ease, t);
        worst = kernel::cmax(worst, kernel::cabs(v - prev) * static_cast<float>(kSamples));
        prev = v;
    }
    return worst * 1.25f;  // sampling headroom
}

bool deformers_break_exactness(const Node& item) {
    // Elongation is non-expansive, so it never shows up in the Lipschitz
    // factor — but its correction is only valid about the origin, so on an
    // asymmetric primitive the field is a bound at Lipschitz 1. That is a
    // reason to drop exactness that the Lipschitz check cannot see.
    for (const Deformer& d : item.deformers) {
        // Per-axis elongation leaves a flat plateau inside the stretch, which
        // is not a distance for any primitive.
        if (d.type == kernel::cdeform_elongate_axis) return true;
        // A lattice warps space non-uniformly; the result is a bound field
        // rather than a distance, whatever the Lipschitz factor comes out as.
        if (Deformer::is_lattice(d.type)) return true;
        if (d.type == kernel::cdeform_elongate && !prim_is_origin_symmetric(item.prim.type))
            return true;
    }
    return false;
}

bool deformers_bound_gradient(const Node& item) {
    // A list, not a computation: each of these has a declared factor that a
    // pairwise slope probe exceeds (taper 1.03-2.5x, wrap 1.05x, bend_curve
    // 7-9x). Tightening the factors would slow every marcher on every item
    // that carries one, for a consumer only the uniform-brick gate is; it
    // refuses these instead, and walks the brick as it always did.
    for (const Deformer& d : item.deformers) {
        if (d.type == kernel::cdeform_taper || d.type == kernel::cdeform_wrap ||
            d.type == kernel::cdeform_bend_curve)
            return false;
    }
    return true;
}

// -- a chain's Lipschitz, priced where its links can actually reach ----------
//
// `deformer_lipschitz` used to fold the whole chain into one running product,
// which charges every pair of links as though they compound. Most of them
// cannot. GRAB, MAGNIFY, POSE, BLOB and ALPHA are all documented as having
// FINITE SUPPORT — outside their own ball the field is untouched, which is what
// makes them brushes rather than modifiers — so two whose balls are far apart
// have no point at which both are anything but the identity, and their factors
// have nothing to multiply.
//
// Measured before this (issue #386): eight `move_surface` drags of radius 0.3
// around the equator of a radius-4 sphere, centres 3.06 apart — ten radii from
// touching — reported a bound of k^8 and a step scale of 0.0616 against 0.7059
// for one drag. That is the whole cost of a Move-heavy session, charged for a
// compounding that cannot physically happen, and a sculptor nudging eight
// different places on a head is the NORMAL case: a second drag on the same spot
// is a correction, not a stroke.
//
// WHY IT IS SOUND. The chain evaluates p -> d0 -> d1 -> ... -> prim, so link k
// sees the point after links 0..k-1 have moved it. Link k is the identity
// unless that point is inside its ball, so two links i < k can both act on one
// evaluation only if their balls are within the distance the chain can carry a
// point between them. Bounding that by the TOTAL travel of every point warp in
// the chain is conservative in the safe direction and needs no ordering
// argument: two links whose balls are further apart than
// `r_i + r_k + total_travel` cannot both be non-identity at any point, so the
// bound over the whole domain is the WORST connected group rather than the
// product of all of them.
//
// The residual: one entry of the easing table returns 5.96e-08 rather than 0 at
// its zero end, so a link is the identity outside its ball only to within that.
// It costs nothing, and not by luck — the region weight is CLAMPED, so outside
// the ball that 5.96e-08 is a CONSTANT. A constant weight makes a grab a rigid
// translation and a magnify a uniform scale within 1e-7 of the identity: no
// slope, which is the only thing a Lipschitz bound measures. The translation it
// does contribute is inside the travel budget above.

namespace {

// Where a deformer stops acting, in the item's own local frame.
struct LinkSupport {
    bool finite = false;
    kernel::cfloat3 centre = cf3(0, 0, 0);
    float radius = 0.0f;
    // How far this link can move a point. Zero for a distance offset, which
    // changes the value and hands the point on untouched.
    float travel = 0.0f;
};

// Does this kind add to the DISTANCE rather than warp the point? The two
// compose differently — see ChainLink::value — and it is also what makes a
// link's travel zero.
bool deformer_is_offset(int type) {
    return type == kernel::cdeform_noise || type == kernel::cdeform_blob ||
           type == kernel::cdeform_alpha || type == kernel::cdeform_displace;
}

LinkSupport link_support(const Deformer& d) {
    LinkSupport s;
    switch (d.type) {
        case kernel::cdeform_grab:
            // `front_only` gates the pull further; it never widens the ball.
            s = {true, cf3(d.k, d.a, d.b), d.c,
                 kernel::clength(cf3(d.ext[0], d.ext[1], d.ext[2]))};
            break;
        case kernel::cdeform_magnify:
            // r -> r * (1 - strength * w), so a point moves at most r*|strength|.
            s = {true, cf3(d.k, d.a, d.b), d.c, d.c * kernel::cabs(d.ext[0])};
            break;
        case kernel::cdeform_pose:
            // A rotation about the centre: an arc of at most radius * angle.
            s = {true, cf3(d.k, d.a, d.b), d.c, d.c * kernel::cabs(d.ext[3])};
            break;
        case kernel::cdeform_blob:
            s = {true, cf3(d.k, d.a, d.b), d.c, 0.0f};
            break;
        case kernel::cdeform_alpha:
            // `k` is not the centre here: the compiler overwrites slot 1 with
            // the stamp's blob handle, so the centre starts one slot later.
            s = {true, cf3(d.a, d.b, d.c), d.stamp.radius, 0.0f};
            break;
        default:
            // Everything else — twist, bend, taper, lattice, pose along a line,
            // the whole modifier family — acts everywhere. `pose_line` is the
            // one that looks like it belongs above and does not: its weight
            // CLAMPS rather than falling to zero, so material past the end is
            // fully rotated (deform.h says so where it is defined).
            break;
    }
    // A degenerate ball would make every link disjoint from everything, which
    // is the unsafe direction; treat it as unbounded instead.
    if (!(s.radius > 0.0f)) s.finite = false;
    return s;
}

// One link of a chain, priced on its own rather than folded into a running
// total — which is what lets two links that cannot both act at one point stop
// multiplying (issue #386).
//
// `value` is a FACTOR for a point warp and an ADDEND for a distance offset,
// because that is how the two compose: a warp multiplies the field's slope
// through the chain rule, while an offset adds its own gradient to it
// (docs/01 §2.5, and `cfi_displace` says the same in one line).
struct ChainLink {
    float value = 1.0f;
    bool additive = false;
    LinkSupport support;
};

std::vector<ChainLink> chain_links(const Node& item) {
    std::vector<ChainLink> links;
    links.reserve(item.deformers.size());
    Aabb local = prim_local_bounds(item);
    for (const Deformer& d : item.deformers) {
        // Priced against the identity, so the number is this link's own
        // contribution and not the chain's total so far.
        kernel::CFieldInfo info = kernel::cfi_exact();
        float radius = 0.0f;
        if (!local.empty()) {
            for (int i = 0; i < 8; ++i) {
                float x = (i & 1) ? local.max.x : local.min.x;
                float y = (i & 2) ? local.max.y : local.min.y;
                float z = (i & 4) ? local.max.z : local.min.z;
                const bool about_y = d.type == kernel::cdeform_twist ||
                                     d.type == kernel::cdeform_twist_range;
                radius = kernel::cmax(radius, about_y ? kernel::clength(kernel::cf2(x, z))
                                                      : kernel::clength(kernel::cf2(x, y)));
            }
        }
        if (d.type == kernel::cdeform_twist) {
            info = kernel::cfi_twist(info, d.k, radius);
        } else if (d.type == kernel::cdeform_bend) {
            info = kernel::cfi_bend(info, d.k, radius);
        } else if (d.type == kernel::cdeform_twist_range ||
                   d.type == kernel::cdeform_bend_range) {
            // Same bound with the ANGULAR RATE the ease actually reaches. The
            // ranged form winds k radians per unit on average across its span,
            // but an eased ramp is steeper than linear somewhere in the middle
            // — smoothstep peaks at 1.5x — and the Lipschitz has to cover the
            // steepest point rather than the average.
            info = d.type == kernel::cdeform_twist_range
                       ? kernel::cfi_twist(info, d.k * ease_max_slope(d.ease), radius)
                       : kernel::cfi_bend(info, d.k * ease_max_slope(d.ease), radius);
        } else if (d.type == kernel::cdeform_lattice ||
                   d.type == kernel::cdeform_lattice_xform) {
            // The SAME bound for both forms, and the reason is worth stating
            // rather than rederiving: the transform is rigid with uniform
            // scale, so with T = sR the warp's Jacobian in the item's frame is
            // R⁻¹ J R — similar to the cage-space Jacobian, hence the same
            // norm. A transform cannot make the field steeper.
            // The DIFFERENCES between neighbouring control points, not their
            // magnitudes: the first says how fast the warp varies, the second
            // only how far it moves material. A cage that translates an item
            // rigidly has large offsets and zero gradient.
            const int nx = static_cast<int>(d.a), ny = static_cast<int>(d.b),
                      nz = static_cast<int>(d.c);
            const kernel::cfloat3 extent =
                kernel::cf3(d.ext[3] - d.ext[0], d.ext[4] - d.ext[1], d.ext[5] - d.ext[2]);
            float worst[3] = {0.0f, 0.0f, 0.0f};
            auto at = [&](int i, int j, int k) {
                return d.cage[static_cast<std::size_t>((k * ny + j) * nx + i)];
            };
            if (!d.cage.empty() && nx >= 2 && ny >= 2 && nz >= 2) {
                for (int k = 0; k < nz; ++k)
                    for (int j = 0; j < ny; ++j)
                        for (int i = 0; i < nx; ++i) {
                            if (i + 1 < nx)
                                worst[0] = kernel::cmax(
                                    worst[0], kernel::clength(at(i + 1, j, k) - at(i, j, k)));
                            if (j + 1 < ny)
                                worst[1] = kernel::cmax(
                                    worst[1], kernel::clength(at(i, j + 1, k) - at(i, j, k)));
                            if (k + 1 < nz)
                                worst[2] = kernel::cmax(
                                    worst[2], kernel::clength(at(i, j, k + 1) - at(i, j, k)));
                        }
            }
            // rate = degree * max|difference| / extent, the Bernstein
            // derivative bound. A flat axis contributes nothing: every point
            // reads the same parameter on it, so the warp cannot vary there.
            auto rate = [](float diff, int count, float span) {
                if (!(span > 1e-6f) || count < 2) return 0.0f;
                return static_cast<float>(count - 1) * diff / span;
            };
            info = kernel::cfi_lattice(info, rate(worst[0], nx, kernel::cabs(extent.x)),
                                       rate(worst[1], ny, kernel::cabs(extent.y)),
                                       rate(worst[2], nz, kernel::cabs(extent.z)));
        } else if (d.type == kernel::cdeform_bend_curve) {
            // Both terms the geometry actually charges: the curvature
            // compression on the inside of the tightest bend, and the rescale
            // from laying the item's span onto the guide's arc length. The
            // bend radius is measured the same way the sweep measures it —
            // by circumradius, which tessellation density cannot fool.
            std::vector<StrokePoint> tess =
                curve_is_polyline(d.guide, false)
                    ? d.guide
                    : tessellate_curve(d.guide, false, item.curve_tolerance);
            if (tess.size() >= 2) {
                float cross = 0.0f;
                if (!local.empty()) {
                    for (int i = 0; i < 4; ++i) {
                        float y = (i & 1) ? local.max.y : local.min.y;
                        float z = (i & 2) ? local.max.z : local.min.z;
                        cross = kernel::cmax(cross, kernel::clength(kernel::cf2(y, z)));
                    }
                }
                info = kernel::cfi_bend_curve(info, cross, guide_bend_radius(tess),
                                              kernel::cabs(d.c - d.b), guide_arc_length(tess));
            }
        } else if (d.type == kernel::cdeform_taper) {
            float s_min = kernel::cmin(d.b, d.c);
            float s_max = kernel::cmax(d.b, d.c);
            float height = kernel::cmax(kernel::cabs(d.a - d.k), 1e-6f);
            info = kernel::cfi_taper(info, s_min, s_max, height, radius);
        } else if (d.type == kernel::cdeform_grab) {
            float len = kernel::clength(kernel::cf3(d.ext[0], d.ext[1], d.ext[2]));
            info = kernel::cfi_grab(info, len, d.c, ease_max_slope(d.ease), d.ext[3] != 0.0f);
        } else if (d.type == kernel::cdeform_pose_line) {
            kernel::cfloat3 anchor = cf3(d.k, d.a, d.b);
            kernel::cfloat3 end = cf3(d.c, d.ext[0], d.ext[1]);
            kernel::cfloat3 axis = cf3(d.ext[2], d.ext[3], d.ext[4]);
            float axis_len = kernel::cmax(kernel::clength(axis), 1e-9f);
            kernel::cfloat3 unit = axis * (1.0f / axis_len);
            float extent = 0.0f;
            if (!local.empty()) {
                for (int i = 0; i < 8; ++i) {
                    kernel::cfloat3 corner = cf3((i & 1) ? local.max.x : local.min.x,
                                                 (i & 2) ? local.max.y : local.min.y,
                                                 (i & 4) ? local.max.z : local.min.z);
                    kernel::cfloat3 v = corner - anchor;
                    kernel::cfloat3 along = unit * kernel::cdot(unit, v);
                    extent = kernel::cmax(extent, kernel::clength(v - along));
                }
            }
            info = kernel::cfi_pose_line(info, d.ext[5], extent,
                                         kernel::clength(end - anchor), ease_max_slope(d.ease));
        } else if (d.type == kernel::cdeform_blob) {
            // The noise bound plus the region's own gradient — see cfi_blob.
            info = kernel::cfi_blob(info, d.ext[0], d.ext[1], static_cast<int>(d.ext[2]),
                                    d.ext[3], d.c, ease_max_slope(d.ease));
        } else if (d.type == kernel::cdeform_alpha) {
            // The stamp's own steepness in world units, plus the region's
            // gradient — see cfi_alpha. `world_slope` is derived from adjacent
            // sample DIFFERENCES, so a flat stamp costs nothing however large
            // its values are.
            info = kernel::cfi_alpha(info, d.stamp.amplitude, d.stamp.world_slope(), d.stamp.peak,
                                     d.stamp.radius, ease_max_slope(d.ease));
        } else if (d.type == kernel::cdeform_noise) {
            info = kernel::cfi_noise(info, d.k, d.a, static_cast<int>(d.b), d.c);
        } else if (d.type == kernel::cdeform_magnify) {
            info = kernel::cfi_magnify(info, d.ext[0], ease_max_slope(d.ease));
        } else if (d.type == kernel::cdeform_pose) {
            info = kernel::cfi_pose(info, d.ext[3], ease_max_slope(d.ease));
        } else if (d.type == kernel::cdeform_bend_linear) {
            // slope = |v| over the span it ramps across
            float seg = kernel::clength(kernel::cf3(d.c - d.k, d.ext[0] - d.a, d.ext[1] - d.b));
            float v_len = kernel::clength(kernel::cf3(d.ext[2], d.ext[3], d.ext[4]));
            info = kernel::cfi_bend_linear(info, v_len, seg);
        } else if (d.type == kernel::cdeform_bend_radial) {
            info = kernel::cfi_bend_radial(info, d.b, d.k, d.a);
        } else if (d.type == kernel::cdeform_wrap) {
            // Stretch grows as the content sits further from the wrap radius,
            // so the content's own radial extent bounds it — the same
            // convention twist and bend use.
            float thickness = local.empty()
                                  ? 0.0f
                                  : kernel::cmax(kernel::cabs(local.min.y),
                                                 kernel::cabs(local.max.y));
            info = kernel::cfi_wrap_around(info, d.k, d.a, thickness);
        } else if (d.type == kernel::cdeform_displace) {
            info = kernel::cfi_displace(info, kernel::cabs(d.k) * kernel::cabs(d.a) * 1.7320508f);
        }
        ChainLink link;
        link.additive = deformer_is_offset(d.type);
        link.value = link.additive ? kernel::cmax(info.lipschitz - 1.0f, 0.0f)
                                   : kernel::cmax(info.lipschitz, 1.0f);
        link.support = link_support(d);
        links.push_back(link);
    }
    return links;
}

// Fold the chain with only `active` links participating, in chain order.
float fold_chain(const std::vector<ChainLink>& links, const std::vector<bool>& active) {
    float l = 1.0f;
    for (std::size_t i = 0; i < links.size(); ++i) {
        if (!active[i]) continue;
        l = links[i].additive ? l + links[i].value : l * links[i].value;
    }
    return l;
}

// Which finite-support links can reach one another, as a 1-based group number
// per link (0 for a link that acts everywhere and is therefore in every group).
// Two links share a group when their balls are closer than the distance the
// chain can carry a point between them, so a group is a set that MIGHT
// compound and two groups provably cannot.
//
// O(n^2) over a chain of tens of links, and the arithmetic is a distance
// compare — against `chain_links`, which samples an easing curve 512 times per
// link, this does not show up.
std::vector<std::size_t> group_by_reach(const std::vector<ChainLink>& links,
                                        std::size_t* out_groups) {
    // Summed over the WHOLE chain rather than over the links between the two
    // being compared: looser, in the safe direction, and it saves having to
    // reason about which links sit between them.
    float travel = 0.0f;
    for (const ChainLink& l : links) travel += l.support.travel;

    const std::size_t n = links.size();
    std::vector<std::size_t> group(n, 0);
    std::size_t groups = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!links[i].support.finite) continue;
        group[i] = ++groups;
        for (std::size_t k = 0; k < i; ++k) {
            if (!links[k].support.finite || group[k] == group[i]) continue;
            const float gap = kernel::clength(links[i].support.centre - links[k].support.centre);
            if (gap > links[i].support.radius + links[k].support.radius + travel) continue;
            // They can meet: fold i's group into k's, and everything already
            // gathered into i's along with it.
            const std::size_t from = group[i], into = group[k];
            for (std::size_t j = 0; j <= i; ++j)
                if (group[j] == from) group[j] = into;
        }
    }
    *out_groups = groups;
    return group;
}

}  // namespace

float deformer_lipschitz(const Node& item) {
    if (item.deformers.empty()) return 1.0f;
    const std::vector<ChainLink> links = chain_links(item);
    const std::size_t n = links.size();
    std::size_t groups = 0;
    const std::vector<std::size_t> group = group_by_reach(links, &groups);

    // The bound over the whole domain is the WORST place in it. A link with
    // unbounded support acts everywhere and is always in; the finite ones only
    // compound within their own group, so each group is priced with the other
    // groups absent and the largest answer wins.
    //
    // Group 0 first, which is the chain with every finite link out — the answer
    // for the part of the item no brush has touched, and the whole answer when
    // there are no finite links to gather.
    std::vector<bool> active(n);
    float worst = 1.0f;
    for (std::size_t g = 0; g <= groups; ++g) {
        for (std::size_t i = 0; i < n; ++i)
            active[i] = !links[i].support.finite || group[i] == g;
        worst = kernel::cmax(worst, fold_chain(links, active));
    }
    return worst;
}

// Local bound of a repeated item: the finite grid sweeps the item across its
// occupied cells, a radial array sweeps it into an annulus. An infinite grid
// has no finite bound — the caller turns that into infinite influence.
Aabb repeated_local_bounds(const Aabb& local, const Repeat& r) {
    if (!r.active() || local.empty()) return local;
    if (r.type == kernel::crepeat_grid_finite) {
        kernel::cfloat3 reach = kernel::cf3(r.spacing.x * r.counts.x, r.spacing.y * r.counts.y,
                                            r.spacing.z * r.counts.z);
        return Aabb{local.min - reach, local.max + reach};
    }
    if (r.type == kernel::crepeat_radial) {
        // copies sit at radius |offset| +/- the item's own reach, all around Y
        float reach = 0.0f;
        for (int i = 0; i < 4; ++i) {
            float x = (i & 1) ? local.max.x : local.min.x;
            float z = (i & 2) ? local.max.z : local.min.z;
            reach = kernel::cmax(reach, kernel::clength(kernel::cf2(x, z)));
        }
        float radius = kernel::cabs(r.spacing.y) + reach;
        return Aabb{cf3(-radius, local.min.y, -radius), cf3(radius, local.max.y, radius)};
    }
    return local;  // infinite grid: handled by the caller
}

Aabb item_local_bounds(const Node& item) {
    Aabb local = prim_local_bounds(item);
    if (!item.deformers.empty())
        local = deformed_local_bounds(local, item.deformers, item.curve_tolerance);
    if (item.repeat.active()) local = repeated_local_bounds(local, item.repeat);
    return local;
}

namespace {

// The geometry bound, with or without the copies the layer's symmetry emits.
// One body for both readings so the dilations cannot drift apart: the
// every-copy bound is what culling and invalidation consult, the item-alone
// bound is what a brush that has already reflected ITSELF tests against
// (brush/move.cpp, drag_images).
// `layer_m` (optional) is `layer_matrix(layer)`, precomputed. It is the same
// matrix for every item in a layer and building it is 7.4 ns of this function's
// 45.3 -- so a caller looping over a layer's items, which is what
// `layer_influence_extent` is, was rebuilding it once per item for no reason.
Aabb geometry_bound(const Node& item, const Layer& layer, bool with_copies,
                    const math::cfloat4x4* layer_m = nullptr) {
    Aabb local = item_local_bounds(item);
    if (local.empty()) return local;
    const math::cfloat4x4 lm = layer_m ? *layer_m : layer_matrix(layer);

    // Each PER-AXIS scale is innermost at its own level, so the item's
    // multiplies the local box before its placement does and the layer's
    // multiplies the result before the layer's placement does. A bound that
    // missed either would be tight around a shape the item no longer is, and
    // the cull would drop a squashed cylinder that is on screen.
    const math::cfloat4x4 axes = math::scale_matrix(item.scale_axes);
    Aabb bound = local.transformed(math::mul(lm, item_matrix(item)));
    if (with_copies && item.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            // The copy's map, on the order emit_item uses: the reflection acts
            // in the layer's LOCAL space, so the layer's per-axis scale is
            // outside it.
            math::cfloat4x4 m = math::mul(
                lm, math::mul(math::reflection_matrix(axis), math::mul(item.xform.matrix(), axes)));
            bound.expand(local.transformed(m));
        }
        bound = bound.dilated(kernel::csmin_quadratic_support(layer.mirror_k));
    }
    // Every copy the radial mode emits, for the same reason: a bound that
    // misses a copy lets the cull drop an item that is on screen. A rotated box
    // is not axis-aligned, so each copy contributes the AABB OF the rotated box
    // — it over-covers, which costs cull precision and never correctness.
    if (with_copies && item.mirror && layer.radial_count > 1) {
        const int axis = layer.radial_axis < 3 ? layer.radial_axis : 1;
        const int count = static_cast<int>(layer.radial_count);
        for (int k = 1; k < count; ++k) {
            const float angle =
                6.2831853071795864769f * static_cast<float>(k) / static_cast<float>(count);
            math::cfloat4x4 m =
                math::mul(lm, math::mul(math::rotation_matrix(axis, angle),
                                        math::mul(item.xform.matrix(), axes)));
            bound.expand(local.transformed(m));
        }
        bound = bound.dilated(kernel::csmin_quadratic_support(layer.radial_k));
    }
    // Rounding is authored in item-local units (tape emits round*scale);
    // erosion (negative rounding) shrinks the surface, never the bound.
    // Paint fades over max(profile support, k). Extended modes deviate
    // within their documented support of the item surface (kernel/tape.h) —
    // for groove/tongue that is the rounding again (rb), on top of the
    // rounding dilation the item field already carries.
    // Rounding is authored in item-local units and the tape converts it by the
    // same factor it multiplies the distance by, so the bound has to use that
    // factor too rather than the uniform scale alone.
    float round_world = item.rounding * placed_distance_scale(layer, item);
    float combine = op_is_extended(item.op)
                        ? kernel::ccombine_extended_support(static_cast<int>(item.op),
                                                            item.blend.k, round_world)
                        : kernel::cmax(item.blend.support(), item.blend.k);
    return bound.dilated(kernel::cmax(round_world, 0.0f) + combine);
}

}  // namespace

Aabb item_geometry_bound(const Node& item, const Layer& layer) {
    return geometry_bound(item, layer, /*with_copies=*/true);
}

bool item_is_feathered_replace(const Node& item) {
    return item.op == Op::Replace && prim_is_volume(item.prim.type) && item.volume &&
           item.volume->feather() > 0.0f;
}

// The feathered combine moves the accumulated value by up to the volume's
// band (the kernel clamps the correction there), so an item whose field the
// caller's dilation put just beyond the clamp can still steer a value back
// inside it. Testing against the region dilated by that band restores the
// contract the CullRegion states: band-clamped results identical to the full
// tape.
float feather_cull_pad(const SdfContent& content, const Layer& layer) {
    float pad = 0.0f;
    for (const auto& [id, n] : content.nodes()) {
        (void)id;
        if (n.is_group || !n.visible) continue;
        if (!item_is_feathered_replace(n)) continue;
        pad = kernel::cmax(pad, n.volume->band() * placed_distance_scale(layer, n));
    }
    return pad;
}

// The measured envelope (#335): pad-in-k-multiples = base + slope *
// log2(N / 75), clamped below at its N = 75 value, clamped by the CALLER at
// the profile's support. The coefficients are a fit over the sweep
// blend_cull_pad's definition records, holding at least 0.5k above every
// measured knee — the drift the knees show across brick-seed draws — at every
// measured (profile, N):
//
//     profile    fit                        worst knee (k-multiples)
//     quadratic  2.80 + 0.35 * log2(N/75)   2.30@75 .. 3.90@5000
//     cubic      2.75 + 0.50 * log2(N/75)   2.15@75 .. 4.85@5000 (x86-64)
//     circular   2.70 + 0.30 * log2(N/75)   2.05@75 .. 3.45@2400
//
// The support clamp takes over where the product crosses it: N ~ 808 for
// quadratic (4k), N ~ 6788 for cubic (6k), N ~ 391 for circular (~3.41k) —
// beyond that the pad IS the support, the pre-#335 value.
namespace {
// The step the envelope is held still by, in the k-multiples the envelope is
// expressed in.
//
// Chosen so ONE STROKE crosses at most one boundary. The fit grows fastest
// just above 75 nodes -- d(envelope)/dn is slope / (n ln2), which for
// quadratic at n = 76 is 0.0067 a node, or 0.16 across a 24-dab stroke. A
// quantum at or above that means a stroke pays a step at most once, and the
// dabs either side of it are answered from their stored values.
//
// The cost of a coarser step is a wider pad: the resolved value can exceed the
// fit by up to this much times k, which keeps items in a brick's culled tape
// that the fit would have dropped. 0.25 spends about 9% of the pad at the top
// of the band to buy back a cache that was returning nothing, and the
// overshoot vanishes entirely once the support clamp takes over.
//
// Across the whole quadratic band (2.80 at 75 nodes to the 4.0 clamp at 808)
// that is five steps, at roughly 76, 123, 202, 331 and 544 nodes -- five full
// refills over an entire blockout, against one per dab.
constexpr float kEnvelopeQuantum = 0.25f;

struct EnvelopeFit {
    float base;
    float slope;
};

bool envelope_fit_for(BlendProfile profile, EnvelopeFit* fit) {
    switch (profile) {
        case BlendProfile::Quadratic: *fit = {2.80f, 0.35f}; return true;
        case BlendProfile::Cubic: *fit = {2.75f, 0.50f}; return true;
        case BlendProfile::Circular: *fit = {2.70f, 0.30f}; return true;
        default: return false;  // hard drags nothing; chamfer's support is k
    }
}

// One measured profile's resolved pad: min(support, k * envelope(N)). Support
// is linear in k for a fixed profile, so this is monotone in k and the
// per-profile maximum k resolves to the layer's largest per-node pad exactly.
float profile_chain_pad(BlendProfile profile, float k, std::size_t nodes) {
    if (k <= 0.0f) return 0.0f;
    return kernel::cmin(kernel::ctape_blend_support(static_cast<int>(profile), k),
                        k * chain_pad_envelope(profile, nodes));
}
}  // namespace

float chain_pad_envelope(BlendProfile profile, std::size_t nodes) {
    EnvelopeFit fit{0.0f, 0.0f};
    if (!envelope_fit_for(profile, &fit)) return 0.0f;
    if (nodes <= 75) return fit.base;
    // QUANTISED, and this is not a detail of the fit -- it is what makes the
    // pad usable as a cache key.
    //
    // A brick's stored value is keyed on the pad by exact equality
    // (bindings/c/clay_c.cpp, seed_for). The raw fit changes on EVERY node
    // added, so every dab invalidated every seed and the brick resume was not
    // degraded but DEAD, for every document between 76 nodes and wherever the
    // support clamp took over -- 808 for quadratic. Measured on the reference
    // iPad, a 24-dab stroke on a smooth-blended document: 0.144 ms a dab at 10
    // stamps, 4.435 at 300, 4.079 at 800, 0.175 at 2000, with bricks resumed
    // per dab going 38.5, ZERO, 26.0, 38.5. A dab in the middle of a blockout
    // cost 31x a dab at the start and cleared the whole frame share.
    //
    // Rounded UP, never down. The pad is conservative: a larger one keeps MORE
    // items in a brick's culled tape, and a band-clamped result cannot be
    // changed by keeping an item that could not have changed it. Rounding down
    // would drop items the fit says are needed, which is a wrong field rather
    // than a slow one. `std::ceil` is what makes the resolved value >= the fit
    // at every node count, which is the property the tests pin.
    //
    // The step is taken on the GROWTH above the base rather than on the whole
    // envelope, so the value at and below 75 nodes is exactly what it was and
    // no existing measurement moves for a document that never enters the band.
    const float growth = fit.slope * std::log2(static_cast<float>(nodes) / 75.0f);
    return fit.base + kEnvelopeQuantum * std::ceil(growth / kEnvelopeQuantum);
}

// How many instances of a mirrored item this layer's symmetry emits. The
// modes compose ADDITIVELY — emit_item (tape_build.cpp) copies the BASE item
// once per set mirror axis and radial_count - 1 times around the axis, and
// never emits mirror-of-rotation products — so the total is a sum, not a
// product. Deliberately CONSERVATIVE as a chain-length multiplier: it also
// counts groups, invisible nodes and items that opted out of the mirror,
// which only raises the envelope, and every term blend_total resolves stays
// clamped at its own support, so the pad never exceeds the pre-#335 one.
std::size_t layer_symmetry_multiplicity(const Layer& layer) {
    std::size_t m = 1 + static_cast<std::size_t>(std::popcount(
                            static_cast<unsigned>(layer.mirror_axes & 0x7u)));
    if (layer.radial_count > 1) m += static_cast<std::size_t>(layer.radial_count) - 1;
    return m;
}

float CullPadTerms::blend_total(std::size_t n_eff) const {
    float pad = blend_fixed;
    pad = kernel::cmax(pad, profile_chain_pad(BlendProfile::Quadratic, blend_k_quadratic, n_eff));
    pad = kernel::cmax(pad, profile_chain_pad(BlendProfile::Cubic, blend_k_cubic, n_eff));
    pad = kernel::cmax(pad, profile_chain_pad(BlendProfile::Circular, blend_k_circular, n_eff));
    if (blend_k_seam <= 0.0f) return pad;
    // The SEAM term: every symmetry copy enters the chain through a quadratic
    // blend with the LAYER's seam k (tape_build.cpp emit_item), independent of
    // any item k, so it drags exactly as a quadratic item of that k would —
    // measured: item k = 0.04 under a 0.12 seam kneed at 5.0 ITEM-k, right
    // where the seam's own envelope sits. Clamped at the CEILING the item
    // maxima alone resolve to, which IS the pre-#335 pad (that pad never saw
    // a seam k either): each item term is <= its own support <= the ceiling,
    // so the whole stays <= the pre-#335 pad everywhere, and where the seam
    // demands more than that pad ever granted the cull is identical to it.
    float ceiling = blend_fixed;
    ceiling = kernel::cmax(ceiling, kernel::ctape_blend_support(
                                        static_cast<int>(BlendProfile::Quadratic),
                                        blend_k_quadratic));
    ceiling = kernel::cmax(
        ceiling, kernel::ctape_blend_support(static_cast<int>(BlendProfile::Cubic), blend_k_cubic));
    ceiling = kernel::cmax(ceiling, kernel::ctape_blend_support(
                                        static_cast<int>(BlendProfile::Circular),
                                        blend_k_circular));
    const float seam = profile_chain_pad(BlendProfile::Quadratic, blend_k_seam, n_eff);
    return kernel::cmax(pad, kernel::cmin(seam, ceiling));
}

namespace {
// One node's blend contribution, into the raw-maxima form. The ONE definition
// of the reach: chain_drag_reach and both cull_pad_terms walks are written in
// terms of it, so a new dragging combine cannot reach the pad through one and
// not the others.
void raise_blend_term(const Node& item, CullPadTerms* t) {
    // Paint and the extended modes keep the FULL reach whatever the profile:
    // it is for the mix/colour channel, whose weight fades over all of it.
    if (item.op == Op::Paint || op_is_extended(item.op)) {
        t->blend_fixed = kernel::cmax(t->blend_fixed,
                                      kernel::cmax(item.blend.support(), item.blend.k));
        return;
    }
    EnvelopeFit fit{0.0f, 0.0f};
    if (envelope_fit_for(item.blend.profile, &fit)) {
        float* slot = item.blend.profile == BlendProfile::Quadratic ? &t->blend_k_quadratic
                      : item.blend.profile == BlendProfile::Cubic   ? &t->blend_k_cubic
                                                                    : &t->blend_k_circular;
        *slot = kernel::cmax(*slot, item.blend.k);
        return;
    }
    // Hard drags nothing — support is zero and so is this. Chamfer's support
    // IS its k, below any envelope value, so the clamp binds: keep support.
    t->blend_fixed = kernel::cmax(t->blend_fixed, item.blend.support());
}
}  // namespace

// See bounds.h, including why this is not the reach a node's own bound uses.
float chain_drag_reach(const Node& item, std::size_t effective_nodes) {
    CullPadTerms t;
    raise_blend_term(item, &t);
    return t.blend_total(effective_nodes);
}

// The pad a SMOOTH-UNION CHAIN needs beyond the caller's band, for the same
// reason feather_cull_pad exists and by the same mechanism.
//
// An item's own influence bound is dilated by max(support, k), which is exactly
// how far that item can move a value it is blended against. That is the right
// bound for ONE blend and the wrong one for a chain: the accumulated field part
// way down a long chain sits well ABOVE where it ends up, so an item whose
// final contribution is nothing can still be within k of the RUNNING value and
// change it. Measured on a 600-dab sphere at k=0.06, the chain drags the field
// 0.26 below the base sphere's own distance -- more than four times k -- and
// culling at the band alone left samples INSIDE the band differing from the
// full tape by up to 0.009, which is half a cell at the resolution that
// document bakes at.
//
// Hard unions do not have it: min() is exact and associative, and the same
// documents measure zero disagreements at any chain length.
//
// The pad is the largest min(support, k * envelope(N)) in the layer, where N
// is the layer's EFFECTIVE contributor count — node-map size times
// layer_symmetry_multiplicity — and the envelope is the per-profile fit above
// (chain_pad_envelope). NO FIXED K-MULTIPLE SUFFICES: the sufficient pad
// grows with chain length, and a sweep of 700+ synthesized configs -- chain
// lengths 75 to 8000, k from 0.03 to 0.24, quadratic/cubic/circular profiles,
// stroke/shuffled/reversed and constructed descending-ladder chain orders,
// mixed hard/smooth and smooth-subtract compositions, three brick-seed draws
// per config, 200 bricks and up to 160,000 in-band samples per config, on
// arm64 and x86-64 -- measured the minimal sufficient quadratic pad, worst
// order per length, climbing from 2.30k at 75 nodes through 3.05k at 600,
// 3.45k at 1200 and 3.90k at 5000 (cubic: to 4.85k; circular tops out at its
// ~3.41k support by 2400). A fixed 3k cap left 3-26 in-band band-clamped
// disagreements per config at 2000-5000 nodes, the worst 9.75e-4 -- 14x the
// fp16 quantization the brick cache stores through (about 7e-5 at band 0.15)
// and the magnitude of one real contributor entering the chain, not last-ULP
// dust. Thresholds cluster in k-units ACROSS profiles because the drag each
// step adds is normalized to k, while support only shapes the blend's fringe
// -- which is why the envelope is a k-multiple and not a support fraction,
// and why cubic's 6k support buys the most back.
//
// N COUNTS SYMMETRY COPIES. emit_item compiles a mirrored item once per copy
// the layer's mirror and radial modes emit -- 1 + popcount(mirror_axes) +
// (radial_count - 1) instances, each folded into the layer's ONE serial
// chain through its own seam combine -- so a 75-node map under radial_count
// 64 is a ~4800-contributor chain, and resolving the envelope against the
// map size alone left it floored at 2.75-2.80k where the measured need was
// ~4k: 15 configs at radial 8-64 with dabs confined to one sector measured
// real in-band disagreements, the worst 1.9e-3 = 27x the fp16 floor, against
// zero for the pre-#335 pad. A second knee campaign over the amplified
// family (radial 4-64, mirror 1-3 axes, combined, N 75-600, both salts)
// measured the amplified knees matching plain chains of the same effective
// length -- additive composition validated on a combined radial-and-mirror
// config -- and envelope(N_eff) clears every one of its 49 unclamped knees
// by at least 0.89k (the sub-0.5k margins were all seam-k configs, which the
// seam term in blend_total closes; item k = 0.04 under a 3x seam k kneed at
// 5.0 item-k, right where the seam's own quadratic envelope sits).
//
// The support clamp is what bounds the story: where k * envelope(N) crosses
// the profile's support the pad IS `max(support, k)` for a smooth blend --
// the pre-#335 pad -- so the envelope never culls wider than that pad did,
// and where the clamp binds it culls identically. The correctness bar is
// RELATIVE for the same reason (#339): the shipped `max(support, k)` itself
// measures a handful of non-zero configs at k = 0.03 past 2400 nodes, so
// what is held is equal-or-fewer disagreements than it, per config -- which
// the envelope fit meets at every one of the sweep's 710 grid points, with
// at least 0.5k of margin (the knees' drift across seed draws) below the
// clamp. It is not a proof for an arbitrary chain -- no fixed dilation is,
// since the drag grows with length -- and tape.h says so rather than
// implying otherwise.
float blend_cull_pad(const SdfContent& content, const Layer& layer) {
    return cull_pad_terms(content, layer)
        .blend_total(content.nodes().size() * layer_symmetry_multiplicity(layer));
}

// ONE node's contribution to both pads, and the ONE definition of either: the
// walks below are folds of this, so a new feathered shape or a new dragging
// combine cannot reach the pad through one of them and not the other. The
// blend half stays RAW (largest k per profile) here -- bounds.h records why
// resolving it against the node count must wait until read time.
CullPadTerms cull_pad_terms(const Node& n, const Layer& layer) {
    CullPadTerms t;
    if (!n.visible) return t;
    const bool feathered = !n.is_group && item_is_feathered_replace(n);
    if (feathered)
        t.feather = n.volume->band() * placed_distance_scale(layer, n);
    raise_blend_term(n, &t);
    // The SEAM blends this node's symmetry copies enter the chain through.
    // Exactly the nodes emit_item copies: items participating in the mirror,
    // minus feathered replaces, which skip both symmetry blocks. A zero seam
    // k is a HARD seam and drags nothing — its copies still lengthen the
    // chain, which the multiplicity counts.
    if (!n.is_group && n.mirror && !feathered) {
        if (layer.mirror_axes != 0) t.blend_k_seam = kernel::cmax(t.blend_k_seam, layer.mirror_k);
        if (layer.radial_count > 1) t.blend_k_seam = kernel::cmax(t.blend_k_seam, layer.radial_k);
    }
    return t;
}

// Both pads in ONE walk of the node map. Worth its own function rather than
// adding the two: each of them walks every node in the layer, the compiler asks
// for the total on every uncached compile, and at ten thousand items that
// second walk measured 20-30% on the per-brick cull benchmarks.
CullPadTerms cull_pad_terms(const SdfContent& content, const Layer& layer) {
    CullPadTerms total;
    for (const auto& [id, n] : content.nodes()) {
        (void)id;
        total.raise(cull_pad_terms(n, layer));
    }
    return total;
}

float cull_pad(const SdfContent& content, const Layer& layer) {
    return cull_pad_terms(content, layer)
        .total(content.nodes().size() * layer_symmetry_multiplicity(layer));
}

bool item_influence_is_local(const Node& item) {
    // Non-local ops (intersect, the spatial morphs) change the field
    // arbitrarily far from the item: claiming a finite bound would let
    // per-brick culling drop them and silently corrupt the result.
    if (!op_is_local(item.op)) return false;
    // an infinite grid produces copies arbitrarily far away
    if (item.repeat.is_infinite_grid()) return false;
    // so do primitives with no finite extent (plane, infinite cylinder)
    if (prim_is_unbounded(item.prim.type)) return false;
    return true;
}

Nonlocality item_nonlocality(const Node& item) {
    // Order matters: an intersect that ALSO repeats infinitely is unbounded,
    // and the weaker answer must not win.
    if (item.repeat.is_infinite_grid()) return Nonlocality::Unbounded;
    if (prim_is_unbounded(item.prim.type)) return Nonlocality::Unbounded;
    // THE SPLIT THIS CHANGE IS ABOUT (#319, measured in #326).
    //
    // A SPATIAL MORPH is unbounded. Its weight saturates:
    // `ctransition_radial_weight` is `clamp((length(p.xz) - r0) / (r1 - r0), 0, 1)`
    // about the WORLD Y axis, so past `r1` the weight is exactly 1 and the
    // result IS the item's own field -- arbitrarily far from anything the
    // layer occupies. Measured: over 400,000 sample points a radial morph
    // leaves 0.0157 of band-clamped drift outside the layer's extent dilated
    // by the band, and a linear morph 0.000568. The layer is not a bound for
    // these.
    //
    // An INTERSECT is bounded by its LAYER. `max(acc, item)` can only take
    // material away, and what it takes away is inside what the layer already
    // occupies -- it cannot put material where the layer has none. Measured
    // over the same 400,000 points on two fixtures: drift exactly 0 outside
    // the layer's extent, against 0.100 and 0.065 outside the item's OWN
    // geometry, which is the bound that looks tighter and does not hold.
    if (item.op == Op::Intersect) return Nonlocality::BoundedByLayer;
    if (!op_is_local(item.op)) return Nonlocality::Unbounded;
    return Nonlocality::None;
}

Aabb layer_influence_extent(const SdfContent& content, const Layer& layer) {
    // Every visible item's geometry, and infinite the moment one of them has
    // none: an intersect in a layer holding a plane is bounded by a plane.
    Aabb out;
    // ONCE for the whole walk. It is the same matrix for every item and it was
    // being rebuilt per item: 7.4 ns of the 45.3 each bound costs, on a walk
    // that runs per edit for every layer holding an intersect (#451).
    const math::cfloat4x4 lm = layer_matrix(layer);
    for (const auto& [id, n] : content.nodes()) {
        (void)id;
        if (n.is_group || !n.visible) continue;
        if (!item_influence_is_local(n)) {
            // A second non-local item bounds this one only if IT is bounded.
            if (item_nonlocality(n) != Nonlocality::BoundedByLayer) return Aabb::infinite();
        }
        out.expand(geometry_bound(n, layer, /*with_copies=*/true, &lm));
    }
    return out;
}

Aabb item_influence_bound(const Node& item, const Layer& layer, LayerExtent* extent) {
    switch (item_nonlocality(item)) {
        case Nonlocality::None:
            return item_geometry_bound(item, layer);
        case Nonlocality::BoundedByLayer: {
            // Computed here when the caller did not already hold it. Only an
            // intersect reaches this, so a layer without one pays nothing --
            // which is why this is a lazy fallback rather than a parameter
            // every caller has to thread. A caller that meets several
            // intersects hands in a LayerExtent and pays for the walk once
            // (#451).
            if (!layer.sdf) return Aabb::infinite();
            if (extent) return extent->of(*layer.sdf, layer);
            return layer_influence_extent(*layer.sdf, layer);
        }
        case Nonlocality::Unbounded:
            break;
    }
    return Aabb::infinite();
}

Aabb item_own_influence_bound(const Node& item, const Layer& layer) {
    // NOT given the layer fallback, deliberately. This one answers "how far
    // does this item's own body reach", which is what a brush that has already
    // reflected itself tests a drag against -- and for a non-local item the
    // honest answer there is still "everywhere", because the brush is asking
    // whether to warp it at all rather than which bricks to redraw.
    if (!item_influence_is_local(item)) return Aabb::infinite();
    return geometry_bound(item, layer, /*with_copies=*/false);
}

float group_blend_support(const Node& group, const Layer& layer) {
    // Extended-op groups: the subtree field is not rounded, so rb comes
    // straight from the group's rounding scaled into world units.
    // Otherwise cmax(support, k), exactly as the item path above: paint fades
    // over max(profile support, k), and a HARD profile has zero support — so
    // support alone dilated a Paint group by nothing while its colour reached
    // out to k.
    //
    // Lifted out of node_influence_bound so the ancestor walk below can apply
    // the SAME expression to one child that the group path applies to the
    // union of them. Two spellings of "how far a group's blend reaches" would
    // be one refactor away from disagreeing, and the walk is only sound
    // because it is the same number.
    return op_is_extended(group.op)
               ? kernel::ccombine_extended_support(static_cast<int>(group.op), group.blend.k,
                                                   group.rounding * layer_distance_scale(layer))
               : kernel::cmax(group.blend.support(), group.blend.k);
}

Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer,
                          LayerExtent* extent) {
    const Node* n = content.find(id);
    if (!n || !n->visible) return Aabb{};
    if (!n->is_group) return item_influence_bound(*n, layer, extent);
    // A GROUP takes the same split its items take: an intersecting group reads
    // the running accumulator and can move the result anywhere the LAYER has
    // material, and no further.
    if (!op_is_local(n->op)) {
        if (n->op != Op::Intersect || !layer.sdf) return Aabb::infinite();
        return extent ? extent->of(*layer.sdf, layer) : layer_influence_extent(*layer.sdf, layer);
    }
    Aabb b;
    for (NodeId c : n->children) {
        Aabb cb = node_influence_bound(content, c, layer, extent);
        if (cb.is_infinite()) return Aabb::infinite();
        b.expand(cb);
    }
    return b.empty() ? b : b.dilated(group_blend_support(*n, layer));
}

Aabb node_reach_bound(const SdfContent& content, NodeId id, const Layer& layer,
                      LayerExtent* extent) {
    // Where an edit to `id` can change the layer's field: the node's own
    // influence bound, then dilated once per enclosing group by that group's
    // blend support.
    //
    // A group's value is a combine over its children, and a blend's SUPPORT is
    // exactly how far changing one operand can move the result — which is why
    // node_influence_bound already applies it to the union of the children.
    // Applying it to ONE child is the same inequality with a smaller left-hand
    // side. Nesting composes: the inner group's output is dilated by the inner
    // support, and that dilated box is what the outer group sees.
    //
    // What this replaces is the root ancestor's whole bound, which was
    // conservative for the right reason and far larger than the reason needs:
    // a sibling's geometry is not something an edit to `id` can reach, so the
    // old answer grew with the size of the GROUP rather than with the size of
    // the edit.
    Aabb b = node_influence_bound(content, id, layer, extent);
    if (b.empty() || b.is_infinite()) return b;

    // Bounded by the node count rather than trusting the tree to be acyclic:
    // SdfContent::move refuses to close a cycle, but `roots` is a public
    // member and this walk must terminate whatever a caller wrote there. The
    // same guard root_ancestor carried, for the same reason.
    NodeId cur = id;
    for (std::size_t step = 0; step <= content.nodes().size(); ++step) {
        NodeId parent = kNoNode;
        int index = -1;
        if (!content.locate(cur, &parent, &index)) return Aabb{};
        if (parent == kNoNode) return b;
        const Node* g = content.find(parent);
        if (!g) return Aabb{};
        // A hidden group contributes nothing at all, so an edit inside one
        // cannot change the field: node_influence_bound says the same about a
        // hidden node and this must agree with it.
        if (!g->visible) return Aabb{};
        // The non-local check at EVERY level, not only the first. An
        // intersect anywhere above reads the running accumulator and can move
        // the result as far as the LAYER reaches, which is what
        // node_influence_bound already reports for the group itself; a spatial
        // morph above can move it further than that and still reports
        // infinite.
        if (!op_is_local(g->op)) {
            if (g->op != Op::Intersect || !layer.sdf) return Aabb::infinite();
            return extent ? extent->of(*layer.sdf, layer)
                          : layer_influence_extent(*layer.sdf, layer);
        }
        b = b.dilated(group_blend_support(*g, layer));
        cur = parent;
    }
    return Aabb{};
}

Aabb node_influence_bound_in_document(const Document& doc, const SdfContent& content,
                                      NodeId id, LayerExtent* extent) {
    // A node can be in more than one PLACE. instance_layer copies the Layer and
    // shares the SdfContent by shared_ptr, so one node is compiled once per
    // instancing layer, each under that layer's own transform -- and editing it
    // moves every copy.
    //
    // node_influence_bound answers for the layer it is handed, which is the
    // right answer to a different question. Handed to a host as "the box to
    // dirty", it names one copy and leaves the others stale: measured on a
    // two-layer instance, a band-clamped value 0.103 outside the box moved,
    // against a band of 0.15 (issue #325).
    //
    // scene::node_command_bound already unions this way for the undo path. This
    // is the same union, shared so the query, the dirty call and the command
    // path cannot disagree about where an edit reaches.
    // Shared content is the only test, matching node_command_bound. NOT
    // layer.visible: the caller named a node and wants to know where it
    // reaches, and node_influence_bound already returns nothing for a node that
    // is itself invisible. Filtering on the LAYER here made a hidden layer
    // report no bounds where it used to report a box, which is a second
    // behaviour change and not this one.
    Aabb out;
    for (const Layer& l : doc.layers) {
        if (l.sdf.get() != &content) continue;
        const Aabb b = node_influence_bound(content, id, l, extent);
        if (b.is_infinite()) return Aabb::infinite();
        out.expand(b);
    }
    return out;
}

Aabb layer_influence_bound(const Layer& layer, LayerExtent* extent) {
    Aabb b;
    if (!layer.sdf) return b;
    // One walk for the whole root list, not one per intersect among them: the
    // roots share a layer, so they share its extent (#451).
    LayerExtent own;
    if (!extent) extent = &own;
    for (NodeId id : layer.sdf->roots) {
        Aabb nb = node_influence_bound(*layer.sdf, id, layer, extent);
        if (nb.is_infinite()) return Aabb::infinite();
        b.expand(nb);
    }
    return b;
}

}  // namespace scene
}  // namespace clay
