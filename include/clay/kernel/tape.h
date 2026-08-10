#pragma once

// Flat postfix tape: the ONE program format every backend interprets
// (scene-model spec: scenes never become shader code; parameter edits only
// rewrite param blocks). The scene module compiles edit lists into this
// format (clay/scene/tape_build.h); this header owns the opcode set, the
// binary layout, and the fixed interpreter, all in the kernel dialect.
//
// Layout
//   instrs:  CTapeInstr[] — {op, param_offset into params[]}
//   params:  float[] — per-instruction parameter blocks
//   blob:    float[] — out-of-line payload pool: stroke points (4 floats
//            each: x, y, z, radius) and polygon profile vertices (2 each)
//
// Primitive param block: [inv affine 12 floats (columns c0..c3, xyz each)]
// [uniform scale s] [rounding r] [color rgb] [prim params, always
// CLAY_TAPE_PRIM_PARAMS wide] [repeat record] [deformer count]
// [deformer records...]. The
// inverse matrix already contains 1/s; the interpreter multiplies the local
// distance back by s and subtracts the rounding. Prim params are fixed-width
// so the deformer block sits at a known offset.
//
// Deformer record (CLAY_TAPE_DEFORM_FLOATS wide): [type] [k] [a] [b] [c]
// [ease] then six extension slots, used only by the wide types (a line-gradient
// pose needs ten parameters). The tape is rebuilt from the document on every
// compile, so its width costs nothing but transient memory.
// [ease]. Deformers warp the LOCAL point in authoring order before the
// primitive's distance function; each may also correct the distance after.
//
// Combine param block: [mode] [profile] [k] [r2], followed for transition
// modes by their own parameters — linear: [ax ay az bx by bz ease],
// radial: [r0 r1 ease]. Every other mode reads only the first four. r2 is the second radius
// of the two-parameter extended modes (groove/tongue half-width rb, mapped
// from the item's rounding in world units by the compiler); 0 elsewhere.

#include "clay/kernel/deform.h"
#include "clay/kernel/xform.h"
#include "clay/kernel/ops.h"
#include "clay/kernel/lift.h"
#include "clay/kernel/prim2d.h"
#include "clay/kernel/repeat.h"
#include "clay/kernel/prim3d.h"
#include "clay/kernel/shim.h"

CLAY_NS_BEGIN

enum CTapeOp {
    ctape_sphere = 0,
    ctape_box,
    ctape_round_box,
    ctape_box_frame,
    ctape_torus,
    ctape_capsule,           // local endpoints a, b + radius
    ctape_capped_cylinder,   // r, h
    ctape_rounded_cylinder,  // ra, rb, h
    ctape_capped_cone,       // h, r1, r2
    ctape_round_cone,        // r1, r2, h
    ctape_ellipsoid,         // rx, ry, rz (bound)
    ctape_octahedron,        // s
    ctape_hex_prism,         // hx, hy
    ctape_pyramid,           // h
    ctape_stroke,            // blend_k, point_offset, point_count
    ctape_extrude,           // profile block + half-depth (along Z)
    ctape_revolve,           // profile block + axis offset (about Y)
    // backfill: the rest of prim3d.h (add-primitive-backfill). The _ab
    // endpoint variants are deliberately absent: they need 8 parameters, and
    // an oriented cylinder or round cone is already expressible as the
    // axis-aligned form plus the item's transform — no capability lost, and
    // the prim block (and therefore the document format) stays put.
    ctape_capped_torus,      // sin, cos of the aperture, ra, rb
    ctape_link,              // le, r1, r2
    ctape_cylinder_inf,      // cx, cz, r  (unbounded)
    ctape_cone,              // sin, cos of the half-angle, h
    ctape_plane,             // nx ny nz h  (unbounded)
    ctape_cut_sphere,        // r, h
    ctape_cut_hollow_sphere, // r, h, t
    ctape_solid_angle,       // sin, cos of the angle, ra
    ctape_tetrahedron,       // r
    ctape_dodecahedron,      // r
    ctape_icosahedron,       // r
    ctape_tri_prism,         // hx, hy      (bound)
    ctape_octahedron_cheap,  // s           (bound)
    ctape_lnorm_sphere,      // r, n        (bound)
    // Loft: half-depth, ease, blob offset of the profile records, count.
    // The records are consecutive CLAY_TAPE_PROFILE_FLOATS blocks in the
    // blob, so a polygon profile's vertices index the blob exactly as they do
    // for a single-profile lift — carrying N profiles needs no new mechanism.
    ctape_loft,              // h, ease, record_offset, count   (bound)
    // Swept: profiles carried along a guide polyline whose frames were
    // parallel-transported when the item was compiled. Transport is
    // sequential along the curve, so it cannot be done per sample — which is
    // why the frames are in the blob rather than derived here.
    // guide_offset, guide_count, record_offset, profile_count, ease  (bound)
    ctape_swept,
    // A field SAMPLED onto a sparse narrow-band brick grid rather than given
    // by a formula. Only bricks that straddle the band store samples; the rest
    // carry a signed lower bound. blob_offset  (bound)
    ctape_volume,
    // An armature: a TREE of spheres, each naming its parent, skinned by one
    // sphere-swept segment per node-parent pair. This is ctape_stroke with the
    // chain generalised — the stroke walks (i, i+1), this walks (i, parent[i])
    // — so the segment maths, the smooth union and the blend are shared rather
    // than re-derived, and an armature whose parents form a line evaluates
    // identically to the stroke with the same points.
    // blend_k, node_offset, parent_offset, node_count  (bound)
    ctape_armature,
    ctape_prim_count,

    // Pushes the far field ("empty space"): standard primitive param block,
    // no prim-specific params. The compiler emits it to seed material-
    // creating combines (shell, replace) whose accumulator is absent.
    ctape_empty = 32,

    ctape_combine = 64,
};

// Extended modes (ops.h math; a = accumulated field, b = item field).
// Parameter mapping: k is the mode's radius/depth (pipe radius, engrave/
// emboss depth, groove/tongue depth ra, inset depth, shell half-thickness);
// r2 is the groove/tongue half-width rb (compiled from the item's rounding
// in world units). Color semantics: carving modes (groove, engrave, inset)
// keep a's color; adding modes (tongue, pipe, emboss, shell) take b's color
// where the added field wins; replace takes b's color inside b.
enum CCombineMode {
    ccombine_add = 0,
    ccombine_subtract = 1,
    ccombine_intersect = 2,
    ccombine_paint = 3,  // color-only (stain)
    ccombine_groove = 4,
    ccombine_tongue = 5,
    ccombine_pipe = 6,
    ccombine_engrave = 7,  // deboss
    ccombine_emboss = 8,
    ccombine_inset = 9,
    ccombine_shell = 10,
    ccombine_replace = 11,
    // Spatial morphs between the accumulated field and the item
    // (deform.h). NON-LOCAL: the weight is non-zero arbitrarily far from
    // both operands, so the scene marks these items infinite-influence and
    // never culls them. Extra params follow the shared four (see below).
    ccombine_transition_linear = 12,
    ccombine_transition_radial = 13,
    // Surface RELIEF: offsets the accumulated field by an amplitude, weighted
    // by the item's own field used as a REGION — the same trick paint plays for
    // colour. k = amplitude, r2 = falloff width.
    //
    // TWO ops rather than one signed amplitude, which is what this was first
    // designed as. blend_k is validated non-negative in three places including
    // the blend constructor, which has no op to be aware of — so the sign has
    // nowhere to live. It is also the convention already here: add/subtract and
    // engrave/emboss are pairs of ops, not one op with a sign. They share the
    // branch below so they cannot drift apart.
    ccombine_relief = 14,  // build the surface up  (ZBrush Standard, ClayBuildup)
    ccombine_incise = 15,  // cut into it           (Crease, DamStandard)
};

enum CBlendProfile {
    cblend_hard = 0,
    cblend_quadratic = 1,
    cblend_cubic = 2,
    cblend_circular = 3,
    cblend_chamfer = 4,
};

// typedef form: the OpenCL backend compiles these headers as C99, where a
// bare struct tag is not a type name.
typedef struct CTapeInstrT {
    unsigned int op;
    unsigned int param_offset;
} CTapeInstr;

typedef struct CTapeValueT {
    float d;
    cfloat3 color;
} CTapeValue;

#define CLAY_TAPE_MAX_STACK 16
#define CLAY_TAPE_FAR 3.4e37f
#define CLAY_TAPE_PRIM_PARAMS 7
#define CLAY_TAPE_DEFORM_FLOATS 12
#define CLAY_TAPE_PRIM_HEADER 17

// Repetition an item can carry (repeat.h). Applied to the local point
// BEFORE the deformer chain, so an array of twisted copies twists each copy.
// Record: [type] [sx] [sy] [sz] [cx] [cy] [cz] — for radial, sx = count and
// sy = axis offset.
#define CLAY_TAPE_REPEAT_FLOATS 7

enum CRepeatType {
    crepeat_none = 0,
    crepeat_grid_infinite = 1,  // spacing per axis
    crepeat_grid_finite = 2,    // spacing + max cell index per axis
    crepeat_radial = 3,         // count about Y, profile offset from the axis
};

// Map the local point into its repetition cell. Radial arrays need the O(2)
// neighbour evaluation, so the caller evaluates twice: pass sector = 0 and
// then sector = crep_radial_neighbor(...).
CLAY_FN cfloat3 ctape_repeat_point(CLAY_DEVICE const float* rec, cfloat3 p, int sector) {
    int type = (int)rec[0];
    if (type == crepeat_grid_infinite) return crep_inf_point(p, cf3(rec[1], rec[2], rec[3]));
    if (type == crepeat_grid_finite)
        return crep_lim_point(p, rec[1], cf3(rec[4], rec[5], rec[6]));
    if (type == crepeat_radial) return crep_radial_point(p, (int)rec[1], sector);
    return p;
}

CLAY_FN bool ctape_repeat_is_radial(CLAY_DEVICE const float* rec) {
    return (int)rec[0] == crepeat_radial;
}

CLAY_FN bool ctape_repeat_active(CLAY_DEVICE const float* rec) {
    return (int)rec[0] != crepeat_none;
}

// Domain warps an item can carry (deform.h). Values are serialization-stable.
enum CDeformType {
    cdeform_twist = 0,     // k = radians per unit about Y
    cdeform_bend = 1,      // k = radians per unit along X
    cdeform_taper = 2,     // k = y0, a = y1, b = s0, c = s1, ease
    cdeform_displace = 3,  // k = amplitude, a = frequency
    cdeform_wrap = 4,      // k = x0, a = x1: bend [x0,x1] about the Z axis
    cdeform_elongate = 5,  // k, a, b = per-axis half-extents to insert
    cdeform_bend_linear = 6,  // a(k,a,b) b(c,e0,e1) v(e2,e3,e4), eased
    cdeform_bend_radial = 7,  // k = r0, a = r1, b = dz, eased
    cdeform_elongate_axis = 8,  // k, a, b = half-extents; no correction
    cdeform_grab = 9,      // centre(k,a,b) radius(c) disp(e0,e1,e2) front(e3)
    cdeform_pose = 10,     // centre(k,a,b) radius(c) axis(e0,e1,e2) angle(e3)
    cdeform_pose_line = 11,  // a(k,a,b) b(c,e0,e1) axis(e2,e3,e4) angle(e5)
    // Radial scale about a centre. One signed strength: positive magnifies,
    // negative pinches. centre(k,a,b) radius(c) strength(e0)
    cdeform_magnify = 12,
    // Fractal gradient noise as a distance OFFSET, like displace — but
    // irregular, which is the whole point. k = amplitude, a = frequency,
    // b = octaves, c = gain, e0 = seed
    cdeform_noise = 13,
};

// Apply one deformer record to the local point. No deformer corrects the
// distance: safety is carried by the tape's tracked Lipschitz factor, which
// also keeps influence bounds tight (see the taper note below).
CLAY_FN cfloat3 ctape_deform_point(CLAY_DEVICE const float* rec, cfloat3 p) {
    int type = (int)rec[0];
    if (type == cdeform_twist) return ctwist_point(p, rec[1]);
    if (type == cdeform_bend) return cbend_point(p, rec[1]);
    if (type == cdeform_taper) {
        // NOTE: deliberately no ctaper_dist here. Multiplying the distance by
        // min(s,1) would keep the field conservative on its own, but it also
        // shrinks the field everywhere, which makes the item's influence
        // reach 1/s further than its geometry — and influence bounds are what
        // brick culling trusts. Instead the taper behaves like twist and bend:
        // the raw warped field, with safety carried by the tape's tracked
        // Lipschitz factor (cfi_taper includes the 1/s_min stretch).
        return ctaper_point(p, rec[1], rec[2], rec[3], rec[4], (int)rec[5]);
    }
    if (type == cdeform_wrap) return cwrap_around_point(p, rec[1], rec[2]);
    if (type == cdeform_bend_linear) {
        return cbend_linear_point(p, cf3(rec[1], rec[2], rec[3]),
                                  cf3(rec[4], rec[6], rec[7]),
                                  cf3(rec[8], rec[9], rec[10]), (int)rec[5]);
    }
    if (type == cdeform_bend_radial) {
        return cbend_radial_point(p, rec[1], rec[2], rec[3], (int)rec[5]);
    }
    if (type == cdeform_grab) {
        return cgrab_point(p, cf3(rec[1], rec[2], rec[3]), rec[4],
                           cf3(rec[6], rec[7], rec[8]), rec[9], (int)rec[5]);
    }
    if (type == cdeform_pose_line) {
        return cpose_line_point(p, cf3(rec[1], rec[2], rec[3]), cf3(rec[4], rec[6], rec[7]),
                                cf3(rec[8], rec[9], rec[10]), rec[11], (int)rec[5]);
    }
    if (type == cdeform_pose) {
        return cpose_point(p, cf3(rec[1], rec[2], rec[3]), rec[4],
                           cf3(rec[6], rec[7], rec[8]), rec[9], (int)rec[5]);
    }
    if (type == cdeform_magnify) {
        return cmagnify_point(p, cf3(rec[1], rec[2], rec[3]), rec[4], rec[6], (int)rec[5]);
    }
    if (type == cdeform_elongate_axis)
        return celongate_axis_point(p, cf3(rec[1], rec[2], rec[3]));
    if (type == cdeform_elongate) {
        // The correction rides ctape_deform_offset, which the chain evaluates
        // at this same pre-warp point — exactly what elongation needs.
        float unused = 0.0f;
        return celongate_point(p, cf3(rec[1], rec[2], rec[3]), &unused);
    }
    return p;  // displace acts on the distance, not the point
}

// Post-primitive distance contribution of one deformer (0 for point warps).
CLAY_FN float ctape_deform_offset(CLAY_DEVICE const float* rec, cfloat3 p) {
    int type = (int)rec[0];
    if (type == cdeform_elongate) {
        float correction = 0.0f;
        celongate_point(p, cf3(rec[1], rec[2], rec[3]), &correction);
        return correction;
    }
    if (type == cdeform_noise) {
        int octaves = (int)rec[3];
        if (octaves < 1) octaves = 1;
        if (octaves > 8) octaves = 8;  // past this the octaves are below a cell
        return rec[1] * cnoise_fbm(p * rec[2], octaves, rec[4], (cuint)rec[6]);
    }
    if (type != cdeform_displace) return 0.0f;
    float amp = rec[1];
    float freq = rec[2];
    return amp * csin(freq * p.x) * csin(freq * p.y) * csin(freq * p.z);
}

// Blend support width in world units — deviation from the hard op is zero
// beyond this |a-b| difference. The scene compiler uses the same function
// for influence bounds.
CLAY_FN float ctape_blend_support(int profile, float k) {
    if (profile == cblend_quadratic) return csmin_quadratic_support(k);
    if (profile == cblend_cubic) return csmin_cubic_support(k);
    if (profile == cblend_circular) return csmin_circular_support(k);
    if (profile == cblend_chamfer) return cchamfer_support(k);
    return 0.0f;
}

// Support width of an extended combine mode: the band-clamped result can
// differ from `a` only where the item field b < support + band, so influence
// bounds dilate the item bound by this width. Groove/tongue deviate within
// the channel half-width r2 of b's surface; pipe within its radius k of the
// intersection curve; engrave/emboss within the V depth k; shell within the
// wall half-thickness k. Inset and replace are decided by b's own sign
// outside its bound (support 0). The blend profile is ignored by these
// modes.
CLAY_FN float ccombine_extended_support(int mode, float k, float r2) {
    // Relief reaches exactly as far as its falloff, and no further: the weight
    // is zero beyond it, so influence bounds stay tight and culling still works.
    if (mode == ccombine_relief || mode == ccombine_incise) return cmax(r2, 0.0f);
    if (mode == ccombine_groove || mode == ccombine_tongue) return cmax(r2, 0.0f);
    if (mode == ccombine_inset || mode == ccombine_replace) return 0.0f;
    return cmax(k, 0.0f);
}

CLAY_FN cfloat2 ctape_smin_m(int profile, float a, float b, float k) {
    if (profile == cblend_quadratic) return csmin_quadratic_m(a, b, k);
    if (profile == cblend_cubic) return csmin_cubic_m(a, b, k);
    if (profile == cblend_circular) return csmin_circular_m(a, b, k);
    if (profile == cblend_chamfer) return cchamfer_m(a, b, k);
    return (a < b) ? cf2(a, 0.0f) : cf2(b, 1.0f);
}

CLAY_FN float ctape_smin(int profile, float a, float b, float k) {
    if (profile == cblend_quadratic) return csmin_quadratic(a, b, k);
    if (profile == cblend_cubic) return csmin_cubic(a, b, k);
    if (profile == cblend_circular) return csmin_circular(a, b, k);
    if (profile == cblend_chamfer) return cchamfer(a, b, k);
    return cmin(a, b);
}

// Stroke: chain of sphere-swept segments over raw float data (4 per point).
// One sphere-swept segment between two nodes. Factored out of the stroke so
// the armature cannot drift from it: a link is the same round cone either way,
// and the degenerate cases (coincident endpoints, equal radii) have to be
// answered the same or the two opcodes disagree on the same geometry.
CLAY_FN float csweep_link(cfloat3 p, cfloat3 a, cfloat3 b, float ra, float rb) {
    if (cdot2(b - a) < 1e-12f) return sd_sphere(p - a, cmax(ra, rb));
    if (cabs(ra - rb) < 1e-7f) return sd_capsule(p, a, b, ra);
    return sd_round_cone_ab(p, a, b, ra, rb);
}

CLAY_FN float ctape_stroke_dist(CLAY_DEVICE const float* pts, int count, cfloat3 p,
                                float blend_k) {
    if (count <= 0) return CLAY_TAPE_FAR;
    cfloat3 a0 = cf3(pts[0], pts[1], pts[2]);
    if (count == 1) return sd_sphere(p - a0, pts[3]);
    float d = CLAY_TAPE_FAR;
    for (int i = 0; i + 1 < count; ++i) {
        cfloat3 a = cf3(pts[i * 4 + 0], pts[i * 4 + 1], pts[i * 4 + 2]);
        cfloat3 b = cf3(pts[i * 4 + 4], pts[i * 4 + 5], pts[i * 4 + 6]);
        float seg = csweep_link(p, a, b, pts[i * 4 + 3], pts[i * 4 + 7]);
        d = (blend_k > 0.0f) ? csmin_quadratic(d, seg, blend_k) : cmin(d, seg);
    }
    return d;
}

// A tree of spheres. `nodes` is count * 4 floats (x, y, z, radius); `parents`
// is count floats, each the index of that node's parent. A node whose parent
// is itself is a root and contributes a bare sphere, so a one-node armature is
// a sphere rather than nothing.
//
// The fold order is ascending node index, and that is a REQUIREMENT rather
// than an implementation detail: csmin_quadratic is not associative, so three
// links meeting at a hip give a different field depending on the order they
// combine. Fixing the order here is what makes an armature evaluate the same
// on every backend.
CLAY_FN float ctape_armature_dist(CLAY_DEVICE const float* nodes,
                                  CLAY_DEVICE const float* parents, int count, cfloat3 p,
                                  float blend_k) {
    if (count <= 0) return CLAY_TAPE_FAR;
    float d = CLAY_TAPE_FAR;
    for (int i = 0; i < count; ++i) {
        int j = (int)parents[i];
        if (j < 0 || j >= count) j = i;
        cfloat3 a = cf3(nodes[i * 4 + 0], nodes[i * 4 + 1], nodes[i * 4 + 2]);
        float ra = nodes[i * 4 + 3];
        float seg;
        if (j == i) {
            // A root. Its sphere is ALREADY inside every link that names it,
            // so adding it again is redundant with a hard union and wrong with
            // a soft one: smooth-min of two overlapping terms pulls the surface
            // outward, and a chain armature would then stop matching the stroke
            // it is supposed to equal. Contribute it only when nothing else
            // does — an isolated node, which is the case that would otherwise
            // vanish. Roots are few, so the scan costs nothing in practice.
            bool referenced = false;
            for (int k = 0; k < count; ++k) {
                if (k == i) continue;
                int pk = (int)parents[k];
                if (pk == i) { referenced = true; break; }
            }
            if (referenced) continue;
            seg = sd_sphere(p - a, ra);
        } else {
            cfloat3 b = cf3(nodes[j * 4 + 0], nodes[j * 4 + 1], nodes[j * 4 + 2]);
            seg = csweep_link(p, a, b, ra, nodes[j * 4 + 3]);
        }
        d = (blend_k > 0.0f) ? csmin_quadratic(d, seg, blend_k) : cmin(d, seg);
    }
    return d;
}

// Closed 2D profiles a lift can carry (prim2d.h). Open curves (segment,
// Bezier) are unsigned distances rather than regions, so they are not
// profiles: documents reach curved outlines by flattening to a polygon.
enum CProfileType {
    cprofile_circle = 0,    // p0 = radius
    cprofile_box = 1,       // p0, p1 = half extents
    cprofile_hexagon = 2,   // p0 = face radius
    cprofile_triangle = 3,  // p0 = radius
    cprofile_trapezoid = 4, // p0 = bottom half-width, p1 = top half-width, p2 = half height
    cprofile_vesica = 5,    // p0 = radius, p1 = center separation
    cprofile_polygon = 6,   // p0 = blob offset, p1 = vertex count (2 floats each)
};

// Profile block layout inside a lift's prim params: [type] [p0..p3].
#define CLAY_TAPE_PROFILE_FLOATS 5

// Fold "how far outside the sampled box we are" into the value read at the
// projected point. The distance to the box ALONE is not usable: it falls to
// zero on the box face, and a sphere tracer reads zero as a surface, so every
// ray would hit an invisible shell where the sampling stopped.
//
// Pythagoras here is exact, not an approximation. The projection onto a box
// satisfies (p - c) . (s - c) <= 0 for every s in the box, so
// |p - s|^2 >= |p - c|^2 + |c - s|^2. The positive part covers a box that
// CLIPS the solid: the zero set then really does include the box face.
CLAY_FN float ctape_volume_outside(float inner, float outside) {
    if (outside <= 0.0f) return inner;
    float reach = cmax(inner, 0.0f);
    return csqrt(outside * outside + reach * reach);
}

// Samples per brick edge in a sampled volume. Bricks store one extra sample
// per axis — a halo — so a trilinear tap inside a brick never needs its
// neighbour, which is what makes the lookup a single array read.
#define CLAY_BRICK_DIM 8

CLAY_FN float ctape_profile_dist(CLAY_DEVICE const float* prof, CLAY_DEVICE const float* blob,
                                 cfloat2 p) {
    int type = (int)prof[0];
    if (type == cprofile_circle) return sd_circle2(p, prof[1]);
    if (type == cprofile_box) return sd_box2(p, cf2(prof[1], prof[2]));
    if (type == cprofile_hexagon) return sd_hexagon2(p, prof[1]);
    if (type == cprofile_triangle) return sd_equilateral_triangle2(p, prof[1]);
    if (type == cprofile_trapezoid) return sd_trapezoid2(p, prof[1], prof[2], prof[3]);
    if (type == cprofile_vesica) return sd_vesica2(p, prof[1], prof[2]);
    if (type == cprofile_polygon) {
        int off = (int)prof[1];
        int count = (int)prof[2];
        if (count < 3) return CLAY_TAPE_FAR;
        // vertices live in the out-of-line pool as consecutive (x, y) pairs;
        // sd_polygon2 walks them without materializing an array
        return sd_polygon2_raw(blob + off, count, p);
    }
    return CLAY_TAPE_FAR;
}

CLAY_FN float ctape_prim_dist(unsigned int op, CLAY_DEVICE const float* q,
                              CLAY_DEVICE const float* blob, cfloat3 lp) {
    // q points at the prim-specific params (after xform/scale/round/color).
    if (op == ctape_sphere) return sd_sphere(lp, q[0]);
    if (op == ctape_box) return sd_box(lp, cf3(q[0], q[1], q[2]));
    if (op == ctape_round_box) return sd_round_box(lp, cf3(q[0], q[1], q[2]), q[3]);
    if (op == ctape_box_frame) return sd_box_frame(lp, cf3(q[0], q[1], q[2]), q[3]);
    if (op == ctape_torus) return sd_torus(lp, q[0], q[1]);
    if (op == ctape_capsule)
        return sd_capsule(lp, cf3(q[0], q[1], q[2]), cf3(q[3], q[4], q[5]), q[6]);
    if (op == ctape_capped_cylinder) return sd_capped_cylinder(lp, q[0], q[1]);
    if (op == ctape_rounded_cylinder) return sd_rounded_cylinder(lp, q[0], q[1], q[2]);
    if (op == ctape_capped_cone) return sd_capped_cone(lp, q[0], q[1], q[2]);
    if (op == ctape_round_cone) return sd_round_cone(lp, q[0], q[1], q[2]);
    if (op == ctape_ellipsoid) return sd_ellipsoid_bound(lp, cf3(q[0], q[1], q[2]));
    if (op == ctape_octahedron) return sd_octahedron(lp, q[0]);
    if (op == ctape_hex_prism) return sd_hex_prism(lp, cf2(q[0], q[1]));
    if (op == ctape_pyramid) return sd_pyramid(lp, q[0]);
    if (op == ctape_capped_torus) return sd_capped_torus(lp, cf2(q[0], q[1]), q[2], q[3]);
    if (op == ctape_link) return sd_link(lp, q[0], q[1], q[2]);
    if (op == ctape_cylinder_inf) return sd_cylinder_inf(lp, cf2(q[0], q[1]), q[2]);
    if (op == ctape_cone) return sd_cone(lp, cf2(q[0], q[1]), q[2]);
    if (op == ctape_plane) return sd_plane(lp, cf3(q[0], q[1], q[2]), q[3]);
    if (op == ctape_cut_sphere) return sd_cut_sphere(lp, q[0], q[1]);
    if (op == ctape_cut_hollow_sphere) return sd_cut_hollow_sphere(lp, q[0], q[1], q[2]);
    if (op == ctape_solid_angle) return sd_solid_angle(lp, cf2(q[0], q[1]), q[2]);
    if (op == ctape_tetrahedron) return sd_tetrahedron(lp, q[0]);
    if (op == ctape_dodecahedron) return sd_dodecahedron(lp, q[0]);
    if (op == ctape_icosahedron) return sd_icosahedron(lp, q[0]);
    if (op == ctape_tri_prism) return sd_tri_prism_bound(lp, cf2(q[0], q[1]));
    if (op == ctape_octahedron_cheap) return sd_octahedron_bound(lp, q[0]);
    if (op == ctape_lnorm_sphere) return sd_lnorm_sphere_bound(lp, q[0], q[1]);
    if (op == ctape_extrude) {
        // exact lift: profile distance merged with the axial slab
        return cop_extrude(ctape_profile_dist(q, blob, cf2(lp.x, lp.y)), lp.z,
                           q[CLAY_TAPE_PROFILE_FLOATS]);
    }
    if (op == ctape_loft) {
        int off = (int)q[2];
        int count = (int)q[3];
        float h = q[0];
        // Two records are always read below, so fewer than two profiles would
        // read a record that was never written — a null blob when the loft is
        // the only item. ctape_swept guards the same way; a document loaded
        // from disk can carry any count.
        if (count < 2) return CLAY_TAPE_FAR;
        // Bracket among the profiles: they sit evenly along the depth, so the
        // span index falls straight out of the normalized height.
        float t = cclamp((lp.z + h) / cmax(2.0f * h, 1e-9f), 0.0f, 1.0f) * (float)(count - 1);
        int i = (int)t;
        if (i > count - 2) i = count - 2;
        if (i < 0) i = 0;
        float u = cease((int)q[1], t - (float)i);
        cfloat2 xy = cf2(lp.x, lp.y);
        float da = ctape_profile_dist(blob + off + i * CLAY_TAPE_PROFILE_FLOATS, blob, xy);
        float db = ctape_profile_dist(blob + off + (i + 1) * CLAY_TAPE_PROFILE_FLOATS, blob, xy);
        return cop_loft(da, db, u, lp.z, h);
    }
    if (op == ctape_volume) {
        CLAY_DEVICE const float* h = blob + (int)q[0];
        cfloat3 origin = cf3(h[0], h[1], h[2]);
        float cell = h[3];
        int bcx = (int)h[5], bcy = (int)h[6], bcz = (int)h[7];
        int index_off = (int)h[8], far_off = (int)h[9], data_off = (int)h[10];
        if (bcx <= 0 || bcy <= 0 || bcz <= 0 || cell <= 0.0f) return CLAY_TAPE_FAR;

        float span_x = (float)(bcx * CLAY_BRICK_DIM) * cell;
        float span_y = (float)(bcy * CLAY_BRICK_DIM) * cell;
        float span_z = (float)(bcz * CLAY_BRICK_DIM) * cell;
        // The point projected onto the sampled box, and how far outside it is.
        // The lookup below runs on the PROJECTED point, so it always reads a
        // real brick; the outside distance is folded in afterwards.
        cfloat3 cp = cf3(cclamp(lp.x, origin.x, origin.x + span_x),
                         cclamp(lp.y, origin.y, origin.y + span_y),
                         cclamp(lp.z, origin.z, origin.z + span_z));
        float outside = clength(lp - cp);

        float gx = (cp.x - origin.x) / cell;
        float gy = (cp.y - origin.y) / cell;
        float gz = (cp.z - origin.z) / cell;
        int cx = (int)cclamp(cfloor(gx), 0.0f, (float)(bcx * CLAY_BRICK_DIM - 1));
        int cy = (int)cclamp(cfloor(gy), 0.0f, (float)(bcy * CLAY_BRICK_DIM - 1));
        int cz = (int)cclamp(cfloor(gz), 0.0f, (float)(bcz * CLAY_BRICK_DIM - 1));
        int bx = cx / CLAY_BRICK_DIM, by = cy / CLAY_BRICK_DIM, bz = cz / CLAY_BRICK_DIM;
        int slot = (bz * bcy + by) * bcx + bx;
        // Relative to the VOLUME's own blob, not the tape's. to_blob writes
        // index/far/data offsets past its own 12-float header, so they are only
        // absolute when the volume happens to sit at blob offset 0 — which is
        // true exactly when nothing else out-of-line was emitted before it. A
        // stroke, loft, swept or armature earlier in the layer puts its payload
        // there first, and the volume then read whatever those had written.
        int entry = (int)h[index_off + slot];
        // An empty brick carries its own signed lower bound: the gap in bricks
        // to the nearest brick that HAS samples, floored at the band less half
        // a cell diagonal. A flat band width would be conservative but useless
        // — the marcher would creep across the empty majority of the region in
        // steps that never grew. See FieldVolume::far_value().
        if (entry < 0) return ctape_volume_outside(h[far_off + slot], outside);

        CLAY_DEVICE const float* block = h + data_off + entry;
        int lx = cx - bx * CLAY_BRICK_DIM;
        int ly = cy - by * CLAY_BRICK_DIM;
        int lz = cz - bz * CLAY_BRICK_DIM;
        float fx = cclamp(gx - (float)cx, 0.0f, 1.0f);
        float fy = cclamp(gy - (float)cy, 0.0f, 1.0f);
        float fz = cclamp(gz - (float)cz, 0.0f, 1.0f);
        const int n = CLAY_BRICK_DIM + 1;
        // The halo is why this needs no neighbouring brick: lx+1 is always in
        // range, so a trilinear tap is one array read.
        float c00 = cmix(block[((lz)*n + ly) * n + lx], block[((lz)*n + ly) * n + lx + 1], fx);
        float c10 =
            cmix(block[((lz)*n + ly + 1) * n + lx], block[((lz)*n + ly + 1) * n + lx + 1], fx);
        float c01 =
            cmix(block[((lz + 1) * n + ly) * n + lx], block[((lz + 1) * n + ly) * n + lx + 1], fx);
        float c11 = cmix(block[((lz + 1) * n + ly + 1) * n + lx],
                         block[((lz + 1) * n + ly + 1) * n + lx + 1], fx);
        return ctape_volume_outside(cmix(cmix(c00, c10, fy), cmix(c01, c11, fy), fz), outside);
    }
    if (op == ctape_swept) {
        int guide_off = (int)q[0];
        int guide_count = (int)q[1];
        int rec_off = (int)q[2];
        int profile_count = (int)q[3];
        if (guide_count < 2 || profile_count < 2) return CLAY_TAPE_FAR;

        CSweepHit hit = csweep_nearest(blob + guide_off, guide_count, lp);
        int seg = hit.seg;
        float t = hit.t;

        CLAY_DEVICE const float* va = blob + guide_off + seg * CLAY_SWEPT_VERTEX_FLOATS;
        CLAY_DEVICE const float* vb = va + CLAY_SWEPT_VERTEX_FLOATS;
        cfloat3 a = cf3(va[0], va[1], va[2]);
        cfloat3 b = cf3(vb[0], vb[1], vb[2]);
        cfloat3 tangent = b - a;
        float seg_len = clength(tangent);
        tangent = seg_len > 1e-9f ? tangent * (1.0f / seg_len) : cf3(0, 0, 1);

        // The two end normals were transported when the item compiled; lerp
        // and re-orthogonalize, which is enough for a polyline guide and
        // avoids a slerp in the inner loop.
        cfloat3 na = cf3(va[3], va[4], va[5]);
        cfloat3 nb = cf3(vb[3], vb[4], vb[5]);
        cfloat3 normal = cnormalize(cmix(na, nb, t));
        normal = cnormalize(normal - tangent * cdot(normal, tangent));
        cfloat3 binormal = ccross(tangent, normal);

        cfloat3 offset = lp - (a + (b - a) * t);
        // The tangential part of the offset is only non-zero where the nearest
        // point was CLAMPED to an end of the guide — everywhere else the
        // projection makes the offset perpendicular by construction. Beyond an
        // end it is the overshoot, and dropping it would make the sweep read
        // as if it extended forever.
        float axial = cdot(offset, tangent);
        cfloat3 perp = offset - tangent * axial;
        cfloat2 xy = cf2(cdot(perp, normal), cdot(perp, binormal));
        float overshoot = 0.0f;
        if (seg == 0 && t <= 0.0f) overshoot = cmax(-axial, 0.0f);
        if (seg == guide_count - 2 && t >= 1.0f) overshoot = cmax(axial, 0.0f);

        // Profiles are distributed by ARC LENGTH, so a guide whose vertices
        // bunch does not bunch the profiles.
        float total = blob[guide_off + (guide_count - 1) * CLAY_SWEPT_VERTEX_FLOATS + 6];
        float s = va[6] + t * seg_len;
        float u = cclamp(total > 1e-9f ? s / total : 0.0f, 0.0f, 1.0f) *
                  (float)(profile_count - 1);
        int i = (int)u;
        if (i > profile_count - 2) i = profile_count - 2;
        if (i < 0) i = 0;
        float f = cease((int)q[4], u - (float)i);
        float da = ctape_profile_dist(blob + rec_off + i * CLAY_TAPE_PROFILE_FLOATS, blob, xy);
        float db =
            ctape_profile_dist(blob + rec_off + (i + 1) * CLAY_TAPE_PROFILE_FLOATS, blob, xy);
        // The sweep is the tube INTERSECTED with the guide's span, so the
        // axial term has to be signed: negative inside the span, positive past
        // an end. `s` is clamped to the guide, so the overshoot from the
        // nearest-point clamp is added rather than folded in — without it a
        // point beyond the end would read as though it sat on the end face.
        float dend = cmax(-s, s - total) + overshoot;
        float d2 = cmix(da, db, f);
        cfloat2 w = cf2(d2, dend);
        return cmin(cmax(w.x, w.y), 0.0f) + clength(cmax(w, cf2(0.0f, 0.0f)));
    }
    if (op == ctape_revolve) {
        // exact lift: evaluate the profile in the (radius - offset, y) plane
        return ctape_profile_dist(q, blob, crevolve_point(lp, q[CLAY_TAPE_PROFILE_FLOATS]));
    }
    if (op == ctape_stroke) {
        int off = (int)q[1];
        int cnt = (int)q[2];
        return ctape_stroke_dist(blob + off, cnt, lp, q[0]);
    }
    if (op == ctape_armature) {
        int nodes = (int)q[1];
        int parents = (int)q[2];
        int cnt = (int)q[3];
        return ctape_armature_dist(blob + nodes, blob + parents, cnt, lp, q[0]);
    }
    return CLAY_TAPE_FAR;
}

// Transition weight for the morph modes (0 -> accumulated, 1 -> item).
CLAY_FN float ctape_transition_weight(int mode, CLAY_DEVICE const float* extra, cfloat3 p) {
    if (mode == ccombine_transition_linear) {
        return ctransition_linear_weight(p, cf3(extra[0], extra[1], extra[2]),
                                         cf3(extra[3], extra[4], extra[5]), (int)extra[6]);
    }
    return ctransition_radial_weight(p, extra[0], extra[1], (int)extra[2]);
}

CLAY_FN bool ctape_mode_is_transition(int mode) {
    return mode == ccombine_transition_linear || mode == ccombine_transition_radial;
}

CLAY_FN CTapeValue ctape_combine_values(CTapeValue a, CTapeValue b, int mode, int profile,
                                        float k, float r2) {
    CTapeValue r;
    r.d = a.d;
    r.color = a.color;
    if (mode == ccombine_add) {
        cfloat2 dm = ctape_smin_m(profile, a.d, b.d, k);
        r.d = dm.x;
        r.color = cmix(a.color, b.color, dm.y);
    } else if (mode == ccombine_subtract) {
        // b carves out of a: -smin(-a, b)
        r.d = -ctape_smin(profile, -a.d, b.d, k);
    } else if (mode == ccombine_intersect) {
        r.d = -ctape_smin(profile, -a.d, -b.d, k);
    } else if (mode == ccombine_paint) {
        // color-only, field untouched
        float support = cmax(ctape_blend_support(profile, k), k);
        float w = 1.0f - cclamp(b.d / cmax(support, 1e-6f), 0.0f, 1.0f);
        r.color = cmix(a.color, b.color, w);
    } else if (mode == ccombine_relief || mode == ccombine_incise) {
        // The item is a REGION, not a shape. Its own field gives the weight,
        // exactly as paint's does — full inside, tapering across the falloff,
        // and zero beyond it.
        //
        // Offsetting the accumulated distance moves its isosurface along that
        // field's own gradient, which IS the surface normal. So this displaces
        // the existing surface along its normal rather than approximating it.
        float width = cmax(r2, 1e-6f);
        float w = 1.0f - cclamp(b.d / width, 0.0f, 1.0f);
        // Smoothstep rather than linear: a linear taper leaves a visible crease
        // at the region's rim, where the weight's slope jumps from 1/width to 0.
        w = w * w * (3.0f - 2.0f * w);
        // One branch, two directions: relief lifts the surface out along its
        // normal, incise pushes it in. Sharing the body is how they stay each
        // other's inverse as either changes.
        r.d = a.d - (mode == ccombine_relief ? k : -k) * w;
        // The colour follows the material the relief moved, not the region's.
    } else if (mode == ccombine_groove) {
        r.d = op_groove(a.d, b.d, k, r2);
    } else if (mode == ccombine_tongue) {
        r.d = op_tongue(a.d, b.d, k, r2);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_pipe) {
        r.d = op_pipe(a.d, b.d, k);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_engrave) {
        r.d = op_engrave(a.d, b.d, k);
    } else if (mode == ccombine_emboss) {
        r.d = op_emboss(a.d, b.d, k);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_inset) {
        r.d = op_inset(a.d, b.d, k);
    } else if (mode == ccombine_shell) {
        r.d = op_shell_union(a.d, b.d, k);
        if (r.d < a.d) r.color = b.color;
    } else if (mode == ccombine_replace) {
        r.d = op_replace(a.d, b.d);
        if (b.d < 0.0f) r.color = b.color;
    }
    // unknown modes fall through as identity (forward compatibility)
    return r;
}

// One primitive evaluation at a local point: the deformer chain, then the
// primitive's distance function. Repetition calls this once per cell it
// needs (twice for radial arrays).
CLAY_FN float ctape_prim_local(unsigned int op, CLAY_DEVICE const float* pr,
                               CLAY_DEVICE const float* blob, cfloat3 lp) {
    CLAY_DEVICE const float* deform =
        pr + CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS + CLAY_TAPE_REPEAT_FLOATS;
    int deform_count = (int)deform[0];
    float offset = 0.0f;
    cfloat3 wp = lp;
    for (int di = 0; di < deform_count; ++di) {
        CLAY_DEVICE const float* rec = deform + 1 + di * CLAY_TAPE_DEFORM_FLOATS;
        offset += ctape_deform_offset(rec, wp);
        wp = ctape_deform_point(rec, wp);
    }
    return ctape_prim_dist(op, pr + CLAY_TAPE_PRIM_HEADER, blob, wp) + offset;
}

// The fixed interpreter every backend ships. Postfix stack machine; empty
// tapes evaluate to "far outside".
CLAY_FN CTapeValue ctape_eval(CLAY_DEVICE const CTapeInstr* instrs, int instr_count,
                              CLAY_DEVICE const float* params, CLAY_DEVICE const float* blob,
                              cfloat3 p) {
    CTapeValue stack[CLAY_TAPE_MAX_STACK];
    int top = 0;
    for (int i = 0; i < instr_count; ++i) {
        unsigned int op = instrs[i].op;
        CLAY_DEVICE const float* pr = params + instrs[i].param_offset;
        if (op == ctape_combine) {
            if (top < 1) continue;
            CTapeValue b = stack[top - 1];
            CTapeValue a;
            // A combine with an empty accumulator applies against empty
            // space: this is how material-creating modes (shell, replace)
            // seed a chain whose earlier items were culled or absent.
            a.d = CLAY_TAPE_FAR;
            a.color = b.color;
            if (top >= 2) {
                a = stack[top - 2];
                --top;
            }
            int mode = (int)pr[0];
            if (ctape_mode_is_transition(mode)) {
                // spatial morph: lerp BOTH operands by the weight at p. Not a
                // distance — the tape's tracked field info records that.
                float w = ctape_transition_weight(mode, pr + 4, p);
                CTapeValue r;
                r.d = cmix(a.d, b.d, w);
                r.color = cmix(a.color, b.color, w);
                stack[top - 1] = r;
            } else {
                stack[top - 1] = ctape_combine_values(a, b, mode, (int)pr[1], pr[2], pr[3]);
            }
        } else {
            if (top >= CLAY_TAPE_MAX_STACK) continue;
            cfloat4x4 inv;
            inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
            inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
            inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
            inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
            float scale = pr[12];
            float round = pr[13];
            CTapeValue v;
            v.color = cf3(pr[14], pr[15], pr[16]);
            cfloat3 lp = cmul_point(inv, p);
            CLAY_DEVICE const float* repeat = pr + CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS;
            float prim_value;  // NB: not `local` — reserved in OpenCL C
            if (!ctape_repeat_active(repeat)) {
                prim_value = ctape_prim_local(op, pr, blob, lp);
            } else if (ctape_repeat_is_radial(repeat)) {
                // O(2): the nearest angular sector and its neighbour, per
                // docs/01 2.4 — one evaluation would seam at sector borders
                float d0 = ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, 0));
                int neighbour = crep_radial_neighbor(lp, (int)repeat[1]);
                float d1 = ctape_prim_local(op, pr, blob,
                                            ctape_repeat_point(repeat, lp, neighbour));
                prim_value = cmin(d0, d1);
            } else {
                prim_value =
                    ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, 0));
            }
            v.d = prim_value * scale - round;
            stack[top] = v;
            ++top;
        }
    }
    if (top == 0) {
        CTapeValue far_out;
        far_out.d = CLAY_TAPE_FAR;
        far_out.color = cf3(0.5f, 0.5f, 0.5f);
        return far_out;
    }
    return stack[top - 1];
}

CLAY_NS_END
