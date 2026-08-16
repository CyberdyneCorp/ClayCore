#pragma once

// Scene vocabulary: primitive descriptors, ops, blends, edit nodes.
// A Node is either an edit ITEM (primitive/stroke + op + blend + color) or a
// GROUP (children + group op, including None = children apply inline).
// Ordered edit-list semantics: each item applies to the combined result of
// everything before it (scene-model spec).

#include <cstdint>
#include <vector>

#include <memory>

#include "clay/field/volume.h"
#include "clay/kernel/tape.h"
#include "clay/math/transform.h"

namespace clay {
namespace scene {

using NodeId = std::uint32_t;
using LayerId = std::uint32_t;
inline constexpr NodeId kNoNode = 0;

// 1:1 with the tape primitive opcodes (kernel/tape.h).
enum class PrimType : std::uint8_t {
    Sphere = kernel::ctape_sphere,
    Box = kernel::ctape_box,
    RoundBox = kernel::ctape_round_box,
    BoxFrame = kernel::ctape_box_frame,
    Torus = kernel::ctape_torus,
    Capsule = kernel::ctape_capsule,
    CappedCylinder = kernel::ctape_capped_cylinder,
    RoundedCylinder = kernel::ctape_rounded_cylinder,
    CappedCone = kernel::ctape_capped_cone,
    RoundCone = kernel::ctape_round_cone,
    Ellipsoid = kernel::ctape_ellipsoid,
    Octahedron = kernel::ctape_octahedron,
    HexPrism = kernel::ctape_hex_prism,
    Pyramid = kernel::ctape_pyramid,
    Stroke = kernel::ctape_stroke,
    Extrude = kernel::ctape_extrude,
    Revolve = kernel::ctape_revolve,
    CappedTorus = kernel::ctape_capped_torus,
    Link = kernel::ctape_link,
    CylinderInfinite = kernel::ctape_cylinder_inf,
    Cone = kernel::ctape_cone,
    Plane = kernel::ctape_plane,
    CutSphere = kernel::ctape_cut_sphere,
    CutHollowSphere = kernel::ctape_cut_hollow_sphere,
    SolidAngle = kernel::ctape_solid_angle,
    Tetrahedron = kernel::ctape_tetrahedron,
    Dodecahedron = kernel::ctape_dodecahedron,
    Icosahedron = kernel::ctape_icosahedron,
    TriPrism = kernel::ctape_tri_prism,
    OctahedronCheap = kernel::ctape_octahedron_cheap,
    LNormSphere = kernel::ctape_lnorm_sphere,
    Loft = kernel::ctape_loft,
    Swept = kernel::ctape_swept,
    Volume = kernel::ctape_volume,
    Armature = kernel::ctape_armature,
};

// Primitives with no finite extent: an item using one influences the field
// arbitrarily far away, so (like intersect, the morphs and infinite grids)
// it must never be culled.
inline bool prim_is_unbounded(PrimType t) {
    return t == PrimType::Plane || t == PrimType::CylinderInfinite;
}

// Primitives whose field is a bound rather than a true distance; the tape's
// tracked exactness downgrades for them.
// Elongation's distance correction is derived about the origin, so it is only
// exact for a primitive invariant under p -> -p. The list is deliberately
// conservative: anything not obviously centrally symmetric is treated as
// asymmetric, which costs a step-scale downgrade rather than correctness.
inline bool prim_is_origin_symmetric(PrimType t) {
    return t == PrimType::Sphere || t == PrimType::Box || t == PrimType::RoundBox ||
           t == PrimType::BoxFrame || t == PrimType::Torus ||
           t == PrimType::CappedCylinder || t == PrimType::RoundedCylinder ||
           t == PrimType::Ellipsoid || t == PrimType::Octahedron ||
           t == PrimType::OctahedronCheap || t == PrimType::HexPrism ||
           t == PrimType::Dodecahedron || t == PrimType::Icosahedron ||
           t == PrimType::LNormSphere;
}

inline bool prim_is_bound_field(PrimType t) {
    return t == PrimType::Ellipsoid || t == PrimType::TriPrism ||
           t == PrimType::OctahedronCheap || t == PrimType::LNormSphere ||
           t == PrimType::Loft || t == PrimType::Swept || t == PrimType::Volume;
}

inline bool prim_is_lift(PrimType t) {
    return t == PrimType::Extrude || t == PrimType::Revolve;
}

// A loft carries its profiles in the item's profile LIST rather than in the
// single `profile` field the lifts above use, so no existing document changes
// meaning.
inline bool prim_is_loft(PrimType t) { return t == PrimType::Loft; }

// A sweep carries the SAME profile list a loft does, and its guide is the
// same control-point list a curve item uses — a guide is not a new kind of
// curve, so it gets the point types, handles and tolerance for free.
inline bool prim_is_swept(PrimType t) { return t == PrimType::Swept; }

// A sampled volume. Held by shared reference on the Node, so instancing one
// costs a pointer rather than a copy of its samples.
inline bool prim_is_volume(PrimType t) { return t == PrimType::Volume; }
inline bool prim_carries_profiles(PrimType t) { return prim_is_loft(t) || prim_is_swept(t); }
// The stroke list means control points for both of them, so anything that
// reads or replaces a point list asks this rather than naming Stroke alone.
inline bool prim_carries_curve(PrimType t) { return t == PrimType::Stroke || prim_is_swept(t); }
// An armature is a TREE of spheres. It reuses the stroke's point list — a
// StrokePoint is already a position and a radius — and adds one parent index
// per point, so the two share their storage as well as their kernel segment.
inline bool prim_is_armature(PrimType t) { return t == PrimType::Armature; }

enum class Op : std::uint8_t {
    None = 255,  // groups only: children apply inline to the outer chain
    Add = kernel::ccombine_add,
    Subtract = kernel::ccombine_subtract,
    Intersect = kernel::ccombine_intersect,
    Paint = kernel::ccombine_paint,
    // Extended vocabulary (kernel/tape.h math + color semantics). blend.k is
    // the mode's radius/depth; the blend profile is ignored. Groove/Tongue
    // additionally consume the node's rounding (world units) as the channel
    // half-width rb — the item field itself still gets rounded, so the
    // channel is centered on the rounded surface.
    Groove = kernel::ccombine_groove,
    Tongue = kernel::ccombine_tongue,
    Pipe = kernel::ccombine_pipe,
    Engrave = kernel::ccombine_engrave,
    Emboss = kernel::ccombine_emboss,
    Inset = kernel::ccombine_inset,
    Shell = kernel::ccombine_shell,
    Replace = kernel::ccombine_replace,
    // Surface relief: the item is a REGION, and blend.k is the amplitude by
    // which the surface accumulated BEFORE it moves along its own normal. The
    // node's rounding is the falloff width, the convention groove and tongue
    // already use.
    //
    // A pair rather than one signed amplitude, because blend.k cannot be
    // negative — and because add/subtract and engrave/emboss are pairs too.
    Relief = kernel::ccombine_relief,  // build up
    Incise = kernel::ccombine_incise,  // cut in
    // Spatial morphs (Node::transition carries their parameters). NON-LOCAL:
    // the weight reaches arbitrarily far, so these report infinite influence
    // and are never culled — see op_is_local below.
    TransitionLinear = kernel::ccombine_transition_linear,
    TransitionRadial = kernel::ccombine_transition_radial,
};

inline bool op_is_transition(Op op) {
    return op == Op::TransitionLinear || op == Op::TransitionRadial;
}

// Ops whose influence is bounded by the item's geometry. Intersect and the
// transitions are not: they change the field arbitrarily far away.
inline bool op_is_local(Op op) { return op != Op::Intersect && !op_is_transition(op); }

// Parameters of a spatial morph (kernel/deform.h weights).
struct Transition {
    kernel::cfloat3 a = kernel::cf3(0, -1, 0);  // linear: segment start
    kernel::cfloat3 b = kernel::cf3(0, 1, 0);   // linear: segment end
    float r0 = 0.0f;                            // radial: inner radius
    float r1 = 1.0f;                            // radial: outer radius
    std::uint8_t ease = 0;
};

inline bool op_is_extended(Op op) {
    // A range plus one, deliberately: the transitions sit numerically between
    // Replace and Relief and are NOT extended — they are non-local, and
    // sweeping them into this predicate would let culling drop them.
    return (static_cast<int>(op) >= kernel::ccombine_groove &&
            static_cast<int>(op) <= kernel::ccombine_replace) ||
           op == Op::Relief || op == Op::Incise;
}

// Diagonal modes mix both gradients (Lipschitz up to sqrt(2); exactness.h).
inline bool op_is_diagonal(Op op) {
    return op == Op::Pipe || op == Op::Engrave || op == Op::Emboss;
}

// Modes that create material independent of the accumulated field. They are
// not skipped on an empty accumulator: the compiler emits their combine
// anyway and the interpreter seeds it with the far field, so per-brick
// culling of everything beneath them stays band-clamp identical.
inline bool op_creates_material(Op op) { return op == Op::Shell || op == Op::Replace; }

enum class BlendProfile : std::uint8_t {
    Hard = kernel::cblend_hard,
    Quadratic = kernel::cblend_quadratic,
    Cubic = kernel::cblend_cubic,
    Circular = kernel::cblend_circular,
    Chamfer = kernel::cblend_chamfer,
};

struct Blend {
    BlendProfile profile = BlendProfile::Hard;
    float k = 0.0f;

    float support() const {
        return kernel::ctape_blend_support(static_cast<int>(profile), k);
    }
};

inline constexpr int kMaxPrimParams = 7;

struct Prim {
    PrimType type = PrimType::Sphere;
    float params[kMaxPrimParams] = {};

    static Prim sphere(float r) { return {PrimType::Sphere, {r}}; }
    static Prim box(kernel::cfloat3 b) { return {PrimType::Box, {b.x, b.y, b.z}}; }
    static Prim round_box(kernel::cfloat3 b, float r) {
        return {PrimType::RoundBox, {b.x, b.y, b.z, r}};
    }
    static Prim box_frame(kernel::cfloat3 b, float e) {
        return {PrimType::BoxFrame, {b.x, b.y, b.z, e}};
    }
    static Prim torus(float R, float r) { return {PrimType::Torus, {R, r}}; }
    static Prim capsule(kernel::cfloat3 a, kernel::cfloat3 b, float r) {
        return {PrimType::Capsule, {a.x, a.y, a.z, b.x, b.y, b.z, r}};
    }
    static Prim capped_cylinder(float r, float h) { return {PrimType::CappedCylinder, {r, h}}; }
    static Prim rounded_cylinder(float ra, float rb, float h) {
        return {PrimType::RoundedCylinder, {ra, rb, h}};
    }
    static Prim capped_cone(float h, float r1, float r2) {
        return {PrimType::CappedCone, {h, r1, r2}};
    }
    static Prim round_cone(float r1, float r2, float h) {
        return {PrimType::RoundCone, {r1, r2, h}};
    }
    static Prim ellipsoid(kernel::cfloat3 r) { return {PrimType::Ellipsoid, {r.x, r.y, r.z}}; }
    static Prim octahedron(float s) { return {PrimType::Octahedron, {s}}; }
    static Prim hex_prism(float hx, float hy) { return {PrimType::HexPrism, {hx, hy}}; }
    static Prim pyramid(float h) { return {PrimType::Pyramid, {h}}; }
    static Prim stroke() { return {PrimType::Stroke, {}}; }
    // The nodes live in Node::stroke and the topology in Node::armature_parents.
    static Prim armature() { return {PrimType::Armature, {}}; }
    // Lifts carry their profile in Node::profile / Node::profile_points; the
    // single prim param is the lift's own (half-depth or axis offset).
    static Prim extrude(float half_depth) { return {PrimType::Extrude, {half_depth}}; }
    // The profiles themselves live on the Node, in `profiles`: a variable
    // number of them cannot fit in a fixed parameter block, and a polygon
    // profile's vertices are already out of line.
    static Prim loft(float half_depth, std::uint8_t ease = 0) {
        return {PrimType::Loft, {half_depth, static_cast<float>(ease)}};
    }
    // The guide lives in the Node's stroke point list and the profiles in its
    // profile list; neither fits a fixed parameter block.
    static Prim swept(std::uint8_t ease = 0) {
        return {PrimType::Swept, {static_cast<float>(ease)}};
    }
    // The samples live on the Node, in `volume`: they are far too large for a
    // parameter block and are shared rather than copied.
    static Prim volume() { return {PrimType::Volume, {}}; }
    static Prim revolve(float offset) { return {PrimType::Revolve, {offset}}; }

    // -- backfill (add-primitive-backfill) ------------------------------------
    // aperture given as the half-angle in radians, stored as (sin, cos)
    static Prim capped_torus(float aperture, float ra, float rb) {
        return {PrimType::CappedTorus,
                {kernel::csin(aperture), kernel::ccos(aperture), ra, rb}};
    }
    static Prim link(float length, float r1, float r2) {
        return {PrimType::Link, {length, r1, r2}};
    }
    static Prim cylinder_infinite(float cx, float cz, float r) {
        return {PrimType::CylinderInfinite, {cx, cz, r}};
    }
    static Prim cone(float half_angle, float h) {
        return {PrimType::Cone, {kernel::csin(half_angle), kernel::ccos(half_angle), h}};
    }
    static Prim plane(kernel::cfloat3 normal, float offset) {
        kernel::cfloat3 n = kernel::cnormalize(normal);
        return {PrimType::Plane, {n.x, n.y, n.z, offset}};
    }
    static Prim cut_sphere(float r, float h) { return {PrimType::CutSphere, {r, h}}; }
    static Prim cut_hollow_sphere(float r, float h, float t) {
        return {PrimType::CutHollowSphere, {r, h, t}};
    }
    static Prim solid_angle(float angle, float ra) {
        return {PrimType::SolidAngle, {kernel::csin(angle), kernel::ccos(angle), ra}};
    }
    static Prim tetrahedron(float r) { return {PrimType::Tetrahedron, {r}}; }
    static Prim dodecahedron(float r) { return {PrimType::Dodecahedron, {r}}; }
    static Prim icosahedron(float r) { return {PrimType::Icosahedron, {r}}; }
    static Prim tri_prism(float hx, float hy) { return {PrimType::TriPrism, {hx, hy}}; }
    static Prim octahedron_cheap(float s) { return {PrimType::OctahedronCheap, {s}}; }
    static Prim lnorm_sphere(float r, float n) { return {PrimType::LNormSphere, {r, n}}; }
};

// Repetition an item carries (kernel/tape.h CRepeatType). Applied to the
// local point before any deformer, so an array repeats the deformed shape.
struct Repeat {
    std::uint8_t type = kernel::crepeat_none;
    kernel::cfloat3 spacing = kernel::cf3(1, 1, 1);  // radial: x = count, y = offset
    kernel::cfloat3 counts = kernel::cf3(0, 0, 0);   // finite grid: max cell index per axis

    static Repeat grid_infinite(kernel::cfloat3 spacing) {
        return {kernel::crepeat_grid_infinite, spacing, kernel::cf3(0, 0, 0)};
    }
    static Repeat grid_finite(float spacing, kernel::cfloat3 counts) {
        return {kernel::crepeat_grid_finite, kernel::cf3(spacing, spacing, spacing), counts};
    }
    static Repeat radial(int count, float offset) {
        return {kernel::crepeat_radial,
                kernel::cf3(static_cast<float>(count), offset, 0.0f), kernel::cf3(0, 0, 0)};
    }

    bool active() const { return type != kernel::crepeat_none; }
    bool is_infinite_grid() const { return type == kernel::crepeat_grid_infinite; }
};

// A closed 2D profile for the lift primitives (kernel/tape.h CProfileType).
// Polygon vertices live on the Node (out-of-line, like stroke points).
struct Profile {
    std::uint8_t type = kernel::cprofile_circle;
    float params[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    static Profile circle(float r) { return {kernel::cprofile_circle, {r}}; }
    static Profile box(float hx, float hy) { return {kernel::cprofile_box, {hx, hy}}; }
    static Profile hexagon(float r) { return {kernel::cprofile_hexagon, {r}}; }
    static Profile triangle(float r) { return {kernel::cprofile_triangle, {r}}; }
    static Profile trapezoid(float bottom, float top, float half_height) {
        return {kernel::cprofile_trapezoid, {bottom, top, half_height}};
    }
    static Profile vesica(float r, float d) { return {kernel::cprofile_vesica, {r, d}}; }
    // vertices are carried by Node::profile_points
    static Profile polygon() { return {kernel::cprofile_polygon, {}}; }

    bool is_polygon() const { return type == kernel::cprofile_polygon; }
};

// One domain warp on an item (kernel/tape.h CDeformType). Ordered chains
// apply in authoring order: the point is warped by deformers[0] first.
// How a point joins the one after it. Hard is the original behaviour and
// stays the default, so a point list written before types existed means what
// it always meant.
enum class StrokePointType : std::uint8_t {
    Hard = 0,   // straight to the next point
    Spline,     // Catmull-Rom through the control points
    BSpline,    // uniform cubic B-spline; approximating, so it smooths corners
    Bezier,     // cubic through the points, shaped by the handles below
};

struct StrokePoint {
    kernel::cfloat3 pos;
    float radius = 0.0f;
    StrokePointType type = StrokePointType::Hard;
    // Bezier only, in the item's LOCAL space and relative to pos. 3DCoat keeps
    // its handles in screen space and its own users call that a wart: the
    // handles then depend on the camera, so a curve means something different
    // depending on where you were standing when you edited it.
    kernel::cfloat3 in_handle = kernel::cf3(0, 0, 0);
    kernel::cfloat3 out_handle = kernel::cf3(0, 0, 0);
};

// A caller-supplied alpha stamp: the scalar image an artist details with.
//
// THE ENGINE DECODES NO IMAGES. These are samples in [0,1] the caller already
// has, row-major with u fastest. A library that compiles to five backends has
// no business linking a PNG decoder, and a host that owns an alpha has already
// paid that cost.
struct AlphaStamp {
    std::vector<float> samples;  // width * height, row-major, u fastest
    int width = 0;
    int height = 0;
    float extent = 1.0f;     // world size of the square the stamp covers
    float radius = 1.0f;     // the region's falloff radius
    float amplitude = 0.0f;  // how far the surface moves at stamp value 1

    // What the Lipschitz bound needs, derived from the samples rather than
    // declared: the largest difference between ADJACENT samples, and the
    // largest magnitude. Cached because a bound is recomputed on every tape
    // build and a 4K stamp is sixteen million samples — but derived by
    // refresh() rather than settable, so a caller cannot understate the bound
    // and get an overshooting raymarch.
    //
    // Set through the factory or refresh(); a caller that writes `samples`
    // directly MUST call refresh(), and deserialization always does, so a file
    // cannot carry a stale one.
    float steepest = 0.0f;  // max |adjacent difference|, in sample units
    float peak = 0.0f;      // max |sample|

    bool empty() const {
        return width < 2 || height < 2 ||
               samples.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    void refresh() {
        steepest = 0.0f;
        peak = 0.0f;
        if (empty()) return;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float v = samples[static_cast<std::size_t>(y) * width + x];
                peak = kernel::cmax(peak, kernel::cabs(v));
                // Right and down neighbours only: every adjacent pair is
                // visited exactly once that way.
                if (x + 1 < width)
                    steepest = kernel::cmax(
                        steepest,
                        kernel::cabs(samples[static_cast<std::size_t>(y) * width + x + 1] - v));
                if (y + 1 < height)
                    steepest = kernel::cmax(
                        steepest,
                        kernel::cabs(samples[static_cast<std::size_t>(y + 1) * width + x] - v));
            }
        }
    }

    // Steepness in WORLD units: the sample difference over the distance
    // between two texels. This is what the bound charges, and it is why a
    // stamp squeezed into a small extent costs more than the same stamp spread
    // wide — the same relief over less distance is a steeper slope.
    float world_slope() const {
        if (empty() || extent <= 1e-9f) return 0.0f;
        const int longest = width > height ? width : height;
        return steepest * static_cast<float>(longest - 1) / extent;
    }
};

struct Deformer {
    std::uint8_t type = kernel::cdeform_twist;
    float k = 0.0f;      // twist/bend: radians per unit; taper: y0; displace: amplitude
    float a = 0.0f;      // taper: y1; displace: frequency
    float b = 1.0f;      // taper: s0
    float c = 1.0f;      // taper: s1
    std::uint8_t ease = 0;
    // Extension slots for the wide deformers: bend_linear needs nine floats
    // and k/a/b/c hold four. Written only for the types that use them, so the
    // document format needs no version bump.
    float ext[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // The guide for `bend_curve`, and empty for every other type. A curve is
    // not a fixed size, so it cannot live in the record; the compiler writes
    // it into the tape's blob and puts a handle in slots 1 and 2, exactly as a
    // swept primitive does with its own guide.
    //
    // Typed control points rather than bare positions, so it tessellates
    // through the same `tessellate_curve` every other curve in the document
    // uses and an artist gets B-spline smoothing for free. `StrokePoint::radius`
    // has no meaning here and is ignored.
    std::vector<StrokePoint> guide;

    // The cage for `lattice`, and empty for every other type: nx*ny*nz
    // control-point OFFSETS in x-fastest order. Like the guide above it does
    // not fit the record, so the compiler writes it into the tape's blob and
    // the record holds a handle — the machinery `bend_curve` added.
    //
    // OFFSETS rather than positions, so an untouched cage is exactly the
    // identity, and they are the INVERSE warp: forward FFD has no closed-form
    // inverse and a deformer must run backwards, so the cage is authored as the
    // map it needs to be. It is not the EXACT inverse — it differs from forward
    // FFD by a term proportional to how the basis varies along the
    // displacement — but the offsets are what was DRAGGED, so material travels
    // with the drag as it does on the mesh lattice.
    std::vector<kernel::cfloat3> cage;

    // Where the cage sits relative to the item, for `lattice_transformed`, and
    // ignored by every other type. The point is mapped by this into the cage's
    // own space, warped, and mapped back — which is what lets ONE cage placed
    // in the world act on items in any frame, including rotated ones that no
    // axis-aligned per-item box could reproduce.
    math::Transform cage_xform;

    // The stamp for `alpha`, and empty for every other type. Like the guide and
    // the cage above, it does not fit the record: the compiler writes it into
    // the tape's blob behind a small header and the record holds a handle.
    AlphaStamp stamp;

    // How many extension floats this type carries; the serializer and the
    // reader both take their count from here, dispatching on the type.
    static int ext_count(std::uint8_t type) {
        if (type == kernel::cdeform_bend_linear) return 5;
        if (type == kernel::cdeform_pose_line) return 6;
        if (type == kernel::cdeform_grab || type == kernel::cdeform_pose) return 4;
        if (type == kernel::cdeform_magnify) return 1;
        if (type == kernel::cdeform_noise) return 1;
        if (type == kernel::cdeform_blob) return 5;
        // The box, which is what the record has room for beside the handle.
        if (type == kernel::cdeform_lattice || type == kernel::cdeform_lattice_xform)
            return 6;
        // Direction and tangent: the stamp's frame, which is what the record
        // has room for beside the handle and the centre. Everything else rides
        // the blob header.
        if (type == kernel::cdeform_alpha) return 6;
        return 0;
    }


    static Deformer twist(float radians_per_unit) {
        Deformer d;
        d.type = kernel::cdeform_twist;
        d.k = radians_per_unit;
        return d;
    }
    static Deformer bend(float radians_per_unit) {
        Deformer d;
        d.type = kernel::cdeform_bend;
        d.k = radians_per_unit;
        return d;
    }
    static Deformer taper(float y0, float y1, float s0, float s1, std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_taper;
        d.k = y0;
        d.a = y1;
        d.b = s0;
        d.c = s1;
        d.ease = ease;
        return d;
    }
    // Bend the local X interval [x0, x1] around a cylinder about Z. The
    // radius is fixed by the interval: r = (x1 - x0) / 2pi.
    static Deformer wrap_around(float x0, float x1) {
        Deformer d;
        d.type = kernel::cdeform_wrap;
        d.k = x0;
        d.a = x1;
        return d;
    }
    // Insert flat sections of half-extent h along each axis: the shape
    // stretches without its ends distorting.
    static Deformer elongate(kernel::cfloat3 h) {
        Deformer d;
        d.type = kernel::cdeform_elongate;
        d.k = h.x;
        d.a = h.y;
        d.b = h.z;
        return d;
    }
    // Displace by v, eased along the segment a -> b.
    static Deformer bend_linear(kernel::cfloat3 a, kernel::cfloat3 b, kernel::cfloat3 v,
                                std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_bend_linear;
        d.k = a.x;
        d.a = a.y;
        d.b = a.z;
        d.c = b.x;
        d.ext[0] = b.y;
        d.ext[1] = b.z;
        d.ext[2] = v.x;
        d.ext[3] = v.y;
        d.ext[4] = v.z;
        d.ease = ease;
        return d;
    }
    // Displace along Y by dz, eased across the radial band r0 -> r1.
    // Ranged twist: k radians per unit, ramped across [y0, y1] and held past
    // it. With a linear ease and a range covering the content this IS twist.
    static Deformer twist_range(float radians_per_unit, float y0, float y1,
                                std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_twist_range;
        d.k = radians_per_unit;
        d.a = y0;
        d.b = y1;
        d.ease = ease;
        return d;
    }
    // The same for bend, across [x0, x1].
    static Deformer bend_range(float radians_per_unit, float x0, float x1,
                               std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_bend_range;
        d.k = radians_per_unit;
        d.a = x0;
        d.b = x1;
        d.ease = ease;
        return d;
    }
    // Bend along a DRAWN guide instead of at a constant rate — the item's
    // local X span [t0, t1] is laid onto the guide's arc length and the
    // material rides the guide's parallel-transported frames.
    //
    // Slots 1 and 2 of the compiled record hold the guide's blob offset and
    // vertex count, which only exist once the tape is built, so `k` and `a`
    // are left for the compiler to fill and the span lives in `b` and `c`.
    static Deformer bend_curve(std::vector<StrokePoint> guide, float t0, float t1) {
        Deformer d;
        d.type = kernel::cdeform_bend_curve;
        d.b = t0;
        d.c = t1;
        d.guide = std::move(guide);
        return d;
    }
    // A lattice cage over the item's local box. Divisions are clamped into
    // [2, kMaxLatticeDivisions]; the cap is low because this is evaluated PER
    // SAMPLE in the raymarcher, at nx*ny*nz multiply-adds each time — unlike
    // the mesh lattice, which runs once per vertex and can afford far more.
    //
    // The offsets start at zero, which is exactly the identity. Set them with
    // `set_cage_offset` and the compiler does the rest.
    static constexpr int kMaxLatticeDivisions = CLAY_LATTICE_MAX_DIV;
    static Deformer lattice(kernel::cfloat3 box_min, kernel::cfloat3 box_max, int nx = 3,
                            int ny = 3, int nz = 3) {
        Deformer d;
        d.type = kernel::cdeform_lattice;
        auto clamp_div = [](int n) {
            return n < 2 ? 2 : (n > kMaxLatticeDivisions ? kMaxLatticeDivisions : n);
        };
        d.a = static_cast<float>(clamp_div(nx));
        d.b = static_cast<float>(clamp_div(ny));
        d.c = static_cast<float>(clamp_div(nz));
        d.ext[0] = box_min.x;
        d.ext[1] = box_min.y;
        d.ext[2] = box_min.z;
        d.ext[3] = box_max.x;
        d.ext[4] = box_max.y;
        d.ext[5] = box_max.z;
        d.cage.assign(static_cast<std::size_t>(d.a) * static_cast<std::size_t>(d.b) *
                          static_cast<std::size_t>(d.c),
                      kernel::cf3(0, 0, 0));
        return d;
    }
    // Drag one control point. Out-of-range indices write nowhere.
    // `ck` rather than `k`: this struct already has a member called k.
    void set_cage_offset(int i, int j, int ck, kernel::cfloat3 v) {
        const int nx = static_cast<int>(a), ny = static_cast<int>(b), nz = static_cast<int>(c);
        if (!is_lattice(type)) return;
        if (i < 0 || j < 0 || ck < 0 || i >= nx || j >= ny || ck >= nz) return;
        cage[static_cast<std::size_t>((ck * ny + j) * nx + i)] = v;
    }
    kernel::cfloat3 cage_offset(int i, int j, int ck) const {
        const int nx = static_cast<int>(a), ny = static_cast<int>(b), nz = static_cast<int>(c);
        if (!is_lattice(type)) return kernel::cf3(0, 0, 0);
        if (i < 0 || j < 0 || ck < 0 || i >= nx || j >= ny || ck >= nz) return kernel::cf3(0, 0, 0);
        return cage[static_cast<std::size_t>((ck * ny + j) * nx + i)];
    }
    // Where a control point started, derived from the box so the two cannot
    // disagree.
    kernel::cfloat3 cage_rest(int i, int j, int ck) const {
        const int nx = static_cast<int>(a), ny = static_cast<int>(b), nz = static_cast<int>(c);
        auto along = [](float lo, float hi, int idx, int count) {
            return count < 2 ? lo
                             : lo + (hi - lo) * (static_cast<float>(idx) /
                                                 static_cast<float>(count - 1));
        };
        return kernel::cf3(along(ext[0], ext[3], i, nx), along(ext[1], ext[4], j, ny),
                           along(ext[2], ext[5], ck, nz));
    }

    // The same cage, applied through `local_to_cage` — the transform taking a
    // point in the ITEM's frame into the cage's own. `brush::lattice_gizmo`
    // computes it; a caller placing a cage by hand on an unrotated item wants
    // `lattice` above, which costs less per sample.
    static Deformer lattice_transformed(kernel::cfloat3 box_min, kernel::cfloat3 box_max,
                                        const math::Transform& local_to_cage, int nx = 3,
                                        int ny = 3, int nz = 3) {
        Deformer d = lattice(box_min, box_max, nx, ny, nz);
        d.type = kernel::cdeform_lattice_xform;
        d.cage_xform = local_to_cage;
        return d;
    }

    // True for either lattice form, which is what most call sites actually
    // mean when they ask.
    static bool is_lattice(std::uint8_t type) {
        return type == kernel::cdeform_lattice || type == kernel::cdeform_lattice_xform;
    }

    // ZBrush's Blob: an irregular swelling under the brush rather than the
    // smooth one `draw` gives. `noise` with the finite support `grab` and
    // `magnify` have — outside the radius the field is untouched, which is
    // what makes it a brush rather than a modifier.
    //
    // The amplitude is signed and so is the noise, so one dab both swells and
    // eats in — which is what reads as blobby rather than as a uniform bulge.
    static Deformer blob(kernel::cfloat3 centre, float radius, float amplitude,
                         float frequency = 6.0f, int octaves = 3, float gain = 0.5f,
                         std::uint32_t seed = 0, std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_blob;
        d.k = centre.x;
        d.a = centre.y;
        d.b = centre.z;
        d.c = radius;
        d.ext[0] = amplitude;
        d.ext[1] = frequency;
        d.ext[2] = static_cast<float>(octaves);
        d.ext[3] = gain;
        d.ext[4] = static_cast<float>(seed);
        d.ease = ease;
        return d;
    }

    // An alpha stamp: a caller-supplied scalar image as a distance offset,
    // under the same radial falloff `blob`, `grab` and `magnify` use.
    //
    // A DEFORMER, NOT A PRIMITIVE. An item shaped like the stamp would ADD
    // material in the stamp's shape; an alpha modulates a surface already
    // there — pores in existing skin. So it offsets the distance, exactly as
    // `noise` and `blob` do, and the surface moves along its own normal.
    //
    // `dir` is where the stamp pushes, `tangent` orients it in the plane (any
    // rough "up" works — it is re-orthogonalised, and a degenerate one falls
    // back to a derived axis). `extent` is the world size of the square the
    // stamp covers; `radius` is where the influence ends. Outside `radius` the
    // field is untouched exactly, which is what makes this a brush rather than
    // a modifier.
    //
    // `amplitude` is how far the surface moves OUTWARD at a stamp value of 1,
    // so white is raised as it is in every sculpting package; a negative
    // amplitude carves. (A deformer's offset is added to the distance, where
    // positive means further away, so the kernel flips the sign once rather
    // than making every caller do it.)
    //
    // `samples` are COPIED, so a caller may free its buffer immediately, and
    // the bound is derived here rather than declared.
    static Deformer alpha(kernel::cfloat3 centre, kernel::cfloat3 dir, kernel::cfloat3 tangent,
                          const float* samples, int width, int height, float extent, float radius,
                          float amplitude, std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_alpha;
        // k is unused: the compiler overwrites slot 1 with the blob handle,
        // exactly as it does for a bend curve's guide.
        d.a = centre.x;
        d.b = centre.y;
        d.c = centre.z;
        d.ext[0] = dir.x;
        d.ext[1] = dir.y;
        d.ext[2] = dir.z;
        d.ext[3] = tangent.x;
        d.ext[4] = tangent.y;
        d.ext[5] = tangent.z;
        d.ease = ease;
        d.stamp.width = width;
        d.stamp.height = height;
        d.stamp.extent = extent;
        d.stamp.radius = radius;
        d.stamp.amplitude = amplitude;
        if (samples && width >= 2 && height >= 2)
            d.stamp.samples.assign(samples, samples + static_cast<std::size_t>(width) * height);
        d.stamp.refresh();
        return d;
    }

    static Deformer bend_radial(float r0, float r1, float dz, std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_bend_radial;
        d.k = r0;
        d.a = r1;
        d.b = dz;
        d.ease = ease;
        return d;
    }
    // Per-axis elongation: works on any primitive, but the flat interior
    // plateau makes it a bound rather than a distance.
    static Deformer elongate_axis(kernel::cfloat3 h) {
        Deformer d;
        d.type = kernel::cdeform_elongate_axis;
        d.k = h.x;
        d.a = h.y;
        d.b = h.z;
        return d;
    }
    // Translate a region by `displacement`, weighted from the centre out and
    // zero past the radius. front_only gates on the half-space the pull heads
    // into, so the far side of a form does not travel with the near side.
    // Fractal gradient noise, offsetting the distance — the irregular sibling
    // of `displace`, whose sine is regular by construction. The seed is an
    // ordinary parameter rather than global state, so two items with the same
    // seed look the same and an item's appearance never depends on the order it
    // was compiled in.
    static Deformer noise(float amplitude, float frequency, int octaves = 4,
                          float gain = 0.5f, std::uint32_t seed = 0) {
        Deformer d;
        d.type = kernel::cdeform_noise;
        d.k = amplitude;
        d.a = frequency;
        d.b = static_cast<float>(octaves);
        d.c = gain;
        d.ext[0] = static_cast<float>(seed);
        return d;
    }

    // Magnify and pinch, which are the same deformation: a radial scale about
    // `centre` with finite support. A POSITIVE strength swells the surface away
    // from the centre, a negative one gathers it toward. Maxon's own page has
    // it — "Magnify: pushes vertices away from cursor; inverse of Pinch" — so
    // one signed parameter is the honest interface rather than two verbs.
    static Deformer magnify(kernel::cfloat3 centre, float radius, float strength,
                            std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_magnify;
        d.k = centre.x;
        d.a = centre.y;
        d.b = centre.z;
        d.c = radius;
        d.ext[0] = strength;
        d.ease = ease;
        return d;
    }

    static Deformer grab(kernel::cfloat3 centre, float radius, kernel::cfloat3 displacement,
                         std::uint8_t ease = 0, bool front_only = false) {
        Deformer d;
        d.type = kernel::cdeform_grab;
        d.k = centre.x;
        d.a = centre.y;
        d.b = centre.z;
        d.c = radius;
        d.ext[0] = displacement.x;
        d.ext[1] = displacement.y;
        d.ext[2] = displacement.z;
        d.ext[3] = front_only ? 1.0f : 0.0f;
        d.ease = ease;
        return d;
    }
    // Rotate a region about `centre`, weighted the same way.
    static Deformer pose(kernel::cfloat3 centre, float radius, kernel::cfloat3 axis, float angle,
                         std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_pose;
        d.k = centre.x;
        d.a = centre.y;
        d.b = centre.z;
        d.c = radius;
        d.ext[0] = axis.x;
        d.ext[1] = axis.y;
        d.ext[2] = axis.z;
        d.ext[3] = angle;
        d.ease = ease;
        return d;
    }
    // Rotate about the axis through `a`, ramping from nothing at `a` to the
    // full angle at `b` and beyond. The anchor is a fixed point; unlike the
    // radial pose this does not stop at a radius.
    static Deformer pose_line(kernel::cfloat3 a, kernel::cfloat3 b, kernel::cfloat3 axis,
                              float angle, std::uint8_t ease = 0) {
        Deformer d;
        d.type = kernel::cdeform_pose_line;
        d.k = a.x;
        d.a = a.y;
        d.b = a.z;
        d.c = b.x;
        d.ext[0] = b.y;
        d.ext[1] = b.z;
        d.ext[2] = axis.x;
        d.ext[3] = axis.y;
        d.ext[4] = axis.z;
        d.ext[5] = angle;
        d.ease = ease;
        return d;
    }
    static Deformer displace(float amplitude, float frequency) {
        Deformer d;
        d.type = kernel::cdeform_displace;
        d.k = amplitude;
        d.a = frequency;
        return d;
    }
};

struct Node {
    NodeId id = kNoNode;
    bool is_group = false;
    bool visible = true;

    Op op = Op::Add;
    Blend blend;

    // item fields
    Prim prim;
    math::Transform xform;
    float rounding = 0.0f;
    kernel::cfloat3 color = kernel::cf3(0.7f, 0.7f, 0.7f);
    // Participates in the layer's active mirror. TRUE by default: a sculptor
    // who turns symmetry on means the layer, so an item has to opt OUT, not
    // in (issue #60 — a layer mirror that mirrored nothing by default). A
    // layer with no mirror axes evaluates identically either way, and a
    // document loads with whatever each node had when it was saved.
    bool mirror = true;
    // Control points, for a Stroke and for a Swept guide alike — a guide is an
    // ordinary curve, so it uses the same list rather than one of its own.
    std::vector<StrokePoint> stroke;
    float stroke_blend_k = 0.0f;      // within-stroke segment smoothing
    // Armature only: parent index per stroke point, same length as `stroke`.
    // A node whose parent is itself is a root. Kept beside the points rather
    // than inside StrokePoint so a stroke costs nothing for a field it has no
    // use for, and so an armature IS a stroke plus a topology.
    std::vector<std::uint32_t> armature_parents;
    // Armature only: +1 or -1 per node, a negative node's link carving instead
    // of skinning (ZBrush's negative ZSphere). Shorter than `stroke` reads as
    // positive-padded, exactly as short parents read as roots.
    std::vector<std::int8_t> armature_signs;
    bool stroke_closed = false;       // last point joins back to the first
    // Maximum distance a tessellated span's midpoint may sit from its chord.
    // A document property, not a viewer setting: two builds have to agree on
    // what a document means.
    float curve_tolerance = 0.01f;
    std::vector<Deformer> deformers;  // applied to the local point, in order
    Transition transition;            // TransitionLinear/Radial ops only
    Repeat repeat;                    // grid / radial array of this item
    Profile profile;                  // Extrude/Revolve prims only
    std::vector<kernel::cfloat2> profile_points;  // polygon profile vertices
    // PrimType::Loft only: two or more profiles interpolated along Z, each
    // with its own polygon vertices at the matching index. Sized for any N
    // from the start, so carrying profiles along a guide later changes where
    // they are placed rather than how they are stored.
    std::vector<Profile> profiles;
    std::vector<std::vector<kernel::cfloat2>> profile_polygons;
    // PrimType::Volume only. Shared, so two items sampling the same source
    // share one set of samples.
    std::shared_ptr<const field::FieldVolume> volume;

    // -- gate ----------------------------------------------------------------
    // A MASK on this item's participation, and the thing that makes masking
    // protect a surface from ANY operation rather than only from a brush.
    //
    // Masks gate AUTHORING elsewhere: a voxel edit consumes one per cell as it
    // writes, and an SDF edit consumes one when a stroke becomes items. Neither
    // touches an item already in the edit list, which is why a mask over an ear
    // has never done anything about the next boolean. This does: where the gate
    // says protected, the accumulated field is exactly what it was before this
    // item combined.
    //
    // A SIGNED DISTANCE, not paint. Negative inside the protected region, and
    // `brush::mask_to_field` is what produces one from a painted MaskField.
    // Storing paint here instead would mean a Lipschitz bound set by however
    // hard the artist's brush edge happened to be; a distance gives a
    // 1-Lipschitz field whose falloff is `gate_width` — a number the caller
    // chooses and can change without repainting.
    //
    // Shared for the same reason `volume` is: several items gated by one
    // painted mask should not each carry a copy of it.
    //
    // ITS BAND MUST REACH AT LEAST `gate_width`. A sampled volume stores true
    // distances only within its band and saturates past it, so a band narrower
    // than the fade means the smoothstep never reaches 1 and the "fully
    // protected" end is never actually reached — the surface comes back at 92%
    // of where it should be, which reads as a subtle bug rather than as a
    // misconfiguration. The bindings derive the band from the width for exactly
    // this reason; a caller building the volume by hand owns the constraint.
    std::shared_ptr<const field::FieldVolume> gate;
    // How far the protection fades across, in world units. Zero would be a step
    // in the field with no finite Lipschitz bound, so it is clamped to
    // something small rather than honoured; a WIDE gate costs almost no step
    // scale and a narrow one costs honestly.
    float gate_width = 0.05f;

    bool gated() const { return gate != nullptr; }

    // group fields
    std::vector<NodeId> children;
};

}  // namespace scene
}  // namespace clay
