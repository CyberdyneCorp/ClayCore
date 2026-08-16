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
    // FEATHERED replace of a sampled volume (add-feathered-volume-replace).
    // Emitted by the tape COMPILER when a volume item placed with Replace
    // carries a feather; it is not a public op — the node's op stays Replace,
    // and a feather of zero emits ccombine_replace exactly as before.
    //
    // The hard replace holds BOTH fields live at the surface, and a volume
    // baked from the document beneath it ties with that field at every sample
    // plane. min/max of two fields that touch and cross at the cell wavelength
    // is what corrugates the normals (issue #67): the field's zero set is
    // exact, but any finite-difference gradient across a branch switch pays
    // |b - a| over its own epsilon, at the cell wavelength. This mode holds
    // ONE field almost everywhere instead: the volume outright deep inside its
    // sampled box, the surrounding field outside, crossfaded over the feather
    // margin just inside the box faces.
    //
    // Extra params after the shared four: [4] the volume's blob header offset
    // (origin, cell, band, brick counts and feather are read from the same
    // header the prim reads, so the two cannot disagree), [5..16] the
    // world-to-local matrix of the instance, [17] its world scale.
    ccombine_replace_feather = 16,
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
    CLAY_UINT_T op;
    CLAY_UINT_T param_offset;
} CTapeInstr;

typedef struct CTapeValueT {
    float d;
    cfloat3 color;
} CTapeValue;

#define CLAY_TAPE_MAX_STACK 16
#define CLAY_TAPE_FAR 3.4e37f
#define CLAY_TAPE_PRIM_PARAMS 7
#define CLAY_TAPE_DEFORM_FLOATS 12
// An alpha's blob payload starts with [w] [h] [extent] [radius] [amplitude],
// then w*h samples. Named so the writer and the reader cannot disagree.
#define CLAY_TAPE_ALPHA_HEADER 5
// A combine record's shared prefix: [mode] [profile] [k] [rb] [gate offset],
// with any mode-specific extras after it. The gate lives in the SHARED part
// because it composes with every mode rather than being one — masking has to
// gate a boolean, and a boolean is a mode. -1 in the gate slot means none.
#define CLAY_TAPE_COMBINE_HEADER 5
// A gate's blob record: [volume handle] [inverse transform, 12] [scale] [width].
#define CLAY_TAPE_GATE_FLOATS 15
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
CLAY_FN cfloat3 ctape_repeat_point(CLAY_FPTR rec, cfloat3 p, int sector) {
    int type = CLAY_INT(CLAY_AT(rec, 0));
    if (type == crepeat_grid_infinite) return crep_inf_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)));
    if (type == crepeat_grid_finite)
        return crep_lim_point(p, CLAY_AT(rec, 1), cf3(CLAY_AT(rec, 4), CLAY_AT(rec, 5), CLAY_AT(rec, 6)));
    if (type == crepeat_radial) return crep_radial_point(p, CLAY_INT(CLAY_AT(rec, 1)), sector);
    return p;
}

CLAY_FN bool ctape_repeat_is_radial(CLAY_FPTR rec) {
    return CLAY_INT(CLAY_AT(rec, 0)) == crepeat_radial;
}

CLAY_FN bool ctape_repeat_active(CLAY_FPTR rec) {
    return CLAY_INT(CLAY_AT(rec, 0)) != crepeat_none;
}

// Domain warps an item can carry (deform.h). Values are serialization-stable.
enum CDeformType {
    cdeform_twist = 0,          // k = radians per unit about Y
    cdeform_bend = 1,           // k = radians per unit along X
    cdeform_taper = 2,          // k = y0, a = y1, b = s0, c = s1, ease
    cdeform_displace = 3,       // k = amplitude, a = frequency
    cdeform_wrap = 4,           // k = x0, a = x1: bend [x0,x1] about the Z axis
    cdeform_elongate = 5,       // k, a, b = per-axis half-extents to insert
    cdeform_bend_linear = 6,    // a(k,a,b) b(c,e0,e1) v(e2,e3,e4), eased
    cdeform_bend_radial = 7,    // k = r0, a = r1, b = dz, eased
    cdeform_elongate_axis = 8,  // k, a, b = half-extents; no correction
    cdeform_grab = 9,           // centre(k,a,b) radius(c) disp(e0,e1,e2) front(e3)
    cdeform_pose = 10,          // centre(k,a,b) radius(c) axis(e0,e1,e2) angle(e3)
    cdeform_pose_line = 11,     // a(k,a,b) b(c,e0,e1) axis(e2,e3,e4) angle(e5)
    // Radial scale about a centre. One signed strength: positive magnifies,
    // negative pinches. centre(k,a,b) radius(c) strength(e0)
    cdeform_magnify = 12,
    // Fractal gradient noise as a distance OFFSET, like displace — but
    // irregular, which is the whole point. k = amplitude, a = frequency,
    // b = octaves, c = gain, e0 = seed
    cdeform_noise = 13,
    // Ranged twist and bend: the same rotation as 0 and 1 with the angle
    // ramped across a span and held beyond it, which is what a gizmo's box
    // does. k = radians per unit, a = t0, b = t1, ease in slot 5.
    cdeform_twist_range = 14,
    cdeform_bend_range = 15,
    // Bend along a DRAWN guide rather than at a constant rate. The first
    // deformer whose payload is not a fixed size, so the guide lives in the
    // blob exactly as a sweep's does and the record holds a handle to it:
    // k = guide offset, a = guide vertex count, b = t0, c = t1.
    cdeform_bend_curve = 16,
    // A lattice cage, as the INVERSE warp (see clattice_point). The offsets are
    // blob-carried like a bend curve's guide; the box and divisions fit the
    // record exactly: k = blob offset, a/b/c = nx/ny/nz, ext[0..2] = box min,
    // ext[3..5] = box max.
    cdeform_lattice = 17,
    // The same cage through a TRANSFORM, so one world-placed cage can act on
    // items in any frame. Its own opcode rather than a flag, so the
    // axis-aligned path above pays nothing. k = blob offset (transform,
    // inverse, then offsets), a/b/c = nx/ny/nz, ext = the box in CAGE space.
    cdeform_lattice_xform = 18,
    // Blob: noise with finite support — the same fractal the whole-item noise
    // uses, under the same radial falloff grab and magnify use.
    // centre(k,a,b) radius(c) ease(5) amplitude(e0) frequency(e1) octaves(e2)
    // gain(e3) seed(e4)
    cdeform_blob = 19,
    // A caller-supplied 2D stamp as a distance offset — pores, fabric, scales.
    // The samples do not fit a record, so they ride in the blob as a bend
    // curve's guide and a lattice's offsets do:
    //   k = blob offset, centre(a,b,c), ease(5),
    //   dir(e0,e1,e2), tangent(e3,e4,e5)
    // which uses the record exactly. The three scalars that did not fit —
    // width, height, extent, radius, amplitude — head the blob payload:
    //   [w] [h] [extent] [radius] [amplitude] then w*h samples
    // Putting them there rather than squeezing the frame out of the record
    // keeps the TANGENT, and a stamp that cannot be rotated is one an artist
    // cannot align to a seam.
    cdeform_alpha = 20,
};

// Apply one deformer record to the local point. No deformer corrects the
// distance: safety is carried by the tape's tracked Lipschitz factor, which
// also keeps influence bounds tight (see the taper note below).
// `blob` is threaded through for the deformers whose payload does not fit a
// fixed record; the ones that carry everything inline ignore it.
CLAY_FN cfloat3 ctape_deform_point(CLAY_FPTR rec, CLAY_FPTR blob, cfloat3 p) {
    int type = CLAY_INT(CLAY_AT(rec, 0));
    if (type == cdeform_twist) return ctwist_point(p, CLAY_AT(rec, 1));
    if (type == cdeform_bend) return cbend_point(p, CLAY_AT(rec, 1));
    if (type == cdeform_twist_range) {
        return ctwist_range_point(p, CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3),
                                  CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_bend_range) {
        return cbend_range_point(p, CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3),
                                 CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_taper) {
        // NOTE: deliberately no ctaper_dist here. Multiplying the distance by
        // min(s,1) would keep the field conservative on its own, but it also
        // shrinks the field everywhere, which makes the item's influence
        // reach 1/s further than its geometry — and influence bounds are what
        // brick culling trusts. Instead the taper behaves like twist and bend:
        // the raw warped field, with safety carried by the tape's tracked
        // Lipschitz factor (cfi_taper includes the 1/s_min stretch).
        return ctaper_point(p, CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3), CLAY_AT(rec, 4), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_wrap) return cwrap_around_point(p, CLAY_AT(rec, 1), CLAY_AT(rec, 2));
    if (type == cdeform_bend_linear) {
        return cbend_linear_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)),
                                  cf3(CLAY_AT(rec, 4), CLAY_AT(rec, 6), CLAY_AT(rec, 7)),
                                  cf3(CLAY_AT(rec, 8), CLAY_AT(rec, 9), CLAY_AT(rec, 10)), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_bend_radial) {
        return cbend_radial_point(p, CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_grab) {
        return cgrab_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)), CLAY_AT(rec, 4),
                           cf3(CLAY_AT(rec, 6), CLAY_AT(rec, 7), CLAY_AT(rec, 8)), CLAY_AT(rec, 9), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_pose_line) {
        return cpose_line_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)), cf3(CLAY_AT(rec, 4), CLAY_AT(rec, 6), CLAY_AT(rec, 7)),
                                cf3(CLAY_AT(rec, 8), CLAY_AT(rec, 9), CLAY_AT(rec, 10)), CLAY_AT(rec, 11), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_pose) {
        return cpose_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)), CLAY_AT(rec, 4),
                           cf3(CLAY_AT(rec, 6), CLAY_AT(rec, 7), CLAY_AT(rec, 8)), CLAY_AT(rec, 9), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_magnify) {
        return cmagnify_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)), CLAY_AT(rec, 4), CLAY_AT(rec, 6), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_elongate_axis)
        return celongate_axis_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)));
    if (type == cdeform_lattice) {
        return clattice_point(CLAY_OFF(blob, CLAY_INT(CLAY_AT(rec, 1))), CLAY_INT(CLAY_AT(rec, 2)),
                              CLAY_INT(CLAY_AT(rec, 3)), CLAY_INT(CLAY_AT(rec, 4)),
                              cf3(CLAY_AT(rec, 6), CLAY_AT(rec, 7), CLAY_AT(rec, 8)),
                              cf3(CLAY_AT(rec, 9), CLAY_AT(rec, 10), CLAY_AT(rec, 11)), p);
    }
    if (type == cdeform_lattice_xform) {
        return clattice_xform_point(CLAY_OFF(blob, CLAY_INT(CLAY_AT(rec, 1))),
                                    CLAY_INT(CLAY_AT(rec, 2)), CLAY_INT(CLAY_AT(rec, 3)),
                                    CLAY_INT(CLAY_AT(rec, 4)),
                                    cf3(CLAY_AT(rec, 6), CLAY_AT(rec, 7), CLAY_AT(rec, 8)),
                                    cf3(CLAY_AT(rec, 9), CLAY_AT(rec, 10), CLAY_AT(rec, 11)), p);
    }
    if (type == cdeform_bend_curve) {
        return cbend_curve_point(CLAY_OFF(blob, CLAY_INT(CLAY_AT(rec, 1))),
                                 CLAY_INT(CLAY_AT(rec, 2)), p, CLAY_AT(rec, 3), CLAY_AT(rec, 4));
    }
    if (type == cdeform_elongate) {
        // The correction rides ctape_deform_offset, which the chain evaluates
        // at this same pre-warp point — exactly what elongation needs.
        float unused = 0.0f;
        return celongate_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)), CLAY_OUTARG(unused));
    }
    return p;  // displace acts on the distance, not the point
}

// Post-primitive distance contribution of one deformer (0 for point warps).
// `blob` is threaded through for the same reason ctape_deform_point takes it:
// an alpha's samples do not fit a record.
CLAY_FN float ctape_deform_offset(CLAY_FPTR rec, CLAY_FPTR blob, cfloat3 p) {
    int type = CLAY_INT(CLAY_AT(rec, 0));
    if (type == cdeform_elongate) {
        float correction = 0.0f;
        celongate_point(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)), CLAY_OUTARG(correction));
        return correction;
    }
    if (type == cdeform_noise) {
        int octaves = CLAY_INT(CLAY_AT(rec, 3));
        if (octaves < 1) octaves = 1;
        if (octaves > 8) octaves = 8;  // past this the octaves are below a cell
        return CLAY_AT(rec, 1) * cnoise_fbm(p * CLAY_AT(rec, 2), octaves, CLAY_AT(rec, 4), CLAY_UINT(CLAY_AT(rec, 6)));
    }
    if (type == cdeform_blob) {
        int octaves = CLAY_INT(CLAY_AT(rec, 8));
        if (octaves < 1) octaves = 1;
        if (octaves > 8) octaves = 8;  // past this the octaves are below a cell
        return cblob_offset(p, cf3(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3)),
                            CLAY_AT(rec, 4), CLAY_AT(rec, 6), CLAY_AT(rec, 7), octaves,
                            CLAY_AT(rec, 9), CLAY_UINT(CLAY_AT(rec, 10)),
                            CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type == cdeform_alpha) {
        int head = CLAY_INT(CLAY_AT(rec, 1));
        return calpha_offset(p, blob + head + CLAY_TAPE_ALPHA_HEADER, CLAY_INT(CLAY_AT(blob, head)),
                             CLAY_INT(CLAY_AT(blob, head + 1)),
                             cf3(CLAY_AT(rec, 2), CLAY_AT(rec, 3), CLAY_AT(rec, 4)),
                             cf3(CLAY_AT(rec, 6), CLAY_AT(rec, 7), CLAY_AT(rec, 8)),
                             cf3(CLAY_AT(rec, 9), CLAY_AT(rec, 10), CLAY_AT(rec, 11)),
                             CLAY_AT(blob, head + 2), CLAY_AT(blob, head + 3),
                             CLAY_AT(blob, head + 4), CLAY_INT(CLAY_AT(rec, 5)));
    }
    if (type != cdeform_displace) return 0.0f;
    float amp = CLAY_AT(rec, 1);
    float freq = CLAY_AT(rec, 2);
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
    // The feathered replace deviates from `a` only inside the sampled box,
    // which IS the item's geometry bound: support 0, exactly as the hard one.
    if (mode == ccombine_replace_feather) return 0.0f;
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

CLAY_FN float ctape_stroke_dist(CLAY_FPTR pts, int count, cfloat3 p,
                                float blend_k) {
    if (count <= 0) return CLAY_TAPE_FAR;
    cfloat3 a0 = cf3(CLAY_AT(pts, 0), CLAY_AT(pts, 1), CLAY_AT(pts, 2));
    if (count == 1) return sd_sphere(p - a0, CLAY_AT(pts, 3));
    float d = CLAY_TAPE_FAR;
    for (int i = 0; i + 1 < count; ++i) {
        cfloat3 a = cf3(CLAY_AT(pts, i * 4 + 0), CLAY_AT(pts, i * 4 + 1), CLAY_AT(pts, i * 4 + 2));
        cfloat3 b = cf3(CLAY_AT(pts, i * 4 + 4), CLAY_AT(pts, i * 4 + 5), CLAY_AT(pts, i * 4 + 6));
        float seg = csweep_link(p, a, b, CLAY_AT(pts, i * 4 + 3), CLAY_AT(pts, i * 4 + 7));
        d = (blend_k > 0.0f) ? csmin_quadratic(d, seg, blend_k) : cmin(d, seg);
    }
    return d;
}

// A tree of spheres. `nodes` is count * 4 floats (x, y, z, radius); `parents`
// is count floats, each the index of that node's parent; `signs` is count
// floats, +1 or -1 per node. A node whose parent is itself is a root and
// contributes a bare sphere, so a one-node armature is a sphere rather than
// nothing.
//
// The signed rule is one sentence: the armature of the positive nodes, MINUS
// the armature of the negative nodes (ZBrush's negative ZSphere), each half
// built exactly as the unsigned armature is. A link therefore exists only
// between two nodes of the SAME sign — skin between builders, carve between
// carvers — and a link whose ends disagree belongs to neither half: skin
// along a negative's links is suppressed rather than drawn, which is the
// membrane cut, and a carve never sweeps a positive parent's radius, which is
// what keeps an eye-socket child from swallowing the head it is cut into. A
// node whose parent has the other sign reads as a root of its own half, and
// the referenced-root suppression is applied per half for the same reason it
// exists at all: two overlapping terms are wrong under a soft fold, subtracted
// exactly as unioned.
//
// The carve is subtracted AFTER the whole positive fold, so a sleeve from any
// other branch that runs through a hollow is cut by it and no later union can
// re-fill it — what the host-side workaround of a trailing subtract sphere
// cannot express.
//
// The fold order is ascending node index within each half, positives first,
// and that is a REQUIREMENT rather than an implementation detail:
// csmin_quadratic is not associative, and neither is the subtraction that
// follows it, so three links meeting at a hip give a different field
// depending on the order they combine. Fixing the order here is what makes an
// armature evaluate the same on every backend. When every sign is positive
// the first pass is instruction-for-instruction the pre-signs fold, which is
// what keeps a chain armature equal to its stroke.
CLAY_FN float ctape_armature_dist(CLAY_FPTR nodes,
                                  CLAY_FPTR parents, CLAY_FPTR signs, int count,
                                  cfloat3 p, float blend_k) {
    if (count <= 0) return CLAY_TAPE_FAR;
    float d = CLAY_TAPE_FAR;
    for (int pass = 0; pass < 2; ++pass) {
        bool negative = pass == 1;
        for (int i = 0; i < count; ++i) {
            if ((CLAY_AT(signs, i) < 0.0f) != negative) continue;
            int j = CLAY_INT(CLAY_AT(parents, i));
            if (j < 0 || j >= count) j = i;
            // A parent of the other sign is no parent in this half: the node
            // reads as a root of its own half.
            if (j != i && (CLAY_AT(signs, j) < 0.0f) != negative) j = i;
            cfloat3 a = cf3(CLAY_AT(nodes, i * 4 + 0), CLAY_AT(nodes, i * 4 + 1), CLAY_AT(nodes, i * 4 + 2));
            float ra = CLAY_AT(nodes, i * 4 + 3);
            float seg;
            if (j == i) {
                // A root. Its sphere is ALREADY inside every same-sign link
                // that names it, so contributing it again is redundant with a
                // hard fold and wrong with a soft one: smooth-min of two
                // overlapping terms pulls the surface outward — and the
                // subtracted half over-carves the same way — so a chain
                // armature would stop matching the stroke it is supposed to
                // equal. Contribute it only when nothing else in ITS HALF
                // does — an isolated node, which is the case that would
                // otherwise vanish. Roots are few, so the scan costs nothing
                // in practice.
                bool referenced = false;
                for (int k = 0; k < count; ++k) {
                    if (k == i) continue;
                    if ((CLAY_AT(signs, k) < 0.0f) != negative) continue;
                    int pk = CLAY_INT(CLAY_AT(parents, k));
                    if (pk == i) { referenced = true; break; }
                }
                if (referenced) continue;
                seg = sd_sphere(p - a, ra);
            } else {
                cfloat3 b = cf3(CLAY_AT(nodes, j * 4 + 0), CLAY_AT(nodes, j * 4 + 1), CLAY_AT(nodes, j * 4 + 2));
                seg = csweep_link(p, a, b, ra, CLAY_AT(nodes, j * 4 + 3));
            }
            if (negative) {
                // The carve half comes out of the skin half: the subtract
                // idiom ctape_combine_values uses, hard where the blend is.
                d = (blend_k > 0.0f) ? -csmin_quadratic(-d, seg, blend_k) : cmax(d, -seg);
            } else {
                d = (blend_k > 0.0f) ? csmin_quadratic(d, seg, blend_k) : cmin(d, seg);
            }
        }
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
// Packed 0x00RRGGBB carried as a float VALUE, not as reinterpreted bits: the
// packed colour is at most 2^24 - 1 and float32 holds every integer to 2^24
// exactly, so this needs no bit-cast — which matters here, because a bit-cast
// is spelled differently in every dialect this header compiles as.
CLAY_FN cfloat3 cmix3(cfloat3 a, cfloat3 b, float t) {
    return cf3(cmix(a.x, b.x, t), cmix(a.y, b.y, t), cmix(a.z, b.z, t));
}

CLAY_FN cfloat3 cunpack_color(float packed) {
    float p = cmax(packed, 0.0f);
    float r = cfloor(p * (1.0f / 65536.0f));
    float g = cfloor((p - r * 65536.0f) * (1.0f / 256.0f));
    float b = p - r * 65536.0f - g * 256.0f;
    return cf3(r * (1.0f / 255.0f), g * (1.0f / 255.0f), b * (1.0f / 255.0f));
}

CLAY_FN float ctape_volume_outside(float inner, float outside) {
    if (outside <= 0.0f) return inner;
    float reach = cmax(inner, 0.0f);
    return csqrt(outside * outside + reach * reach);
}

// Samples per brick edge in a sampled volume. Bricks store one extra sample
// per axis — a halo — so a trilinear tap inside a brick never needs its
// neighbour, which is what makes the lookup a single array read.
#define CLAY_BRICK_DIM 8

CLAY_FN float ctape_profile_dist(CLAY_FPTR prof, CLAY_FPTR blob,
                                 cfloat2 p) {
    int type = CLAY_INT(CLAY_AT(prof, 0));
    if (type == cprofile_circle) return sd_circle2(p, CLAY_AT(prof, 1));
    if (type == cprofile_box) return sd_box2(p, cf2(CLAY_AT(prof, 1), CLAY_AT(prof, 2)));
    if (type == cprofile_hexagon) return sd_hexagon2(p, CLAY_AT(prof, 1));
    if (type == cprofile_triangle) return sd_equilateral_triangle2(p, CLAY_AT(prof, 1));
    if (type == cprofile_trapezoid) return sd_trapezoid2(p, CLAY_AT(prof, 1), CLAY_AT(prof, 2), CLAY_AT(prof, 3));
    if (type == cprofile_vesica) return sd_vesica2(p, CLAY_AT(prof, 1), CLAY_AT(prof, 2));
    if (type == cprofile_polygon) {
        int off = CLAY_INT(CLAY_AT(prof, 1));
        int count = CLAY_INT(CLAY_AT(prof, 2));
        if (count < 3) return CLAY_TAPE_FAR;
        // vertices live in the out-of-line pool as consecutive (x, y) pairs;
        // sd_polygon2 walks them without materializing an array
        return sd_polygon2_raw(CLAY_OFF(blob, off), count, p);
    }
    return CLAY_TAPE_FAR;
}

// `out_color` is INOUT rather than out, and that is load-bearing: the caller
// SEEDS it with the item's own colour and a prim that has no colour of its own
// leaves it alone. GLSL's `out` is copy-out, so an unwritten parameter would
// come back as garbage there while working everywhere else — which is exactly
// what the Vulkan parity run caught. Only a sampled volume carrying colour
// writes it, and only where it has samples, so the caller reads back whichever
// applies — which is how one opcode gains a per-sample colour without every
// other prim paying for a colour it does not have. Widening the return type to
// CTapeValue was the alternative and would have charged all of them.
// The ONE opcode that writes colour, lifted out of ctape_prim_dist so that
// the other forty do not carry an out-parameter they never touch. See the
// dispatch in ctape_prim_local for why that is worth a function.
CLAY_FN float ctape_volume_dist(CLAY_FPTR q, CLAY_FPTR blob, cfloat3 lp,
                                CLAY_INOUT(cfloat3) out_color) {
    CLAY_FPTR h = blob + CLAY_INT(CLAY_AT(q, 0));
    cfloat3 origin = cf3(CLAY_AT(h, 0), CLAY_AT(h, 1), CLAY_AT(h, 2));
    float cell = CLAY_AT(h, 3);
    int bcx = CLAY_INT(CLAY_AT(h, 5)), bcy = CLAY_INT(CLAY_AT(h, 6)), bcz = CLAY_INT(CLAY_AT(h, 7));
    int index_off = CLAY_INT(CLAY_AT(h, 8)), far_off = CLAY_INT(CLAY_AT(h, 9)), data_off = CLAY_INT(CLAY_AT(h, 10));
    if (bcx <= 0 || bcy <= 0 || bcz <= 0 || cell <= 0.0f) return CLAY_TAPE_FAR;

    float span_x = CLAY_FLOATC((bcx * CLAY_BRICK_DIM)) * cell;
    float span_y = CLAY_FLOATC((bcy * CLAY_BRICK_DIM)) * cell;
    float span_z = CLAY_FLOATC((bcz * CLAY_BRICK_DIM)) * cell;
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
    int cx = CLAY_INT(cclamp(cfloor(gx), 0.0f, CLAY_FLOATC((bcx * CLAY_BRICK_DIM - 1))));
    int cy = CLAY_INT(cclamp(cfloor(gy), 0.0f, CLAY_FLOATC((bcy * CLAY_BRICK_DIM - 1))));
    int cz = CLAY_INT(cclamp(cfloor(gz), 0.0f, CLAY_FLOATC((bcz * CLAY_BRICK_DIM - 1))));
    int bx = cx / CLAY_BRICK_DIM, by = cy / CLAY_BRICK_DIM, bz = cz / CLAY_BRICK_DIM;
    int slot = (bz * bcy + by) * bcx + bx;
    // Relative to the VOLUME's own blob, not the tape's. to_blob writes
    // index/far/data offsets past its own 12-float header, so they are only
    // absolute when the volume happens to sit at blob offset 0 — which is
    // true exactly when nothing else out-of-line was emitted before it. A
    // stroke, loft, swept or armature earlier in the layer puts its payload
    // there first, and the volume then read whatever those had written.
    int entry = CLAY_INT(CLAY_AT(h, index_off + slot));
    // An empty brick carries its own signed lower bound: the gap in bricks
    // to the nearest brick that HAS samples, floored at the band less half
    // a cell diagonal. A flat band width would be conservative but useless
    // — the marcher would creep across the empty majority of the region in
    // steps that never grew. See FieldVolume::far_value().
    if (entry < 0) return ctape_volume_outside(CLAY_AT(h, far_off + slot), outside);

    CLAY_FPTR block = CLAY_OFF(h, data_off + entry);
    int lx = cx - bx * CLAY_BRICK_DIM;
    int ly = cy - by * CLAY_BRICK_DIM;
    int lz = cz - bz * CLAY_BRICK_DIM;
    float fx = cclamp(gx - CLAY_FLOATC(cx), 0.0f, 1.0f);
    float fy = cclamp(gy - CLAY_FLOATC(cy), 0.0f, 1.0f);
    float fz = cclamp(gz - CLAY_FLOATC(cz), 0.0f, 1.0f);
    const int n = CLAY_BRICK_DIM + 1;
    // The halo is why this needs no neighbouring brick: lx+1 is always in
    // range, so a trilinear tap is one array read.
    float c00 = cmix(CLAY_AT(block, ((lz)*n + ly) * n + lx), CLAY_AT(block, ((lz)*n + ly) * n + lx + 1), fx);
    float c10 =
        cmix(CLAY_AT(block, ((lz)*n + ly + 1) * n + lx), CLAY_AT(block, ((lz)*n + ly + 1) * n + lx + 1), fx);
    float c01 =
        cmix(CLAY_AT(block, ((lz + 1) * n + ly) * n + lx), CLAY_AT(block, ((lz + 1) * n + ly) * n + lx + 1), fx);
    float c11 = cmix(CLAY_AT(block, ((lz + 1) * n + ly + 1) * n + lx),
                     CLAY_AT(block, ((lz + 1) * n + ly + 1) * n + lx + 1), fx);

    // Slot 13 is the colour offset, 0 when this volume carries none — and
    // a volume written before colour existed has a 13-float header and no
    // slot 13 at all, which is why the header size decides whether to look
    // rather than the blob's length. Colour is interpolated at the SAME
    // eight samples as the distance: a nearest-sample read would facet a
    // surface that has none.
    int header_size = index_off;
    if (header_size >= 14) {
        int color_off = CLAY_INT(CLAY_AT(h, 13));
        if (color_off > 0) {
            CLAY_FPTR cblock = CLAY_OFF(h, color_off + entry);
            // UNPACK BEFORE MIXING. Interpolating the packed words and
            // unpacking the result mixes the channels through their own
            // carries — a green halfway between two blues, and a blue that
            // is whatever the arithmetic left over. Each corner becomes a
            // colour first, and the mix is then an ordinary colour mix.
            cfloat3 q000 = cunpack_color(CLAY_AT(cblock, ((lz)*n + ly) * n + lx));
            cfloat3 q100 = cunpack_color(CLAY_AT(cblock, ((lz)*n + ly) * n + lx + 1));
            cfloat3 q010 = cunpack_color(CLAY_AT(cblock, ((lz)*n + ly + 1) * n + lx));
            cfloat3 q110 = cunpack_color(CLAY_AT(cblock, ((lz)*n + ly + 1) * n + lx + 1));
            cfloat3 q001 = cunpack_color(CLAY_AT(cblock, ((lz + 1) * n + ly) * n + lx));
            cfloat3 q101 = cunpack_color(CLAY_AT(cblock, ((lz + 1) * n + ly) * n + lx + 1));
            cfloat3 q011 = cunpack_color(CLAY_AT(cblock, ((lz + 1) * n + ly + 1) * n + lx));
            cfloat3 q111 =
                cunpack_color(CLAY_AT(cblock, ((lz + 1) * n + ly + 1) * n + lx + 1));
            cfloat3 k00 = cmix3(q000, q100, fx);
            cfloat3 k10 = cmix3(q010, q110, fx);
            cfloat3 k01 = cmix3(q001, q101, fx);
            cfloat3 k11 = cmix3(q011, q111, fx);
            CLAY_SET(out_color, cmix3(cmix3(k00, k10, fy), cmix3(k01, k11, fy), fz));
        }
    }
    return ctape_volume_outside(cmix(cmix(c00, c10, fy), cmix(c01, c11, fy), fz), outside);
    return CLAY_TAPE_FAR;
}

CLAY_FN float ctape_prim_dist(CLAY_UINT_T op, CLAY_FPTR q,
                              CLAY_FPTR blob, cfloat3 lp) {
    // q points at the prim-specific params (after xform/scale/round/color).
    if (op == ctape_sphere) return sd_sphere(lp, CLAY_AT(q, 0));
    if (op == ctape_box) return sd_box(lp, cf3(CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2)));
    if (op == ctape_round_box) return sd_round_box(lp, cf3(CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2)), CLAY_AT(q, 3));
    if (op == ctape_box_frame) return sd_box_frame(lp, cf3(CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2)), CLAY_AT(q, 3));
    if (op == ctape_torus) return sd_torus(lp, CLAY_AT(q, 0), CLAY_AT(q, 1));
    if (op == ctape_capsule)
        return sd_capsule(lp, cf3(CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2)), cf3(CLAY_AT(q, 3), CLAY_AT(q, 4), CLAY_AT(q, 5)), CLAY_AT(q, 6));
    if (op == ctape_capped_cylinder) return sd_capped_cylinder(lp, CLAY_AT(q, 0), CLAY_AT(q, 1));
    if (op == ctape_rounded_cylinder) return sd_rounded_cylinder(lp, CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2));
    if (op == ctape_capped_cone) return sd_capped_cone(lp, CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2));
    if (op == ctape_round_cone) return sd_round_cone(lp, CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2));
    if (op == ctape_ellipsoid) return sd_ellipsoid_bound(lp, cf3(CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2)));
    if (op == ctape_octahedron) return sd_octahedron(lp, CLAY_AT(q, 0));
    if (op == ctape_hex_prism) return sd_hex_prism(lp, cf2(CLAY_AT(q, 0), CLAY_AT(q, 1)));
    if (op == ctape_pyramid) return sd_pyramid(lp, CLAY_AT(q, 0));
    if (op == ctape_capped_torus) return sd_capped_torus(lp, cf2(CLAY_AT(q, 0), CLAY_AT(q, 1)), CLAY_AT(q, 2), CLAY_AT(q, 3));
    if (op == ctape_link) return sd_link(lp, CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2));
    if (op == ctape_cylinder_inf) return sd_cylinder_inf(lp, cf2(CLAY_AT(q, 0), CLAY_AT(q, 1)), CLAY_AT(q, 2));
    if (op == ctape_cone) return sd_cone(lp, cf2(CLAY_AT(q, 0), CLAY_AT(q, 1)), CLAY_AT(q, 2));
    if (op == ctape_plane) return sd_plane(lp, cf3(CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2)), CLAY_AT(q, 3));
    if (op == ctape_cut_sphere) return sd_cut_sphere(lp, CLAY_AT(q, 0), CLAY_AT(q, 1));
    if (op == ctape_cut_hollow_sphere) return sd_cut_hollow_sphere(lp, CLAY_AT(q, 0), CLAY_AT(q, 1), CLAY_AT(q, 2));
    if (op == ctape_solid_angle) return sd_solid_angle(lp, cf2(CLAY_AT(q, 0), CLAY_AT(q, 1)), CLAY_AT(q, 2));
    if (op == ctape_tetrahedron) return sd_tetrahedron(lp, CLAY_AT(q, 0));
    if (op == ctape_dodecahedron) return sd_dodecahedron(lp, CLAY_AT(q, 0));
    if (op == ctape_icosahedron) return sd_icosahedron(lp, CLAY_AT(q, 0));
    if (op == ctape_tri_prism) return sd_tri_prism_bound(lp, cf2(CLAY_AT(q, 0), CLAY_AT(q, 1)));
    if (op == ctape_octahedron_cheap) return sd_octahedron_bound(lp, CLAY_AT(q, 0));
    if (op == ctape_lnorm_sphere) return sd_lnorm_sphere_bound(lp, CLAY_AT(q, 0), CLAY_AT(q, 1));
    if (op == ctape_extrude) {
        // exact lift: profile distance merged with the axial slab
        return cop_extrude(ctape_profile_dist(q, blob, cf2(lp.x, lp.y)), lp.z,
                           CLAY_AT(q, CLAY_TAPE_PROFILE_FLOATS));
    }
    if (op == ctape_loft) {
        int off = CLAY_INT(CLAY_AT(q, 2));
        int count = CLAY_INT(CLAY_AT(q, 3));
        float h = CLAY_AT(q, 0);
        // Two records are always read below, so fewer than two profiles would
        // read a record that was never written — a null blob when the loft is
        // the only item. ctape_swept guards the same way; a document loaded
        // from disk can carry any count.
        if (count < 2) return CLAY_TAPE_FAR;
        // Bracket among the profiles: they sit evenly along the depth, so the
        // span index falls straight out of the normalized height.
        float t = cclamp((lp.z + h) / cmax(2.0f * h, 1e-9f), 0.0f, 1.0f) * CLAY_FLOATC((count - 1));
        int i = CLAY_INT(t);
        if (i > count - 2) i = count - 2;
        if (i < 0) i = 0;
        float u = cease(CLAY_INT(CLAY_AT(q, 1)), t - CLAY_FLOATC(i));
        cfloat2 xy = cf2(lp.x, lp.y);
        float da = ctape_profile_dist(CLAY_OFF(blob, off + i * CLAY_TAPE_PROFILE_FLOATS), blob, xy);
        float db = ctape_profile_dist(CLAY_OFF(blob, off + (i + 1) * CLAY_TAPE_PROFILE_FLOATS), blob, xy);
        return cop_loft(da, db, u, lp.z, h);
    }
    if (op == ctape_swept) {
        int guide_off = CLAY_INT(CLAY_AT(q, 0));
        int guide_count = CLAY_INT(CLAY_AT(q, 1));
        int rec_off = CLAY_INT(CLAY_AT(q, 2));
        int profile_count = CLAY_INT(CLAY_AT(q, 3));
        if (guide_count < 2 || profile_count < 2) return CLAY_TAPE_FAR;

        CLAY_FPTR guide = CLAY_OFF(blob, guide_off);
        CSweepHit hit = csweep_nearest(guide, guide_count, lp);
        int seg = hit.seg;
        float t = hit.t;

        // The frame construction is shared with the bend-along-a-curve
        // deformer, which asks this same question backwards.
        CSweepFrame fr = csweep_frame(guide, hit);
        cfloat3 tangent = fr.tangent;
        cfloat3 normal = fr.normal;
        cfloat3 binormal = fr.binormal;

        cfloat3 offset = lp - fr.origin;
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
        float total = csweep_length(guide, guide_count);
        float s = fr.arclen;
        float u = cclamp(total > 1e-9f ? s / total : 0.0f, 0.0f, 1.0f) *
                  CLAY_FLOATC((profile_count - 1));
        int i = CLAY_INT(u);
        if (i > profile_count - 2) i = profile_count - 2;
        if (i < 0) i = 0;
        float f = cease(CLAY_INT(CLAY_AT(q, 4)), u - CLAY_FLOATC(i));
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
        return ctape_profile_dist(q, blob, crevolve_point(lp, CLAY_AT(q, CLAY_TAPE_PROFILE_FLOATS)));
    }
    if (op == ctape_stroke) {
        int off = CLAY_INT(CLAY_AT(q, 1));
        int cnt = CLAY_INT(CLAY_AT(q, 2));
        return ctape_stroke_dist(CLAY_OFF(blob, off), cnt, lp, CLAY_AT(q, 0));
    }
    if (op == ctape_armature) {
        int nodes = CLAY_INT(CLAY_AT(q, 1));
        int parents = CLAY_INT(CLAY_AT(q, 2));
        int cnt = CLAY_INT(CLAY_AT(q, 3));
        int signs = CLAY_INT(CLAY_AT(q, 4));
        return ctape_armature_dist(blob + nodes, blob + parents, blob + signs, cnt, lp,
                                   CLAY_AT(q, 0));
    }
    return CLAY_TAPE_FAR;
}

// Transition weight for the morph modes (0 -> accumulated, 1 -> item).
CLAY_FN float ctape_transition_weight(int mode, CLAY_FPTR extra, cfloat3 p) {
    if (mode == ccombine_transition_linear) {
        return ctransition_linear_weight(p, cf3(CLAY_AT(extra, 0), CLAY_AT(extra, 1), CLAY_AT(extra, 2)),
                                         cf3(CLAY_AT(extra, 3), CLAY_AT(extra, 4), CLAY_AT(extra, 5)), CLAY_INT(CLAY_AT(extra, 6)));
    }
    return ctransition_radial_weight(p, CLAY_AT(extra, 0), CLAY_AT(extra, 1), CLAY_INT(CLAY_AT(extra, 2)));
}

CLAY_FN bool ctape_mode_is_transition(int mode) {
    return mode == ccombine_transition_linear || mode == ccombine_transition_radial;
}

// The feathered replace (ccombine_replace_feather). rec points past the four
// shared combine params: [0] volume header offset in the blob, [1..12] the
// instance's world-to-local matrix, [13] its world scale.
//
//     r = a + w(p) * clamp(b - a, -band, band)
//
// w is a smoothstep of the Chebyshev inset into the sampled box over the
// feather width: 0 at and outside the box faces — the surrounding field
// continues untouched, which is what removes the hard box edge — and 1 a
// feather inside them, where the result IS the volume and only one gradient
// survives, which is what removes the corrugation.
//
// The correction is clamped at the volume's band, and that clamp is what two
// other contracts hang off. Lipschitz: the crossfade adds at most
// band * 1.5 / feather to the declared slope (cfi_replace_feather), because
// that is the largest value the weight's own gradient can multiply. Per-brick
// culling: a dropped item can shift `a` only where `a` already exceeded the
// cull dilation, and a correction no larger than the band cannot pull such a
// value back inside the brick's clamp — provided the compiler widened the
// cull test by this band, which it does (compile-time counterpart in
// tape_build.cpp). A surface the volume moved FURTHER than its band is
// therefore expressed only up to the band; bake with a band that covers the
// verb, which is the same contract the volume's own accuracy already states.
// A GATE on an item's participation: 1 where the item is fully protected from
// acting, 0 where it acts unhindered.
//
// The payload is blob-carried because it is fifteen floats and a combine record
// cannot afford them: [volume handle] [inverse transform, 12] [scale] [width].
// The volume itself is an ordinary sampled field — the signed distance to the
// protected region, negative inside — so evaluating it is `ctape_volume_dist`
// and nothing new.
//
// The falloff is a smoothstep across `width` centred on the region's boundary,
// which is the whole reason the payload is a DISTANCE rather than paint: a
// distance is 1-Lipschitz, so the weight's own slope is 1.5/width, a number the
// caller chose. Paint values would have made it whatever the artist's brush
// edge happened to be.
//
// A width of zero would be a step with no finite Lipschitz bound, so it is
// clamped rather than honoured — a gate that made the field unmarchable would
// be a worse answer than a gate that is very slightly soft.
CLAY_FN float ctape_gate_weight(CLAY_FPTR rec, CLAY_FPTR blob, cfloat3 p) {
    float width = cmax(CLAY_AT(rec, 14), 1e-4f);

    cfloat4x4 inv;
    inv.c0 = cf4(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3), 0.0f);
    inv.c1 = cf4(CLAY_AT(rec, 4), CLAY_AT(rec, 5), CLAY_AT(rec, 6), 0.0f);
    inv.c2 = cf4(CLAY_AT(rec, 7), CLAY_AT(rec, 8), CLAY_AT(rec, 9), 0.0f);
    inv.c3 = cf4(CLAY_AT(rec, 10), CLAY_AT(rec, 11), CLAY_AT(rec, 12), 1.0f);
    cfloat3 lp = cmul_point(inv, p);

    cfloat3 ignored = cf3(0.0f, 0.0f, 0.0f);
    // The stored distance is in the gate's own space; the scale carries it back
    // to world units, exactly as a placed volume's does.
    float d = ctape_volume_dist(rec, blob, lp, CLAY_OUTARG(ignored)) * CLAY_AT(rec, 13);

    // Negative inside the protected region, so -width is fully protected and
    // +width is fully open.
    float t = cclamp(0.5f - d / (2.0f * width), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

CLAY_FN CTapeValue ctape_replace_feather(CTapeValue a, CTapeValue b, CLAY_FPTR rec,
                                         CLAY_FPTR blob, cfloat3 p) {
    CLAY_FPTR h = blob + CLAY_INT(CLAY_AT(rec, 0));
    cfloat3 origin = cf3(CLAY_AT(h, 0), CLAY_AT(h, 1), CLAY_AT(h, 2));
    float cell = CLAY_AT(h, 3);
    float band = CLAY_AT(h, 4);
    int bcx = CLAY_INT(CLAY_AT(h, 5)), bcy = CLAY_INT(CLAY_AT(h, 6)), bcz = CLAY_INT(CLAY_AT(h, 7));
    float feather = CLAY_AT(h, 12);
    if (bcx <= 0 || bcy <= 0 || bcz <= 0 || cell <= 0.0f || feather <= 0.0f) {
        CTapeValue r;
        r.d = op_replace(a.d, b.d);
        r.color = b.d < 0.0f ? b.color : a.color;
        return r;
    }

    cfloat4x4 inv;
    inv.c0 = cf4(CLAY_AT(rec, 1), CLAY_AT(rec, 2), CLAY_AT(rec, 3), 0.0f);
    inv.c1 = cf4(CLAY_AT(rec, 4), CLAY_AT(rec, 5), CLAY_AT(rec, 6), 0.0f);
    inv.c2 = cf4(CLAY_AT(rec, 7), CLAY_AT(rec, 8), CLAY_AT(rec, 9), 0.0f);
    inv.c3 = cf4(CLAY_AT(rec, 10), CLAY_AT(rec, 11), CLAY_AT(rec, 12), 1.0f);
    cfloat3 lp = cmul_point(inv, p);

    // Chebyshev inset into the sampled box: positive inside, zero on a face.
    float sx = CLAY_FLOATC((bcx * CLAY_BRICK_DIM)) * cell;
    float sy = CLAY_FLOATC((bcy * CLAY_BRICK_DIM)) * cell;
    float sz = CLAY_FLOATC((bcz * CLAY_BRICK_DIM)) * cell;
    float inset = cmin(cmin(lp.x - origin.x, origin.x + sx - lp.x),
                       cmin(cmin(lp.y - origin.y, origin.y + sy - lp.y),
                            cmin(lp.z - origin.z, origin.z + sz - lp.z)));
    float t = cclamp(inset / feather, 0.0f, 1.0f);
    float w = t * t * (3.0f - 2.0f * t);

    // band and the correction are compared in world units: the header carries
    // both in the volume's own, so the instance scale converts them.
    float scale = CLAY_AT(rec, 13);
    float band_world = band * scale;
    CTapeValue r;
    r.d = a.d + w * cclamp(b.d - a.d, -band_world, band_world);
    r.color = b.d < 0.0f ? cmix(a.color, b.color, w) : a.color;
    return r;
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
CLAY_FN float ctape_prim_local(CLAY_UINT_T op, CLAY_FPTR pr,
                               CLAY_FPTR blob, cfloat3 lp, CLAY_INOUT(cfloat3) out_color) {
    CLAY_FPTR deform =
        CLAY_OFF(pr, CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS + CLAY_TAPE_REPEAT_FLOATS);
    int deform_count = CLAY_INT(CLAY_AT(deform, 0));
    float offset = 0.0f;
    cfloat3 wp = lp;
    for (int di = 0; di < deform_count; ++di) {
        CLAY_FPTR rec = CLAY_OFF(deform, 1 + di * CLAY_TAPE_DEFORM_FLOATS);
        offset += ctape_deform_offset(rec, blob, wp);
        wp = ctape_deform_point(rec, blob, wp);
    }
    // Only a sampled volume can write colour, so only it is handed the
    // out-parameter. Every other prim evaluates without one.
    if (op == ctape_volume)
        return ctape_volume_dist(CLAY_OFF(pr, CLAY_TAPE_PRIM_HEADER), blob, wp,
                                 out_color) + offset;
    return ctape_prim_dist(op, CLAY_OFF(pr, CLAY_TAPE_PRIM_HEADER), blob, wp) + offset;
}

// The fixed interpreter every backend ships. Postfix stack machine; empty
// tapes evaluate to "far outside".
CLAY_FN CTapeValue ctape_eval(CLAY_IPTR instrs, int instr_count,
                              CLAY_FPTR params, CLAY_FPTR blob,
                              cfloat3 p) {
    CTapeValue stack[CLAY_TAPE_MAX_STACK];
    int top = 0;
    for (int i = 0; i < instr_count; ++i) {
        CLAY_UINT_T op = CLAY_INSTR_OP(instrs, i);
        CLAY_FPTR pr = CLAY_OFF(params, CLAY_INSTR_PARAM(instrs, i));
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
            int mode = CLAY_INT(CLAY_AT(pr, 0));
            CTapeValue combined;
            if (ctape_mode_is_transition(mode)) {
                // spatial morph: lerp BOTH operands by the weight at p. Not a
                // distance — the tape's tracked field info records that.
                float w = ctape_transition_weight(mode, CLAY_OFF(pr, CLAY_TAPE_COMBINE_HEADER), p);
                combined.d = cmix(a.d, b.d, w);
                combined.color = cmix(a.color, b.color, w);
            } else if (mode == ccombine_replace_feather) {
                combined =
                    ctape_replace_feather(a, b, CLAY_OFF(pr, CLAY_TAPE_COMBINE_HEADER), blob, p);
            } else {
                combined = ctape_combine_values(a, b, mode, CLAY_INT(CLAY_AT(pr, 1)),
                                                CLAY_AT(pr, 2), CLAY_AT(pr, 3));
            }
            // A GATE, if this item carries one: where the mask protects, the
            // result is what the accumulator already held — the item does not
            // participate. Slot 4 is the gate payload's blob offset, and a
            // negative value means no gate, which is the common case and costs
            // one comparison.
            //
            // This composes with EVERY mode rather than being one of them,
            // which is the point: masking has to gate a boolean, and a boolean
            // is a mode.
            int gate_off = CLAY_INT(CLAY_AT(pr, 4));
            if (gate_off >= 0) {
                float g = ctape_gate_weight(blob + gate_off, blob, p);
                // Both ends have to be EXACT, and only one of them is exact
                // through the mix. cmix(x, y, t) is x + (y - x) * t, so t == 0
                // returns x bit for bit — but t == 1 returns x + (y - x), which
                // is y only up to rounding. That last ULP is not cosmetic: it
                // puts a seam of one float step along the whole border of every
                // protected region, and "the protected region is exactly what
                // it was" stops being true.
                //
                // So the fully-protected end is a branch rather than an
                // arithmetic accident.
                if (g >= 1.0f) {
                    combined = a;
                } else {
                    combined.d = cmix(combined.d, a.d, g);
                    combined.color = cmix(combined.color, a.color, g);
                }
            }
            stack[top - 1] = combined;
        } else {
            if (top >= CLAY_TAPE_MAX_STACK) continue;
            cfloat4x4 inv;
            inv.c0 = cf4(CLAY_AT(pr, 0), CLAY_AT(pr, 1), CLAY_AT(pr, 2), 0.0f);
            inv.c1 = cf4(CLAY_AT(pr, 3), CLAY_AT(pr, 4), CLAY_AT(pr, 5), 0.0f);
            inv.c2 = cf4(CLAY_AT(pr, 6), CLAY_AT(pr, 7), CLAY_AT(pr, 8), 0.0f);
            inv.c3 = cf4(CLAY_AT(pr, 9), CLAY_AT(pr, 10), CLAY_AT(pr, 11), 1.0f);
            float scale = CLAY_AT(pr, 12);
            float round = CLAY_AT(pr, 13);
            CTapeValue v;
            v.color = cf3(CLAY_AT(pr, 14), CLAY_AT(pr, 15), CLAY_AT(pr, 16));
            cfloat3 lp = cmul_point(inv, p);
            CLAY_FPTR repeat = CLAY_OFF(pr, CLAY_TAPE_PRIM_HEADER + CLAY_TAPE_PRIM_PARAMS);
            float prim_value;  // NB: not `local` — reserved in OpenCL C
            // A REPEATED volume reports the item's colour rather than its
            // samples'. The repeat paths evaluate the primitive more than once
            // and take a min, so a colour written by whichever call ran last
            // would not be the colour of the instance that won. Discarding it
            // is the honest answer until something repeats a coloured volume —
            // neither producer does: consolidate and the voxel conversion each
            // emit one placed volume.
            cfloat3 repeat_color = v.color;
            if (!ctape_repeat_active(repeat)) {
                prim_value = ctape_prim_local(op, pr, blob, lp, CLAY_OUTARG(v.color));
            } else if (ctape_repeat_is_radial(repeat)) {
                // O(2): the nearest angular sector and its neighbour, per
                // docs/01 2.4 — one evaluation would seam at sector borders
                float d0 = ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, 0),
                                            CLAY_OUTARG(repeat_color));
                int neighbour = crep_radial_neighbor(lp, CLAY_INT(CLAY_AT(repeat, 1)));
                float d1 = ctape_prim_local(op, pr, blob,
                                            ctape_repeat_point(repeat, lp, neighbour),
                                            CLAY_OUTARG(repeat_color));
                prim_value = cmin(d0, d1);
            } else {
                prim_value = ctape_prim_local(op, pr, blob, ctape_repeat_point(repeat, lp, 0),
                                              CLAY_OUTARG(repeat_color));
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
