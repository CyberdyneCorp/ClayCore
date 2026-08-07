#include "clay/kernel/ease.h"
#include "clay/scene/bounds.h"

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
Aabb deformed_local_bounds(const Aabb& local, const std::vector<Deformer>& deformers) {
    Aabb b = local;
    for (const Deformer& d : deformers) {
        if (b.empty()) break;
        switch (d.type) {
            case kernel::cdeform_twist: {
                // rotation about Y: |(x,z)| preserved -> hull is the cylinder
                float r = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    float x = (i & 1) ? b.max.x : b.min.x;
                    float z = (i & 2) ? b.max.z : b.min.z;
                    r = kernel::cmax(r, kernel::clength(kernel::cf2(x, z)));
                }
                b = Aabb{cf3(-r, b.min.y, -r), cf3(r, b.max.y, r)};
                break;
            }
            case kernel::cdeform_bend: {
                // rotation of (x,y) about the origin: |(x,y)| preserved
                float r = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    float x = (i & 1) ? b.max.x : b.min.x;
                    float y = (i & 2) ? b.max.y : b.min.y;
                    r = kernel::cmax(r, kernel::clength(kernel::cf2(x, y)));
                }
                b = Aabb{cf3(-r, -r, b.min.z), cf3(r, r, b.max.z)};
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
        if (d.type == kernel::cdeform_elongate && !prim_is_origin_symmetric(item.prim.type))
            return true;
    }
    return false;
}

float deformer_lipschitz(const Node& item) {
    if (item.deformers.empty()) return 1.0f;
    Aabb local = prim_local_bounds(item);
    kernel::CFieldInfo info = kernel::cfi_exact();
    for (const Deformer& d : item.deformers) {
        float radius = 0.0f;
        if (!local.empty()) {
            for (int i = 0; i < 8; ++i) {
                float x = (i & 1) ? local.max.x : local.min.x;
                float y = (i & 2) ? local.max.y : local.min.y;
                float z = (i & 4) ? local.max.z : local.min.z;
                radius = kernel::cmax(radius, d.type == kernel::cdeform_twist
                                                  ? kernel::clength(kernel::cf2(x, z))
                                                  : kernel::clength(kernel::cf2(x, y)));
            }
        }
        if (d.type == kernel::cdeform_twist) {
            info = kernel::cfi_twist(info, d.k, radius);
        } else if (d.type == kernel::cdeform_bend) {
            info = kernel::cfi_bend(info, d.k, radius);
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
    }
    return kernel::cmax(info.lipschitz, 1.0f);
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
    if (!item.deformers.empty()) local = deformed_local_bounds(local, item.deformers);
    if (item.repeat.active()) local = repeated_local_bounds(local, item.repeat);
    return local;
}

Aabb item_geometry_bound(const Node& item, const Layer& layer) {
    Aabb local = item_local_bounds(item);
    if (local.empty()) return local;

    math::Transform world = layer.xform * item.xform;
    Aabb bound = local.transformed(world.matrix());
    if (item.mirror && layer.mirror_axes != 0) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!(layer.mirror_axes & (1u << axis))) continue;
            math::cfloat4x4 m = math::mul(
                layer.xform.matrix(),
                math::mul(math::reflection_matrix(axis), item.xform.matrix()));
            bound.expand(local.transformed(m));
        }
        bound = bound.dilated(kernel::csmin_quadratic_support(layer.mirror_k));
    }
    // Rounding is authored in item-local units (tape emits round*scale);
    // erosion (negative rounding) shrinks the surface, never the bound.
    // Paint fades over max(profile support, k). Extended modes deviate
    // within their documented support of the item surface (kernel/tape.h) —
    // for groove/tongue that is the rounding again (rb), on top of the
    // rounding dilation the item field already carries.
    float round_world = item.rounding * world.scale;
    float combine = op_is_extended(item.op)
                        ? kernel::ccombine_extended_support(static_cast<int>(item.op),
                                                            item.blend.k, round_world)
                        : kernel::cmax(item.blend.support(), item.blend.k);
    return bound.dilated(kernel::cmax(round_world, 0.0f) + combine);
}

Aabb item_influence_bound(const Node& item, const Layer& layer) {
    // Non-local ops (intersect, the spatial morphs) change the field
    // arbitrarily far from the item: claiming a finite bound would let
    // per-brick culling drop them and silently corrupt the result.
    if (!op_is_local(item.op)) return Aabb::infinite();
    // an infinite grid produces copies arbitrarily far away
    if (item.repeat.is_infinite_grid()) return Aabb::infinite();
    // so do primitives with no finite extent (plane, infinite cylinder)
    if (prim_is_unbounded(item.prim.type)) return Aabb::infinite();
    return item_geometry_bound(item, layer);
}

Aabb node_influence_bound(const SdfContent& content, NodeId id, const Layer& layer) {
    const Node* n = content.find(id);
    if (!n || !n->visible) return Aabb{};
    if (!n->is_group) return item_influence_bound(*n, layer);
    if (!op_is_local(n->op)) return Aabb::infinite();
    Aabb b;
    for (NodeId c : n->children) {
        Aabb cb = node_influence_bound(content, c, layer);
        if (cb.is_infinite()) return Aabb::infinite();
        b.expand(cb);
    }
    // Extended-op groups: the subtree field is not rounded, so rb comes
    // straight from the group's rounding scaled into world units.
    float support = op_is_extended(n->op)
                        ? kernel::ccombine_extended_support(
                              static_cast<int>(n->op), n->blend.k,
                              n->rounding * layer.xform.scale)
                        : n->blend.support();
    return b.empty() ? b : b.dilated(support);
}

Aabb layer_influence_bound(const Layer& layer) {
    Aabb b;
    if (!layer.sdf) return b;
    for (NodeId id : layer.sdf->roots) {
        Aabb nb = node_influence_bound(*layer.sdf, id, layer);
        if (nb.is_infinite()) return Aabb::infinite();
        b.expand(nb);
    }
    return b;
}

}  // namespace scene
}  // namespace clay
