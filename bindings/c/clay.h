/* clay.h — the flat, stable, versioned C ABI of claycore (c-abi spec).
 *
 * Rules of the boundary:
 *  - opaque handles, integer error codes, caller-owned buffers
 *  - buffers are either caller-allocated via the size-query pattern or
 *    returned behind an owner handle with an explicit clay_*_destroy
 *  - no C++ types, no exceptions, fixed-size integer types, UTF-8 strings
 *  - no variadics, no bitfields (FFI-general: Swift, C#, Rust, ctypes)
 *  - every descriptor struct starts with a uint32_t struct_size the caller
 *    sets and the library reads only up to (see descriptor struct versioning)
 *
 * Error details: every failing call records a thread-local UTF-8 message
 * retrievable with clay_last_error().
 */

#ifndef CLAY_H
#define CLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAY_ABI_MAJOR 0
#define CLAY_ABI_MINOR 22
#define CLAY_ABI_PATCH 1

/* Upper bound on the element count of any batch call: points, rays, cells,
 * selected node ids, stroke points, polygon vertices. A count above it is
 * rejected with CLAY_ERROR_INVALID_ARGUMENT rather than used to size a
 * buffer. A count is the one argument this ABI cannot check against the
 * caller's memory, and several calls allocate from one, so a byte length
 * passed where an element count belongs — or a negative signed value widened
 * to size_t, the usual Swift Int mistake — would otherwise allocate until the
 * process dies. Split larger work into batches. */
#define CLAY_MAX_BATCH 16777216 /* 1 << 24 */

typedef enum clay_result {
    CLAY_OK = 0,
    CLAY_ERROR_INVALID_ARGUMENT = 1,
    CLAY_ERROR_NOT_FOUND = 2,
    CLAY_ERROR_BUFFER_TOO_SMALL = 3,
    CLAY_ERROR_IO = 4,
    CLAY_ERROR_FORWARD_VERSION = 5,
    CLAY_ERROR_BUDGET_EXCEEDED = 6,
    CLAY_ERROR_UNSUPPORTED = 7,
    CLAY_ERROR_BACKEND = 8
} clay_result;

/* Library/ABI version (see c-abi spec). Compare majors at init — and while
 * the major is 0, the minor too: under SemVer's 0.x rule the ABI may still
 * break on a minor bump, 0.2.0 did break (see below), and every minor below
 * 1.0 adds entry points a host cannot dlopen its way out of missing. */
void clay_version(int32_t* major, int32_t* minor, int32_t* patch);

/* Thread-local detail message for the last failing call on this thread. */
const char* clay_last_error(void);

/* -- enums (values are ABI-stable) ---------------------------------------- */

/* Every primitive the scene model supports. The values are the engine's tape
 * opcodes, so there is no translation table to drift; a static assertion per
 * entry pins them (bindings/c/clay_c.cpp). params is the raw parameter block:
 * sizes are half-extents and angles are given resolved to their sine and
 * cosine, a pair the library checks is one. A plane's normal is normalized
 * for you, as it is in the Python bindings; a zero-length one is rejected. */
typedef enum clay_prim {
    CLAY_PRIM_SPHERE = 0,             /* params: r */
    CLAY_PRIM_BOX = 1,                /* params: bx by bz */
    CLAY_PRIM_ROUND_BOX = 2,          /* params: bx by bz r */
    CLAY_PRIM_BOX_FRAME = 3,          /* params: bx by bz e */
    CLAY_PRIM_TORUS = 4,              /* params: R r */
    CLAY_PRIM_CAPSULE = 5,            /* params: ax ay az bx by bz r */
    CLAY_PRIM_CAPPED_CYLINDER = 6,    /* params: r h */
    CLAY_PRIM_ROUNDED_CYLINDER = 7,   /* params: ra rb h */
    CLAY_PRIM_CAPPED_CONE = 8,        /* params: h r1 r2 */
    CLAY_PRIM_ROUND_CONE = 9,         /* params: r1 r2 h */
    CLAY_PRIM_ELLIPSOID = 10,         /* params: rx ry rz (bound) */
    CLAY_PRIM_OCTAHEDRON = 11,        /* params: s */
    CLAY_PRIM_HEX_PRISM = 12,         /* params: hx hy */
    CLAY_PRIM_PYRAMID = 13,           /* params: h */
    /* Out-of-line payload: the point chain / the 2D profile lives on the
     * item, not in params, so these need the item builder below rather than
     * clay_item_desc. */
    CLAY_PRIM_STROKE = 14,            /* points (x y z r) carried by the item */
    CLAY_PRIM_EXTRUDE = 15,           /* profile + half-depth (along Z) */
    CLAY_PRIM_REVOLVE = 16,           /* profile + axis offset (about Y) */
    CLAY_PRIM_CAPPED_TORUS = 17,      /* params: sin(aperture) cos(aperture) ra rb */
    CLAY_PRIM_LINK = 18,              /* params: le r1 r2 */
    CLAY_PRIM_CYLINDER_INFINITE = 19, /* params: cx cz r (unbounded) */
    CLAY_PRIM_CONE = 20,              /* exact cone; params: sin(a) cos(a) h, a = half-angle */
    CLAY_PRIM_PLANE = 21,             /* params: nx ny nz h, n normalized in (unbounded) */
    CLAY_PRIM_CUT_SPHERE = 22,        /* params: r h */
    CLAY_PRIM_CUT_HOLLOW_SPHERE = 23, /* params: r h t */
    CLAY_PRIM_SOLID_ANGLE = 24,       /* params: sin(angle) cos(angle) ra */
    CLAY_PRIM_TETRAHEDRON = 25,       /* params: r */
    CLAY_PRIM_DODECAHEDRON = 26,      /* params: r */
    CLAY_PRIM_ICOSAHEDRON = 27,       /* params: r */
    CLAY_PRIM_TRI_PRISM = 28,         /* params: hx hy (bound) */
    CLAY_PRIM_OCTAHEDRON_CHEAP = 29,  /* params: s (bound) */
    CLAY_PRIM_LNORM_SPHERE = 30,      /* params: r n, n >= 2 (bound) */
    /* Loft: params are the half-depth and the easing curve. The PROFILES are
     * not parameters — a variable number of them cannot fit in a fixed block —
     * so they are added with clay_item_add_loft_profile, two or more of them,
     * and interpolated evenly along Z. (bound) */
    CLAY_PRIM_LOFT = 31,
    /* Swept: the same profiles as a loft, carried along a GUIDE curve rather
     * than the Z axis. params is the easing curve. The guide is the item's
     * curve point list (clay_item_set_curve_points), and the profiles are
     * added with clay_item_add_loft_profile — a guide is not a new kind of
     * curve and a swept profile is not a new kind of profile. (bound) */
    CLAY_PRIM_SWEPT = 32,
    /* A sampled narrow-band volume. Not built with clay_item_create, which
     * has no way to supply the samples: the producers are
     * clay_item_volume_from_mesh and clay_item_volume_from_document. (bound) */
    CLAY_PRIM_VOLUME = 33
} clay_prim;

/* Combine ops. For the extended modes (groove..replace) blend_k is the
 * mode's radius or depth and the blend profile is ignored; groove and tongue
 * additionally read the item's rounding as the channel half-width. The two
 * transitions morph between the accumulated field and the item over a
 * world-space span; their parameters do not fit clay_item_desc, so they are
 * authored with clay_item_set_transition_linear / _radial. */
typedef enum clay_op {
    CLAY_OP_ADD = 0,
    CLAY_OP_SUBTRACT = 1,
    CLAY_OP_INTERSECT = 2,
    CLAY_OP_PAINT = 3, /* color only */
    CLAY_OP_GROOVE = 4,
    CLAY_OP_TONGUE = 5,
    CLAY_OP_PIPE = 6,
    CLAY_OP_ENGRAVE = 7, /* deboss */
    CLAY_OP_EMBOSS = 8,
    CLAY_OP_INSET = 9,
    CLAY_OP_SHELL = 10,
    CLAY_OP_REPLACE = 11,
    CLAY_OP_TRANSITION_LINEAR = 12, /* morph along a segment */
    CLAY_OP_TRANSITION_RADIAL = 13, /* morph over an XZ radius */
    /* Surface relief: the item is a REGION, and blend_k is the amplitude by
     * which the surface accumulated BEFORE it moves along its own normal. The
     * item's rounding is the falloff width, the same convention groove and
     * tongue use. Support is finite, so influence bounds and culling are
     * unaffected.
     *
     * A pair rather than one signed amplitude, because blend_k is required
     * non-negative — and because add/subtract and engrave/emboss are pairs. */
    CLAY_OP_RELIEF = 14, /* build up: ZBrush Standard, ClayBuildup */
    CLAY_OP_INCISE = 15  /* cut in:   Crease, DamStandard */
} clay_op;

typedef enum clay_blend {
    CLAY_BLEND_HARD = 0,
    CLAY_BLEND_QUADRATIC = 1,
    CLAY_BLEND_CUBIC = 2,
    CLAY_BLEND_CIRCULAR = 3,
    CLAY_BLEND_CHAMFER = 4
} clay_blend;

/* Domain warps an item can carry, applied to its local point (item builder).
 * The parameters each kind takes, in order:
 *   TWIST     k             radians per unit of height, about Y
 *   BEND      k             radians per unit along X
 *   TAPER     y0 y1 s0 s1   cross-section scale s0 at y0 to s1 at y1
 *   DISPLACE  amplitude frequency   sine displacement of the field
 *   WRAP      x0 x1         bend the X interval around a cylinder about Z
 *   ELONGATE  hx hy hz      insert flat sections of half-extent h per axis
 *   BEND_LINEAR ax ay az bx by bz vx vy vz   displace by v, eased along a->b
 *   BEND_RADIAL r0 r1 dz  displace along Y by dz, eased across r0->r1
 *   ELONGATE_AXIS hx hy hz  per-axis stretch; a bound for any primitive
 *   GRAB      cx cy cz r dx dy dz front   pull a region; identity past r
 *   POSE      cx cy cz r ax ay az angle   rotate a region about its centre
 *   POSE_LINE ax ay az bx by bz nx ny nz angle  ramp a rotation along a -> b */
typedef enum clay_deform {
    CLAY_DEFORM_TWIST = 0,
    CLAY_DEFORM_BEND = 1,
    CLAY_DEFORM_TAPER = 2,
    CLAY_DEFORM_DISPLACE = 3,
    CLAY_DEFORM_WRAP_AROUND = 4,
    CLAY_DEFORM_ELONGATE = 5,
    CLAY_DEFORM_BEND_LINEAR = 6,
    CLAY_DEFORM_BEND_RADIAL = 7,
    CLAY_DEFORM_ELONGATE_AXIS = 8,
    CLAY_DEFORM_GRAB = 9,
    CLAY_DEFORM_POSE = 10,
    CLAY_DEFORM_POSE_LINE = 11,
    /* Radial scale about a centre: centre(3), radius, strength. ONE signed
     * strength covers both directions — positive magnifies, negative pinches —
     * because they are the same deformation. */
    CLAY_DEFORM_MAGNIFY = 12,
    /* Fractal gradient noise as a distance offset: amplitude, frequency,
     * octaves, gain, seed. The irregular sibling of CLAY_DEFORM_DISPLACE,
     * whose sine is regular by construction. The seed is an ordinary parameter,
     * not global state, so the same seed always gives the same field. */
    CLAY_DEFORM_NOISE = 13
} clay_deform;

/* Easing curves are given by index; 0 is linear. Only the taper deformer and
 * the transition ops read one. */
#define CLAY_EASE_LINEAR 0
#define CLAY_EASE_COUNT 33 /* valid indices are [0, CLAY_EASE_COUNT) */

/* Closed 2D profiles a lift primitive (extrude, revolve) can carry. Open
 * curves are unsigned distances rather than regions, so they are not
 * profiles: flatten them to a polygon host-side. */
typedef enum clay_profile {
    CLAY_PROFILE_CIRCLE = 0,    /* params: r */
    CLAY_PROFILE_BOX = 1,       /* params: hx hy */
    CLAY_PROFILE_HEXAGON = 2,   /* params: r (face radius) */
    CLAY_PROFILE_TRIANGLE = 3,  /* params: r */
    CLAY_PROFILE_TRAPEZOID = 4, /* params: bottom top half_height */
    CLAY_PROFILE_VESICA = 5,    /* params: r d */
    CLAY_PROFILE_POLYGON = 6    /* vertices: clay_item_set_profile_polygon */
} clay_profile;

/* -- descriptor struct versioning ------------------------------------------ */

/* Every descriptor struct crossing this ABI starts with a uint32_t
 * struct_size that the caller sets to the sizeof of the struct it compiled
 * against. The library reads only that prefix, so fields may be appended
 * without a major bump; anything the caller did not declare takes its
 * documented default, which is the zero value for every field so far.
 *
 * Setting it is mandatory. The original layout — the field set a struct had
 * when the convention shipped in ABI 0.2.0 — is the shortest value accepted;
 * anything below it, zero included, is rejected with
 * CLAY_ERROR_INVALID_ARGUMENT. A value larger than this header knows about is
 * clamped, so a newer caller's unknown tail is ignored rather than misread,
 * and a value too large to be any descriptor is rejected outright.
 *
 * clay_mesh_params is the first struct to have used this: mesher and
 * experimental were appended to it in 0.3.0, and a caller compiled against
 * the 0.2.0 layout still meshes, with both fields taking their zero default.
 *
 * ABI 0.2.0 BREAKS BINARY COMPATIBILITY WITH 0.1.0 for the two descriptors
 * that predate the convention, clay_item_desc and clay_mesh_params: the new
 * leading field shifts every other one. This is deliberate and allowed by
 * SemVer's 0.x rule; there is no way to tell a 0.1.0 descriptor from a
 * zeroed 0.2.0 one by content, which is why zero is not a sentinel for the
 * original layout. A 0.1.0 binary passing either struct puts a prim value or
 * a voxel size where struct_size now is, so the call is rejected rather than
 * misread. Recompile against this header, set struct_size, and the fields
 * mean exactly what they meant in 0.1.0. */

/* -- document -------------------------------------------------------------- */

typedef struct clay_document clay_document; /* opaque */
typedef uint32_t clay_layer_id;
typedef uint32_t clay_node_id;

clay_document* clay_document_create(void);
void clay_document_destroy(clay_document* doc);

clay_result clay_document_save(const clay_document* doc, const char* path);
clay_result clay_document_load(const char* path, clay_document** out_doc);

clay_result clay_add_sdf_layer(clay_document* doc, const char* name,
                               clay_layer_id* out_layer);

typedef struct clay_item_desc {
    uint32_t struct_size; /* = sizeof(clay_item_desc); required, see above */
    int32_t prim;        /* clay_prim */
    float params[7];     /* primitive parameters, see clay_prim comments */
    float position[3];
    float rotation[4];   /* quaternion x y z w (identity: 0 0 0 1) */
    float scale;         /* uniform; 0 means 1 */
    int32_t op;          /* clay_op */
    int32_t blend;       /* clay_blend */
    float blend_k;
    float rounding;
    float color[3];
    int32_t mirror;      /* 0/1: apply through the layer's mirror axes */
} clay_item_desc;

/* Adds one edit from the flat descriptor — sugar over the item builder for
 * edits that need no modifier chain and no out-of-line payload. Primitives
 * that carry one (CLAY_PRIM_STROKE and the two lifts) and the transition ops
 * need parameters this descriptor cannot express; they are rejected with
 * CLAY_ERROR_INVALID_ARGUMENT, as is any unknown prim, op or blend value.
 * Use clay_item_create for those. */
clay_result clay_add_item(clay_document* doc, clay_layer_id layer,
                          const clay_item_desc* item, clay_node_id* out_node);
clay_result clay_remove_node(clay_document* doc, clay_layer_id layer, clay_node_id node);

/* -- undo -----------------------------------------------------------------
 * Opt-in per document: a document that never enables it behaves exactly as it
 * did before, and edits made before enabling are not undoable. Once enabled
 * every editing entry point records its own inverse, so no reachable edit
 * escapes undo. Nothing to undo is reported through *out_undone, not returned
 * as a failure, so a UI can drive the buttons without tracking state. */
clay_result clay_document_enable_undo(clay_document* doc);
clay_result clay_document_undo(clay_document* doc, int32_t* out_undone);
clay_result clay_document_redo(clay_document* doc, int32_t* out_redone);
/* One query for everything a UI needs to label its buttons. Any out pointer
 * may be NULL. The depths read 0 when undo is not enabled, which *out_enabled
 * distinguishes from an enabled-but-empty history. */
clay_result clay_document_undo_state(const clay_document* doc, int32_t* out_enabled,
                                     size_t* out_undo_depth, size_t* out_redo_depth);
/* Bracket a burst of edits so they undo as one step. */
clay_result clay_document_begin_undo_group(clay_document* doc);
clay_result clay_document_end_undo_group(clay_document* doc);

/* -- editing a placed node -------------------------------------------------
 * Everything below addresses an existing node by the id clay_layer_add_item
 * returned, and applies one command from the engine's vocabulary — the same
 * one the document format records — so a binding edit means exactly what a
 * saved document means. An id the document does not hold gives
 * CLAY_ERROR_NOT_FOUND and leaves the document byte-identical.
 *
 * Unlike the Python bindings, which take partial updates, these take the
 * whole value: C has no idiomatic "leave this one alone" argument. Read the
 * current state, change what you want, pass all of it back. */

/* Retransform a node. axis/angle give the rotation; scale must be > 0. */
clay_result clay_layer_set_transform(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float position[3], const float rotation_axis[3],
                                     float rotation_angle, float scale);
/* Replace a node's primitive. Its deformers, repetition, profile and stroke
 * belong to the node, not to the primitive, and survive the edit. */
clay_result clay_layer_set_prim(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                int32_t prim, const float* params, size_t param_count);
clay_result clay_layer_set_color(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                 const float rgb[3]);
/* op is clay_op, blend is clay_blend; rounding must be >= 0. */
clay_result clay_layer_set_op_blend(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t op, int32_t blend, float blend_k, float rounding);
/* Reparent or reorder. new_parent 0 moves the node to the layer root, and
 * index < 0 appends. */
clay_result clay_layer_move(clay_document* doc, clay_layer_id layer, clay_node_id node,
                            clay_node_id new_parent, int32_t index);
/* Append (x, y, z, radius) quadruples to a placed stroke: points_xyzr holds
 * count * 4 floats. */
clay_result clay_layer_append_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float* points_xyzr, size_t count);
/* Remove the last count points from a placed stroke. */
clay_result clay_layer_trim_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   uint32_t count);

/* -- editing layers -------------------------------------------------------- */

clay_result clay_document_remove_layer(clay_document* doc, clay_layer_id layer);
/* Reorder a layer. The command vocabulary expresses this as remove-then-add,
 * so it is the one edit that is a pair rather than a single command. */
clay_result clay_document_move_layer(clay_document* doc, clay_layer_id layer, int32_t index);
/* A hidden layer contributes nothing to the field; showing it again restores
 * the original field exactly. */
clay_result clay_document_set_layer_visible(clay_document* doc, clay_layer_id layer,
                                            int32_t visible);
/* Protection, both off by default. A GHOSTED layer is still evaluated but is
 * never picked and never edited: "show me this for reference, but stay out of
 * my way". A LOCKED layer is still picked but never edited: "this is
 * finished". Neither changes what the document evaluates to — how a host
 * DRAWS a ghost is its own business.
 *
 * Every edit naming a protected layer returns CLAY_ERROR_INVALID_ARGUMENT and
 * leaves the document unchanged. It is refused rather than silently dropped: a
 * host that greys the layer out wants to know, and one that does not must not
 * quietly discard the artist's work. This call itself is always allowed —
 * locking would otherwise be irreversible. */
clay_result clay_document_set_layer_protection(clay_document* doc, clay_layer_id layer,
                                               int32_t ghost, int32_t locked);
clay_result clay_document_layer_protection(const clay_document* doc, clay_layer_id layer,
                                           int32_t* out_ghost, int32_t* out_locked);
clay_result clay_document_set_layer_transform(clay_document* doc, clay_layer_id layer,
                                              const float position[3],
                                              const float rotation_axis[3],
                                              float rotation_angle, float scale);
clay_result clay_set_layer_mirror(clay_document* doc, clay_layer_id layer, int32_t axis_x,
                                  int32_t axis_y, int32_t axis_z, float mirror_k);

/* -- item builder ---------------------------------------------------------- */

/* An edit composed one call at a time, then appended to a layer. This is the
 * path to everything the flat descriptor cannot express: chained modifiers —
 * call order is application order, because deformers do not commute — and
 * variable-length payloads, which are COPIED into the builder, so the
 * caller's arrays need not outlive the call.
 *
 * A builder is independent of any document and may be added to a layer any
 * number of times. Every call below returns clay_result and records
 * clay_last_error() on failure, leaving the builder as it was. */

typedef struct clay_item clay_item; /* opaque */

/* Creates a builder for one primitive. params/param_count are the parameters
 * documented on that clay_prim value, and a wrong count is rejected;
 * CLAY_PRIM_STROKE takes none (its points are set separately) and a lift
 * takes one (half-depth or axis offset). Returns NULL on invalid input, with
 * the detail in clay_last_error(). Free with clay_item_destroy. */
clay_item* clay_item_create(int32_t prim, const float* params, size_t param_count);
void clay_item_destroy(clay_item* item);

clay_result clay_item_set_position(clay_item* item, const float position[3]);
/* Rotation as an axis (need not be normalized) and an angle in radians. */
clay_result clay_item_set_rotation(clay_item* item, const float axis[3], float radians);
clay_result clay_item_set_scale(clay_item* item, float scale);           /* uniform, > 0 */
clay_result clay_item_set_op(clay_item* item, int32_t op);               /* clay_op */
clay_result clay_item_set_blend(clay_item* item, int32_t blend, float k); /* clay_blend, k >= 0 */
clay_result clay_item_set_rounding(clay_item* item, float rounding);     /* >= 0 */
clay_result clay_item_set_color(clay_item* item, const float rgb[3]);
clay_result clay_item_set_mirror(clay_item* item, int32_t mirror); /* 0/1: layer mirror axes */

/* Appends one domain warp (clay_deform) to the item's chain: the local point
 * is warped by the first one added first. params/param_count are the kind's
 * own, listed on clay_deform; ease indexes an easing curve and is read by the
 * taper only. */
clay_result clay_item_add_deformer(clay_item* item, int32_t deform, const float* params,
                                   size_t param_count, int32_t ease);

/* Repetition of the item, applied before the deformer chain (so an array
 * repeats the deformed shape). One per item: the last call wins.
 * counts == NULL gives an infinite grid, whose influence is unbounded and so
 * never culled; otherwise counts is the max cell index per axis and one
 * spacing serves all three. */
clay_result clay_item_set_repeat_grid(clay_item* item, const float spacing[3],
                                      const float counts[3]);
/* Circular array of count (>= 2) copies about Y, offset from the axis. */
clay_result clay_item_set_repeat_radial(clay_item* item, int32_t count, float offset);

/* The 2D profile of a lift primitive, with the parameters listed on
 * clay_profile; a lift with no profile set uses a unit circle. Polygons go
 * through the call below, which takes count (>= 3) x, y pairs in local space:
 * the outline is implicitly closed and filled by the even-odd rule, so
 * concave and self-intersecting outlines are allowed. */
clay_result clay_item_set_profile(clay_item* item, int32_t profile, const float* params,
                                  size_t param_count);
clay_result clay_item_set_profile_polygon(clay_item* item, const float* xy, size_t count);

/* Append a profile to a CLAY_PRIM_LOFT item. Two or more are required; they
 * are interpolated evenly along Z in the order added. `params` is the
 * profile's own parameter block as clay_item_set_profile takes it, and
 * `polygon_xy` (count*2 floats, or NULL) supplies the vertices when the
 * profile is CLAY_PROFILE_POLYGON.
 *
 * The profiles are not item parameters: a variable number of them cannot fit
 * in a fixed block, and a polygon's vertices are already out of line. */
clay_result clay_item_add_loft_profile(clay_item* item, int32_t profile, const float* params,
                                       size_t param_count, const float* polygon_xy,
                                       size_t polygon_count);

/* Point chain of a CLAY_PRIM_STROKE item: count points of x, y, z, radius,
 * replacing any previous chain. One point is a sphere and none contributes
 * nothing, as in the Python bindings. blend_k (>= 0) smooths consecutive
 * segments of the chain itself. */
clay_result clay_item_set_stroke_points(clay_item* item, const float* xyzr, size_t count);
clay_result clay_item_add_stroke_point(clay_item* item, const float position[3], float radius);
clay_result clay_item_set_stroke_blend_k(clay_item* item, float k);

/* How a point joins the one after it. A stroke is a curve whose points are all
 * hard corners, so CLAY_POINT_HARD is both the default and exactly what a
 * chain authored before types existed means. */
typedef enum clay_point_type {
    CLAY_POINT_HARD = 0,   /* straight segment to the next point */
    CLAY_POINT_SPLINE = 1, /* Catmull-Rom, passing through the points */
    CLAY_POINT_BSPLINE = 2,/* uniform cubic B-spline; approximating, so it rounds corners */
    CLAY_POINT_BEZIER = 3  /* cubic shaped by the handles below */
} clay_point_type;

/* The typed form of the call above: `count` points of x, y, z, radius, plus
 * optional parallel arrays of count clay_point_type values and of count*3
 * floats for the incoming and outgoing Bezier handles. Passing NULL for an
 * optional array leaves that field at its default, so
 * clay_item_set_curve_points(item, xyzr, count, NULL, NULL, NULL) is exactly
 * clay_item_set_stroke_points.
 *
 * Handles are in the item's LOCAL space, relative to their point. 3DCoat keeps
 * its handles in screen space and its own users call that a wart: the curve
 * then means something different depending on where the camera was.
 *
 * Typed points are tessellated into the same segment chain at compile time, so
 * a curve costs nothing at evaluation time and no backend knows it exists. */
clay_result clay_item_set_curve_points(clay_item* item, const float* xyzr, size_t count,
                                       const int32_t* types, const float* in_handles_xyz,
                                       const float* out_handles_xyz);

/* Close the chain, and set the tessellation tolerance: the largest distance a
 * span's midpoint may sit from its chord, which must be > 0. Tolerance is a
 * property of the DOCUMENT, not of the viewer — two builds have to agree on
 * what a document means, so it is not a rendering setting.
 *
 * Also applies to a CLAY_PRIM_SWEPT item's guide, which is an ordinary curve.
 * A swept guide cannot be CLOSED, and asking for one is refused rather than
 * ignored: transporting a frame around a loop does not generally return it to
 * its starting orientation, and the leftover twist is real geometry. */
clay_result clay_item_set_curve(clay_item* item, int32_t closed, float tolerance);

/* Replace a placed stroke or curve's whole point list, through the command
 * vocabulary, so the edit is undoable and refused on a protected layer. A
 * curve is tens of points: a whole-list replace costs less than the
 * bookkeeping granular commands would need, and its inverse is exact. */
clay_result clay_layer_set_stroke_points(clay_document* doc, clay_layer_id layer,
                                         clay_node_id node, const float* xyzr, size_t count,
                                         const int32_t* types, const float* in_handles_xyz,
                                         const float* out_handles_xyz, int32_t closed,
                                         float tolerance);

/* Parameters of a spatial morph, in world space. Required by the matching
 * transition op and rejected with any other op: linear morphs along the
 * segment a -> b, radial over the XZ radius from r0 to r1. */
clay_result clay_item_set_transition_linear(clay_item* item, const float a[3],
                                            const float b[3], int32_t ease);
clay_result clay_item_set_transition_radial(clay_item* item, float r0, float r1, int32_t ease);

/* Appends the composed edit to a layer. The builder is left untouched. */
clay_result clay_layer_add_item(clay_document* doc, clay_layer_id layer, const clay_item* item,
                                clay_node_id* out_node);

/* -- evaluation ------------------------------------------------------------ */

/* Comma-separated registered backend names via the size-query pattern:
 * call with buffer == NULL to receive the required size (incl. NUL) in
 * *size; call again with an adequate buffer to fill it. */
clay_result clay_list_backends(char* buffer, size_t* size);

/* Batch field evaluation. points_xyz is count*3 floats; out_distances is
 * count floats; out_colors_rgb (count*3) may be NULL. backend NULL = "cpu". */
clay_result clay_eval_points(const clay_document* doc, const char* backend,
                             const float* points_xyz, size_t count, float* out_distances,
                             float* out_colors_rgb);

/* Normalized field gradients (the tetrahedron trick, so they are unit-length
 * surface normals rather than raw differences) at the same points
 * clay_eval_points takes; out_gradients_xyz is count*3 floats. A call of its
 * own because a parameter cannot be appended to one that already shipped. */
clay_result clay_eval_gradients(const clay_document* doc, const char* backend,
                                const float* points_xyz, size_t count,
                                float* out_gradients_xyz);

/* The same two queries against one layer's own field: only that layer is
 * compiled, so an edit in a layer above cannot change the answer and a layer
 * can be probed while the stack around it is being authored. A layer that
 * shows nothing — hidden, or a voxel layer, which carries no SDF content —
 * evaluates as empty space rather than failing. */
clay_result clay_layer_eval_points(const clay_document* doc, clay_layer_id layer,
                                   const char* backend, const float* points_xyz, size_t count,
                                   float* out_distances, float* out_colors_rgb);
clay_result clay_layer_eval_gradients(const clay_document* doc, clay_layer_id layer,
                                      const char* backend, const float* points_xyz, size_t count,
                                      float* out_gradients_xyz);

/* Multiply a field distance by this before stepping along a ray: the tape's
 * Lipschitz safety factor, which a scaled or displaced edit lowers. */
clay_result clay_safe_step_scale(const clay_document* doc, float* out_scale);
clay_result clay_layer_safe_step_scale(const clay_document* doc, clay_layer_id layer,
                                       float* out_scale);

/* Single raycast (origin + normalized direction). *out_hit is 0/1. */
clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]);

/* Batch raycast. rays_origin_dir is count*6 floats, an origin xyz then a
 * direction xyz per ray; unlike clay_raycast the directions are normalized
 * here, so a zero-length one is rejected rather than traced. Each out buffer
 * may be NULL and holds count values, or count*3 for position and normal. */
clay_result clay_raycast_many(const clay_document* doc, const float* rays_origin_dir,
                              size_t count, int32_t* out_hits, float* out_t,
                              float* out_positions_xyz, float* out_normals_xyz);

/* -- picking --------------------------------------------------------------- */

/* Raycast that also reports WHAT was hit: the layer and the item whose field
 * is closest at the hit point, so a subtract item is attributed the surface it
 * carved. *out_layer and *out_node are 0 when nothing attributes, and 0 is
 * never a valid layer or node id. The direction is normalized here.
 *
 * This is not the cheap path — it compiles the document, then one tape per
 * layer and one per candidate item. Use clay_raycast when only the position
 * and normal are wanted. */
clay_result clay_raycast_attributed(const clay_document* doc, const float origin[3],
                                    const float dir[3], int32_t* out_hit, float* out_t,
                                    float out_position[3], float out_normal[3],
                                    clay_layer_id* out_layer, clay_node_id* out_node);

/* Snap count points onto the nearest surface by gradient descent.
 * out_positions_xyz and out_normals_xyz are count*3 floats, out_ok is count
 * flags, and each may be NULL. A point that did not converge still reports the
 * best position and outward normal found, with its flag 0. */
clay_result clay_snap_to_surface(const clay_document* doc, const float* points_xyz,
                                 size_t count, float* out_positions_xyz,
                                 float* out_normals_xyz, int32_t* out_ok);

/* Tight world-space bounds of a layer's shapes, with no blend or rounding
 * dilation — the box to frame a camera on. *out_has_bounds is 0 when the layer
 * shows nothing, and out_min/out_max are then left alone. */
clay_result clay_layer_bounds(const clay_document* doc, clay_layer_id layer, float out_min[3],
                              float out_max[3], int32_t* out_has_bounds);
/* The same box for a subset of the layer's nodes — zoom to selection. nodes is
 * count node ids; an id the layer does not hold contributes nothing, so a
 * selection of only such ids reports no bounds rather than an error. */
clay_result clay_layer_selection_bounds(const clay_document* doc, clay_layer_id layer,
                                        const clay_node_id* nodes, size_t count,
                                        float out_min[3], float out_max[3],
                                        int32_t* out_has_bounds);

/* -- meshing (owner-handle pattern) ---------------------------------------- */

typedef struct clay_mesh clay_mesh; /* opaque */

/* Which mesher runs. No engine enumeration stands behind these — the meshers
 * are three separate entry points — so the value is checked against this list
 * and an unknown one is rejected rather than mapped onto the default. Marching
 * is 0 because that is what a caller whose struct_size predates the field
 * gets, and it is what every caller got before the field existed. */
typedef enum clay_mesher {
    CLAY_MESHER_MARCHING = 0,       /* watertight and 2-manifold by construction */
    CLAY_MESHER_NETS = 1,           /* surface nets: preview speed, NOT manifold */
    CLAY_MESHER_DUAL_CONTOURING = 2 /* sharp features; experimental, see below */
} clay_mesher;

typedef struct clay_mesh_params {
    uint32_t struct_size;   /* = sizeof(clay_mesh_params); required, see above */
    float voxel_size;       /* world units per cell; <= 0 picks from resolution */
    int32_t resolution;     /* used when voxel_size <= 0: cells across the largest extent */
    int32_t decimate;       /* 0/1 */
    float decimate_ratio;   /* target triangle ratio when decimate != 0 */
    /* appended in ABI 0.3.0, after the original layout; both default to 0 */
    int32_t mesher;         /* clay_mesher */
    int32_t experimental;   /* 0/1: the opt-in CLAY_MESHER_DUAL_CONTOURING needs */
} clay_mesh_params;

/* Meshes the document's SDF content. CLAY_MESHER_DUAL_CONTOURING is not
 * hardened — plain dual contouring is not guaranteed manifold — so it is
 * reachable only with experimental set; without it the call returns
 * CLAY_ERROR_INVALID_ARGUMENT rather than meshing, which is the refusal the
 * Python bindings make in the same place. */
clay_result clay_document_mesh(const clay_document* doc, const clay_mesh_params* params,
                               clay_mesh** out_mesh);
void clay_mesh_destroy(clay_mesh* mesh);

size_t clay_mesh_vertex_count(const clay_mesh* mesh);
size_t clay_mesh_index_count(const clay_mesh* mesh);
/* Borrowed pointers, valid until clay_mesh_destroy. normals/colors may be
 * NULL when absent. */
const float* clay_mesh_positions(const clay_mesh* mesh);
const float* clay_mesh_normals(const clay_mesh* mesh);
const float* clay_mesh_colors(const clay_mesh* mesh);
const uint32_t* clay_mesh_indices(const clay_mesh* mesh);

/* watertight/manifold checks (mesh validation module). */
clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                               int32_t* out_manifold);

/* Save by extension: .obj, .ply, .fbx, .glb */
clay_result clay_mesh_save(const clay_mesh* mesh, const char* path);

/* Guardrails for an importer, checked against the file's DECLARED counts before
 * anything is allocated — which is the point: a malformed or hostile file can
 * claim a billion triangles, and the check has to happen before the allocation
 * rather than after it. Zero on a field means the library's default. */
typedef struct clay_import_budget {
    uint32_t struct_size; /* = sizeof(clay_import_budget); required */
    uint64_t max_vertices;
    uint64_t max_triangles;
} clay_import_budget;

/* Load by extension: .obj, .ply, .fbx, matched case-insensitively. The
 * counterpart to clay_mesh_save, and what gives clay_item_volume_from_mesh
 * something to sample.
 *
 * `budget` may be NULL for the library's defaults. Exceeding it returns
 * CLAY_ERROR_BUDGET_EXCEEDED rather than allocating. */
clay_result clay_mesh_load(const char* path, const clay_import_budget* budget,
                           clay_mesh** out_mesh);

/* Build a mesh from caller-owned triangles: positions is count*3 floats and
 * indices is triangle_count*3 vertex indices. Copied, so the caller's buffers
 * may be freed on return. */
clay_result clay_mesh_from_triangles(const float* positions, size_t vertex_count,
                                     const uint32_t* indices, size_t index_count,
                                     clay_mesh** out_mesh);

/* -- importing a mesh as a field ------------------------------------------- */

typedef struct clay_volume_params {
    uint32_t struct_size; /* = sizeof(clay_volume_params); required */
    float cell_size;      /* sample spacing; <= 0 picks from the mesh's own size */
    float band;           /* half-width of the band kept; <= 0 means three cells */
    float padding;        /* how far past the mesh's bounds to sample; <= 0 means the band */
    /* How far a BVH node must be before it is summarized by a single term
     * rather than descended. Larger is more accurate and slower; <= 0 means
     * the default. Sampling every triangle exactly is not reachable from here:
     * it is a testing control, not something an app should ask for. */
    float beta;
} clay_volume_params;

/* Samples `mesh` into an item carrying a volume, which is what turns an
 * imported model into something that can be combined, cut and sculpted.
 *
 * The sign comes from the generalized winding number rather than a ray cast or
 * the nearest triangle's normal, because real assets are not watertight: one
 * hole flips a parity test for a whole half-space, while a winding number
 * degrades continuously across an opening.
 *
 * The returned item is owned by the caller until it is added to a layer, the
 * same as clay_item_create. A mesh with no triangles is refused. */
clay_result clay_item_volume_from_mesh(const clay_mesh* mesh, const clay_volume_params* params,
                                       clay_item** out_item);

/* Samples a DOCUMENT's own field into an item carrying a volume — baking what
 * an edit list evaluates to, rather than importing something from outside.
 *
 * This is what makes the volume operations reachable from an app at all. Relax
 * and flatten act on a volume, and until now the only way to get one through
 * this ABI was from a mesh, so an app could smooth an imported scan but not its
 * own sculpt. It is also the consolidation step for a long session: a document
 * with thousands of stroke items bakes to one volume, dropping the history and
 * the per-item tape cost while keeping the shape.
 *
 * The cost is the one baking always has: the items are gone, and with them the
 * ability to go back and change a radius. `cell_size` is the resolution the
 * shape now has, so it is the caller's decision to make deliberately.
 *
 * `region` is an optional (min[3], max[3]) pair, or NULL for the document's own
 * bounds padded by the band. Sampling an empty document is refused. */
clay_result clay_item_volume_from_document(const clay_document* doc,
                                           const clay_volume_params* params,
                                           const float region_min[3], const float region_max[3],
                                           clay_item** out_item);

typedef struct clay_relax_params {
    uint32_t struct_size;  /* = sizeof(clay_relax_params); required */
    float strength;        /* how much of the smoothed value to take, 0..1, per pass */
    int32_t radius_cells;  /* averaging radius in cells; <= 0 means 1 */
    int32_t iterations;    /* passes; <= 0 means 1 */
    float centre[3];       /* where it acts */
    float region_radius;   /* 0 relaxes everywhere, which is a filter not a brush */
    float falloff;         /* taper at the region's edge; widened if too narrow to hide the seam */
} clay_relax_params;

/* Smooths an item that carries a volume, in place. The last of the core
 * sculpting brushes: voxel layers had smoothing, SDF layers had none.
 *
 * Smoothing destroys EXACTNESS — the field no longer reports the true distance
 * to its own surface — but it cannot break the Lipschitz bound, because an
 * average cannot vary faster than the thing it averages, and a field whose
 * slope is bounded by one is automatically a conservative bound on the distance
 * to its own zero set. So the raymarcher stays correct.
 *
 * The item must carry a volume; anything else is refused rather than ignored.
 * From this ABI that means an imported mesh, which is the workflow an app
 * wants: bring in a scan, then smooth it. */
clay_result clay_item_volume_relax(clay_item* item, const clay_relax_params* params);

/* Which side of its plane a flatten acts on.
 *
 * CLAY_FLATTEN_TWO_SIDED is ZBrush's Flatten: material above the plane goes AND
 * hollows below it fill. CLAY_FLATTEN_CUT_ONLY is the hard-surface family —
 * hPolish, Planar, the Trim brushes — where cutting WITHOUT filling is the whole
 * brush: it leaves a crisp facet against untouched surface, and filling the
 * hollows beside a facet is what a polish must not do. CLAY_FLATTEN_FILL_ONLY is
 * the dual, and fills a scanned hole flat without touching the surface around
 * it. The three differ by one clamp, which is why this is a mode rather than
 * three entry points. */
typedef enum clay_flatten_mode {
    CLAY_FLATTEN_TWO_SIDED = 0,
    CLAY_FLATTEN_CUT_ONLY = 1,
    CLAY_FLATTEN_FILL_ONLY = 2
} clay_flatten_mode;

typedef struct clay_flatten_params {
    uint32_t struct_size;  /* = sizeof(clay_flatten_params); required */
    float plane_point[3];  /* a point on the plane to flatten onto */
    float plane_normal[3]; /* unit; material on this side goes. Zero length is refused */
    float strength;        /* 1 puts the surface on the plane, 0 changes nothing */
    float centre[3];       /* where it acts */
    float region_radius;   /* REQUIRED > 0: flatten is local, and with no region it
                            * replaces the shape with a half-space rather than
                            * flattening it — a ball comes back as a box */
    float falloff;         /* taper at the region's edge; widened when too narrow to declare */
    /* Which side of the plane to act on; APPENDED, so a descriptor sized to the
     * layout that predates it still describes a two-sided flatten. */
    int32_t mode;          /* clay_flatten_mode */
} clay_flatten_params;

/* Pulls an item's volume onto a plane, in place. The verb SDF layers were
 * missing: voxel grids have had clay_voxel_sculpt_flatten all along.
 *
 * TWO-SIDED, matching the voxel verb: material on the normal's side goes AND
 * hollows on the other side fill. It is not a subtract, and it is not ZBrush's
 * Clip — as a solid, Clip is exactly Trim, which clay_cut already does.
 *
 * The item's volume is RE-SAMPLED with the flatten applied, so the new band
 * brackets the flattened surface rather than the original one. Accurate while
 * the surface stays near the band it came from: past that a volume reports a
 * bound rather than a distance, and the facet is placed against the bound.
 *
 * A region blends under a weight that varies across it, so the result may be
 * steeper than a plain volume; it declares that, and the document's safe step
 * scale drops to match. The item must carry a volume; anything else is refused
 * rather than ignored. */
clay_result clay_item_volume_flatten(clay_item* item, const clay_flatten_params* params);

/* -- voxel grids ----------------------------------------------------------- */

/* Palette-indexed colored voxels on an integer lattice: cell (x, y, z) covers
 * world space [x, x+1) * voxel_size per axis, and palette index 0 means empty
 * everywhere — setting it erases, painting skips it, meshing emits no face,
 * and it is never returned by clay_voxel_palette_add. Cells cross this ABI as
 * int32_t[3] and batches as a packed int32_t array of count*3, so a caller's
 * (N, 3) int32 buffer needs no repacking.
 *
 * Two lifetimes, and the handle records which one it is:
 *  - clay_voxel_grid_create returns a grid the CALLER owns and destroys.
 *  - clay_document_add_voxel_layer and clay_document_voxel_layer return a
 *    grid BORROWED from a document layer: the document owns both the grid and
 *    the handle, edits through it are what clay_document_save writes, and
 *    clay_voxel_grid_destroy on it is rejected rather than obeyed.
 * A borrowed handle names its layer rather than pointing at the grid, and
 * looks it up again on every call, so it never caches a pointer that a later
 * edit to the document could move. Nothing in this ABI removes a layer today,
 * so that lookup does not fail; if a removal call is ever added, calls through
 * a handle to the removed layer become CLAY_ERROR_NOT_FOUND rather than a use
 * after free. Destroying the document does invalidate the handle and nothing
 * can detect that, so a borrowed handle must not outlive its document. */

typedef struct clay_voxel_grid clay_voxel_grid; /* opaque */

/* Mirror axes of a mirrored edit, OR'd together; 0 is the plain edit. The
 * mirror plane passes through lattice coordinate 0, so cell x reflects to
 * -1-x, and the cell given is always written alongside its reflections. */
typedef enum clay_mirror {
    CLAY_MIRROR_X = 1,
    CLAY_MIRROR_Y = 2,
    CLAY_MIRROR_Z = 4
} clay_mirror;

/* Brush footprint. A sphere is the ball of the same diameter, so it is always
 * a subset of the cube of the same size. */
typedef enum clay_brush_shape {
    CLAY_BRUSH_SHAPE_CUBE = 0,
    CLAY_BRUSH_SHAPE_SPHERE = 1
} clay_brush_shape;

/* Falloff curve over the normalized distance from the footprint centre.
 * Occupancy is binary, so a weight between 0 and 1 cannot be stored in a
 * cell: it is resolved by dithering against a hash of the cell coordinate and
 * the seed, which makes a soft stamp reproducible — same stamp, same seed,
 * same cells, on every platform and through every binding. */
typedef enum clay_brush_falloff {
    CLAY_BRUSH_FALLOFF_CONSTANT = 0, /* hard-edged, the usual brush */
    CLAY_BRUSH_FALLOFF_LINEAR = 1,
    CLAY_BRUSH_FALLOFF_SMOOTH = 2, /* smoothstep */
    CLAY_BRUSH_FALLOFF_GAUSSIAN = 3
} clay_brush_falloff;

/* One brush stamp. Every field belongs to the original layout, so none of
 * them can be left out by a shorter struct_size, and none of them has a
 * default: a struct_size shorter than this is rejected outright. Keep it that
 * way — a field appended later takes the zero value when a caller does not
 * declare it, so only fields whose default is zero may be appended.
 *
 * One field is a pointer, which sharpens the struct_size contract: a caller
 * that declares a struct_size LARGER than the layout it actually compiled
 * against has the boundary read a mask handle out of bytes it never wrote,
 * and a handle cannot be validated the way a number can be range-checked.
 * Declaring sizeof of the struct you compiled against is not a formality.
 *
 * strength reaches the engine untouched, so a stamp lands on exactly the
 * cells the same stamp lands on through the Python bindings. It must be > 0:
 * a strength that is not covers no cell at all, which is a zero-initialized
 * descriptor rather than a request, and unlike the Python bindings this
 * boundary rejects it instead of stamping nothing. A strength at or above 1
 * is the full footprint. */
typedef struct clay_mask clay_mask; /* opaque; see -- masks -- below */

typedef struct clay_brush_params {
    uint32_t struct_size; /* = sizeof(clay_brush_params); required, see above */
    int32_t size;         /* cells the footprint spans per axis; must be > 0 */
    int32_t shape;        /* clay_brush_shape */
    int32_t falloff;      /* clay_brush_falloff */
    float strength;       /* coverage multiplier; must be > 0, >= 1 is full */
    uint32_t seed;        /* dither seed */
    /* Appended at ABI 0.12.0. NULL — the zero value, and what a caller
     * compiled against 0.11.0 effectively declares — means no gating, so an
     * older caller's stamp lands exactly where it always did. The mask is
     * borrowed for the duration of the call and must outlive it. */
    const clay_mask* mask;
} clay_brush_params;

/* A standalone grid, owned by the caller. voxel_size is world units per cell
 * and must be > 0. Returns NULL on invalid input, with the detail in
 * clay_last_error(). Free with clay_voxel_grid_destroy. */
clay_voxel_grid* clay_voxel_grid_create(float voxel_size);
/* Frees a grid created by clay_voxel_grid_create. A handle borrowed from a
 * document layer is owned by that document: this returns
 * CLAY_ERROR_INVALID_ARGUMENT and leaves the document untouched. */
clay_result clay_voxel_grid_destroy(clay_voxel_grid* grid);

/* Adds a voxel layer and borrows its grid. A voxel layer carries no SDF
 * content, so clay_add_item and clay_layer_add_item do not apply to it. */
clay_result clay_document_add_voxel_layer(clay_document* doc, const char* name,
                                          float voxel_size, clay_layer_id* out_layer,
                                          clay_voxel_grid** out_grid);
/* Borrows the grid of an existing voxel layer by name; CLAY_ERROR_NOT_FOUND
 * when the document has no such layer. */
clay_result clay_document_voxel_layer(clay_document* doc, const char* name,
                                      clay_layer_id* out_layer, clay_voxel_grid** out_grid);

clay_result clay_voxel_size(const clay_voxel_grid* grid, float* out_voxel_size);

/* -- palette --------------------------------------------------------------- */

/* Palette indices are [0, 255] and anything else is rejected. Index 0 is the
 * empty slot: it is never added, never matched, and cannot be recolored.
 * The palette saturates at 256 entries, returning 255 rather than growing. */
clay_result clay_voxel_palette_add(clay_voxel_grid* grid, const float rgb[3],
                                   int32_t* out_index);
clay_result clay_voxel_palette_color(const clay_voxel_grid* grid, int32_t index,
                                     float out_rgb[3]);
/* Recolors an entry; every voxel referencing it follows, with no voxel data
 * touched. A no-op for index 0 and for an index the palette does not hold. */
clay_result clay_voxel_palette_set(clay_voxel_grid* grid, int32_t index, const float rgb[3]);
/* Counts the unused index-0 slot, so a fresh grid reports 1. */
clay_result clay_voxel_palette_size(const clay_voxel_grid* grid, size_t* out_size);

/* -- edits ----------------------------------------------------------------- */

clay_result clay_voxel_get(const clay_voxel_grid* grid, const int32_t cell[3],
                           int32_t* out_index);
clay_result clay_voxel_set(clay_voxel_grid* grid, const int32_t cell[3], int32_t index);
clay_result clay_voxel_erase(clay_voxel_grid* grid, const int32_t cell[3]);
/* Recolors an occupied cell; empty cells and index 0 are no-ops. */
clay_result clay_voxel_paint(clay_voxel_grid* grid, const int32_t cell[3], int32_t index);

/* Batch forms: cells_xyz is count*3 int32 values. */
clay_result clay_voxel_set_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count,
                                int32_t index);
clay_result clay_voxel_erase_many(clay_voxel_grid* grid, const int32_t* cells_xyz, size_t count);

clay_result clay_voxel_fill_box(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                int32_t index); /* inclusive corners */
clay_result clay_voxel_fill_line(clay_voxel_grid* grid, const int32_t a[3], const int32_t b[3],
                                 int32_t index); /* 3D DDA */

/* The edit and every mirror combination of `axes` (clay_mirror bits) in one
 * call; axes == 0 is the plain edit. Bits outside the three axes are
 * rejected. */
clay_result clay_voxel_set_mirrored(clay_voxel_grid* grid, const int32_t cell[3], int32_t index,
                                    int32_t axes);
clay_result clay_voxel_paint_mirrored(clay_voxel_grid* grid, const int32_t cell[3],
                                      int32_t index, int32_t axes);

/* -- brushes --------------------------------------------------------------- */

/* Stamp the footprint centered on a cell. Size n spans exactly n cells per
 * axis for every n: the footprint runs -((n-1)/2) ..= n/2, symmetric for odd
 * n and biased half a cell toward +XYZ for even n. */
clay_result clay_voxel_set_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                 const clay_brush_params* brush, int32_t index);
clay_result clay_voxel_erase_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush);
clay_result clay_voxel_paint_brush(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush, int32_t index);

/* -- sculpting verbs ------------------------------------------------------- */

/* These reshape existing material instead of stamping a footprint. Each reads
 * a snapshot of the region first, so a cell's outcome never depends on a
 * neighbour the same call already changed. */

/* Majority filter over the 26-neighbourhood: spurs dissolve, notches fill. */
clay_result clay_voxel_sculpt_smooth(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush);
/* amount > 0 dilates, < 0 erodes, |amount| one-cell passes; 0 is a no-op. */
clay_result clay_voxel_sculpt_inflate(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, int32_t amount);
/* Pull the surface onto the plane through the brush centre: material on the
 * +normal side goes, hollows on the other side that touch material are
 * filled. offset_cells slides the plane along +normal, in CELLS. Unlike the
 * Python bindings, a zero-length normal is rejected rather than evaluated. */
clay_result clay_voxel_sculpt_flatten(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush, const float normal[3],
                                      float offset_cells);
/* Move surface cells one step toward the brush centre. */
clay_result clay_voxel_sculpt_pinch(clay_voxel_grid* grid, const int32_t cell[3],
                                    const clay_brush_params* brush);
/* ...and one step away from it: pinch's inverse, sharing its walk so the two
 * cannot drift apart. The SDF side spells the pair as one signed strength on
 * CLAY_DEFORM_MAGNIFY for the same reason. */
clay_result clay_voxel_sculpt_magnify(clay_voxel_grid* grid, const int32_t cell[3],
                                      const clay_brush_params* brush);
/* Translate occupancy through the same map the SDF grab deformer uses. Binary
 * occupancy resamples nearest-cell: a displacement larger than a cell moves
 * material in whole cells rather than flowing. */
clay_result clay_voxel_sculpt_grab(clay_voxel_grid* grid, const int32_t cell[3],
                                   const clay_brush_params* brush, const float displacement[3],
                                   int32_t front_only);

/* -- masks ----------------------------------------------------------------- */

/* A paintable scalar mask in [0, 1] gating how strongly an edit may act: the
 * effective brush weight at a cell becomes strength * (1 - mask), so a fully
 * masked cell is frozen and an unmasked one is untouched by the feature.
 *
 * The mask is addressed in WORLD units on its own lattice, deliberately not
 * in a layer's voxel cells, so changing a layer's resolution or moving
 * content between the SDF and voxel representations cannot silently drop or
 * misalign it. Its cell size is independent of any grid's.
 *
 * Masking gates edits where they are AUTHORED. Voxel edits consume a mask at
 * apply time, per cell. SDF edits are declarative items with no per-point
 * strength, so they consume it when a stroke becomes items — a mask painted
 * now does not retroactively gate items already in the edit list.
 *
 * Lifetime mirrors clay_voxel_grid exactly: clay_mask_create returns a mask
 * the CALLER destroys, while clay_document_add_mask and clay_document_mask
 * return one BORROWED from a document layer, which clay_mask_destroy rejects
 * and which must not outlive its document. */

/* A standalone mask, owned by the caller. cell_size is world units per mask
 * cell and must be > 0. NULL on invalid input, detail in clay_last_error. */
clay_mask* clay_mask_create(float cell_size);
/* Frees a mask from clay_mask_create; rejects a borrowed handle. */
clay_result clay_mask_destroy(clay_mask* mask);

/* Attach a mask to a layer and hand back a borrowed handle. Replaces any mask
 * the layer already had. */
clay_result clay_document_add_mask(clay_document* doc, clay_layer_id layer, float cell_size,
                                   clay_mask** out_mask);
/* The layer's mask, or CLAY_ERROR_NOT_FOUND when it has none. */
clay_result clay_document_mask(clay_document* doc, clay_layer_id layer, clay_mask** out_mask);
/* Drop a layer's mask. CLAY_ERROR_NOT_FOUND when there was none. Borrowed
 * handles onto it then fail with CLAY_ERROR_NOT_FOUND rather than dangling. */
clay_result clay_document_remove_mask(clay_document* doc, clay_layer_id layer);

clay_result clay_mask_cell_size(const clay_mask* mask, float* out_cell_size);
clay_result clay_mask_painted_count(const clay_mask* mask, size_t* out_count);
/* Non-zero when nothing has been painted. Equivalent to a zero painted count,
 * and the cheap way to ask. */
clay_result clay_mask_empty(const clay_mask* mask, int32_t* out_empty);

/* Single-cell access, on the MASK's lattice. Values are clamped to [0, 1]. */
clay_result clay_mask_get(const clay_mask* mask, const int32_t cell[3], float* out_value);
clay_result clay_mask_set(clay_mask* mask, const int32_t cell[3], float value);

/* Mask at a world position, 0 where nothing has been painted. */
clay_result clay_mask_sample(const clay_mask* mask, const float point[3], float* out_value);
/* Batch form: points is a packed float array of count*3, out_values receives
 * count floats. Neither is a size query — the caller knows the count. */
clay_result clay_mask_sample_many(const clay_mask* mask, const float* points_xyz, size_t count,
                                  float* out_values);

/* Brush the mask toward `target` (clamped to [0, 1]): 1 masks, 0 erases. The
 * footprint is the ordinary brush vocabulary, sized in MASK cells; the seed
 * is unused, because a mask stores a fractional weight directly rather than
 * dithering it. brush->mask is ignored here — a mask does not gate itself. */
clay_result clay_mask_paint(clay_mask* mask, const float point[3],
                            const clay_brush_params* brush, float target);
clay_result clay_mask_paint_cell(clay_mask* mask, const int32_t cell[3],
                                 const clay_brush_params* brush, float target);

/* Region operations over the painted region. invert flips what has been
 * painted: a sparse field has no finite complement to invert into. */
clay_result clay_mask_invert(clay_mask* mask);
clay_result clay_mask_clear(clay_mask* mask);
clay_result clay_mask_expand(clay_mask* mask, int32_t steps);   /* grey dilation */
clay_result clay_mask_contract(clay_mask* mask, int32_t steps); /* grey erosion */
clay_result clay_mask_smooth(clay_mask* mask, int32_t iterations);

/* Inclusive cell bounds of the painted region. *out_has_bounds is 0 for an
 * empty mask, and out_min/out_max are then left alone. */
clay_result clay_mask_bounds(const clay_mask* mask, int32_t out_min[3], int32_t out_max[3],
                             int32_t* out_has_bounds);

/* -- the cut tool ---------------------------------------------------------- */

/* A shape drawn over the model, resolved into an ordinary edit item.
 *
 * The caller gives the FRAME the shape was drawn on — an origin and an
 * orthonormal basis, which a viewport already has because it needed one to
 * draw the overlay — and the shape in WORLD units on that frame. Not pixels
 * and not normalized device coordinates: this engine has no viewport and does
 * not want one.
 *
 * The cut is a PRISM, not a frustum. A shape drawn under a perspective camera
 * sweeps a converging wedge, and cutting with one would give a cut face that
 * is not flat and a solid that depends on where the camera was standing. A
 * trim is a straight cut, as it is in ZBrush and 3DCoat.
 *
 * Which side survives is the OP the resolved item is placed with:
 * CLAY_OP_SUBTRACT removes what the shape covers, CLAY_OP_INTERSECT keeps only
 * that. A separate flag would be a second way to say one thing. */

typedef enum clay_cut_shape {
    CLAY_CUT_RECT = 0,
    CLAY_CUT_CIRCLE = 1,
    CLAY_CUT_POLYGON = 2 /* outline in polygon_xy, closed implicitly */
} clay_cut_shape;

typedef struct clay_cut_desc {
    uint32_t struct_size; /* = sizeof(clay_cut_desc); required */
    float origin[3];
    float right[3];   /* unit, in-plane: shape x is measured in these world units */
    float up[3];      /* unit, in-plane */
    float forward[3]; /* unit, the sweep direction */
    int32_t shape;    /* clay_cut_shape */
    float half_width; /* CLAY_CUT_RECT */
    float half_height;
    float radius;   /* CLAY_CUT_CIRCLE */
    float rounding; /* bevels the cut walls; >= 0 */
    /* The region being cut, used only to size the sweep so that a cut goes all
     * the way through instead of stopping inside it. */
    float region_min[3];
    float region_max[3];
    /* Sweep extent either side of the origin along forward. Both zero means
     * "derive it from the region", which is what a caller wants unless it is
     * asking for a deliberate partial cut. */
    float near_extent;
    float far_extent;
} clay_cut_desc;

/* Resolve a cut into an item the caller places like any other, or NULL with
 * the detail in clay_last_error: a frame that is not orthonormal is refused
 * rather than squared up, because the shape the user saw was drawn in the
 * frame they think they have. `polygon_xy` is count*2 floats and is read only
 * for CLAY_CUT_POLYGON. Free the result with clay_item_destroy. */
clay_item* clay_cut_create(const clay_cut_desc* desc, const float* polygon_xy,
                           size_t polygon_count);

/* A closed control-point curve drawn in the cut plane, flattened into a
 * polygon outline through the same tessellator curves use — so a spline lasso
 * follows the same curve a spline item would. Size-query pattern: call with
 * out_xy == NULL to receive the vertex count in *count. `points_xyzr` and the
 * optional `types` are as clay_item_set_curve_points takes them; only x and y
 * are read, since the outline lies in the cut plane. */
clay_result clay_cut_polygon_from_curve(const float* points_xyzr, size_t count,
                                        const int32_t* types, float tolerance, float* out_xy,
                                        size_t* out_count);

/* -- brush strokes --------------------------------------------------------- */

/* A drag becomes stamps, and a stamp becomes an ordinary edit.
 *
 * Resolution is pure: samples and a preset go in, stamps come out, and no
 * document is read or touched. Application turns those stamps into ordinary
 * voxel brush stamps or ordinary edit-list nodes, so undo, stroke coalescing,
 * `.clayspace` serialization and picking apply to a stroked edit without any
 * of them knowing this exists.
 *
 * It is also where a mask reaches SDF edits. An SDF item is declarative and
 * has no per-point strength, so gating it at evaluation time would cost the
 * rigidity and finite support per-brick culling depends on. It is gated here
 * instead, where the item is authored: a stamp in a frozen region emits
 * nothing at all. */

/* One moment of a drag: position, pressure in [0,1], tilt in radians. Samples
 * cross this ABI as a packed float array of count*5, in this order, so a
 * caller's (N, 5) buffer needs no repacking. */
typedef struct clay_stroke_sample {
    float position[3];
    float pressure;
    float tilt;
} clay_stroke_sample;

/* One resolved stamp: where an edit goes and how strong it is. */
typedef struct clay_stamp {
    float position[3];
    float radius;
    float strength;
    float rotation[4]; /* xyzw quaternion */
    float along;       /* [0,1] along the stroke */
} clay_stamp;

/* How overlapping stamps in one stroke combine. */
typedef enum clay_accumulation {
    CLAY_ACCUMULATION_BUILDUP = 0, /* passing twice acts twice */
    CLAY_ACCUMULATION_CLAMPED = 1  /* the stroke reaches its strength once */
} clay_accumulation;

/* How a drag becomes stamps. A versioned descriptor like the others, and
 * additionally a thing a user SAVES: a preset library outlives the engine
 * version that wrote it, which is why clay_stroke_preset_serialize tags its
 * output with a schema version from the first release rather than gaining one
 * later. struct_size is the in-memory contract; the schema version is the
 * on-disk one, and they move independently. */
typedef struct clay_stroke_preset {
    uint32_t struct_size; /* = sizeof(clay_stroke_preset); required */
    float radius;         /* world units; must be > 0 */
    float spacing;        /* stamp spacing as a fraction of the DIAMETER; > 0 */
    float strength;
    float pressure_size;     /* 0 = pressure does not drive the radius */
    float pressure_strength; /* 0 = pressure does not drive the strength */
    float pressure_curve;    /* exponent applied to pressure before either */
    float jitter_position;   /* fraction of the radius */
    float jitter_size;       /* fraction of the radius */
    float jitter_rotation;   /* radians */
    uint32_t seed;           /* jitter is a hash of the stamp index and this */
    int32_t rotate_along_stroke;
    float taper_start; /* fraction of the stroke the radius ramps in over */
    float taper_end;
    float steady;        /* lazy-mouse lag; 0 follows exactly, ->1 lags more */
    int32_t accumulation; /* clay_accumulation */
} clay_stroke_preset;

/* Fill a descriptor with the engine's defaults, struct_size included. The one
 * way to get a valid preset without knowing every field, and what a caller
 * should start from before overriding what it cares about. */
clay_result clay_stroke_preset_defaults(clay_stroke_preset* out_preset);

/* Preset <-> bytes, tagged with the schema version. Serialization uses the
 * size-query pattern: call with out_data == NULL for the size. Loading a
 * preset from an OLDER schema succeeds, taking defaults for what it did not
 * carry; one from a NEWER schema is refused with CLAY_ERROR_INVALID_ARGUMENT
 * rather than read as a prefix, because the fields that would be skipped can
 * change what the ones read mean. */
clay_result clay_stroke_preset_serialize(const clay_stroke_preset* preset, uint8_t* out_data,
                                         size_t* count);
clay_result clay_stroke_preset_deserialize(const uint8_t* data, size_t size,
                                           clay_stroke_preset* out_preset);
/* The schema version this build writes. */
uint32_t clay_stroke_preset_version(void);

/* Resolve samples into stamps, via the size-query pattern: call with
 * out_stamps == NULL to receive the count, then again with a buffer of that
 * many. *count is the capacity going in and the count written coming out. */
clay_result clay_stroke_resolve(const float* samples_xyzpt, size_t sample_count,
                                const clay_stroke_preset* preset, clay_stamp* out_stamps,
                                size_t* count);

/* Resolve a stroke and stamp it into a grid. `index` is the palette entry to
 * set, or 0 to erase; `mask` may be NULL. *out_applied receives how many
 * stamps actually ran — a stamp in a frozen region is dropped, not weakened
 * to nothing. */
clay_result clay_voxel_apply_stroke(clay_voxel_grid* grid, const float* samples_xyzpt,
                                    size_t sample_count, const clay_stroke_preset* preset,
                                    int32_t index, int32_t shape, int32_t falloff,
                                    const clay_mask* mask, size_t* out_applied);

/* -- the Move brush -------------------------------------------------------- */

typedef struct clay_move_params {
    uint32_t struct_size; /* = sizeof(clay_move_params); required */
    float radius;         /* the drag's radius in WORLD units; must be > 0 */
    int32_t ease;         /* falloff curve across the region */
    int32_t front_only;   /* non-zero: do not drag the far side of a form */
} clay_move_params;

/* Drag a layer's assembled SURFACE — ZBrush's Move. Named apart from
 * clay_layer_move, which reparents a node in the tree: these move different
 * things and confusing them would be easy.
 *
 * NOT the same as putting a grab deformer on an item, which is what a host
 * reaching for clay_item_add_deformer gets. A deformer is per ITEM and its
 * centre is in that item's LOCAL frame, so a grab drags one item's own field:
 * on a form blended from several, it pulls that item's share and leaves the
 * rest behind. This resolves the drag against every item the region reaches,
 * maps it into each one's frame, and puts it at the FRONT of each chain —
 * which is where a warp has to go to act on the assembled shape.
 *
 * With undo enabled the whole drag is ONE step however many items it touched.
 *
 * *out_applied receives how many items took a warp, so a host can tell "the
 * drag reached nothing" from "the drag did nothing visible". A drag that
 * reaches nothing succeeds and changes nothing.
 *
 * The surface moves LESS than the displacement asked for — the region weight
 * is taken at the sample point rather than at its preimage, so a drag of 0.5
 * over a radius of 0.8 moves a tip about 0.31. That is grab's documented
 * behaviour and the pull is monotonic, so a UI can calibrate against it. */
/* Which nodes a move WOULD warp, without touching the document, so a host can
 * preview a drag before committing it. Size-query pattern: call with
 * out_nodes == NULL to receive the count in *out_count, then again with a
 * buffer of that size. */
clay_result clay_layer_move_surface_preview(const clay_document* doc, clay_layer_id layer,
                                            const float centre[3],
                                            const float displacement[3],
                                            const clay_move_params* params,
                                            clay_node_id* out_nodes, size_t capacity,
                                            size_t* out_count);

clay_result clay_layer_move_surface(clay_document* doc, clay_layer_id layer,
                                    const float centre[3],
                            const float displacement[3], const clay_move_params* params,
                            size_t* out_applied);

/* Add one domain warp to a node ALREADY IN a document, undoably.
 *
 * clay_item_add_deformer builds an item; this edits a placed one, which
 * nothing could do before — a deformer could only be set when its node was
 * created, so no verb built on one could touch an existing sculpt.
 *
 * `at_front` non-zero puts the warp at the head of the chain rather than the
 * tail. It matters: deformers apply in authoring order, so the FIRST is the
 * outermost warp on the geometry, and one appended at the back has its region
 * weight read at a point the earlier deformers already moved. A warp meant to
 * act on the finished shape goes at the front. */
clay_result clay_layer_add_deformer(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t deform, const float* params, size_t param_count,
                                    int32_t ease, int32_t at_front);

/* Resolve a stroke and append one edit per stamp to a layer, using `item` as
 * the stamp template scaled to each stamp's radius. The builder is left
 * untouched. With undo enabled the whole stroke is ONE step.
 *
 * This is NOT a size-query call: it applies the stroke exactly once, however
 * it is called. `count` is the capacity of out_nodes going in and the number
 * of nodes created coming out; if the buffer was too small the first capacity
 * ids are written and *count still reports the true total. out_nodes may be
 * NULL when the ids are not wanted.
 *
 * A caller that wants every id can size the buffer first with
 * clay_stroke_resolve, which is pure and gives the same stamp count for the
 * same input — an upper bound, since a masked stamp emits no node. */
clay_result clay_layer_apply_stroke(clay_document* doc, clay_layer_id layer,
                                    const float* samples_xyzpt, size_t sample_count,
                                    const clay_stroke_preset* preset, const clay_item* item,
                                    const clay_mask* mask, clay_node_id* out_nodes,
                                    size_t* count);

/* Fill pockets inside the footprint: an empty cell with at least four of its
 * six face neighbours occupied is inside a cavity rather than beside a
 * surface, and `passes` iterations reach that many cells deep. A through-hole,
 * an open face and a wide shallow dent are left alone — smoothing is the verb
 * for surface irregularity. `passes` must be > 0.
 *
 * This began as a morphological closing, which cannot do the job: a ball of
 * radius r fits INTO a dent wider than r, so a larger element fills less, and
 * the erosion reaches through from the void behind a one-cell wall and reopens
 * every hole the dilation just sealed. */
clay_result clay_voxel_sculpt_fill_cavities(clay_voxel_grid* grid, const int32_t cell[3],
                                            const clay_brush_params* brush, int32_t passes);

/* Flatten onto the plane AND smooth, from ONE snapshot. Calling flatten then
 * smooth is not the same thing: the flatten's output would feed the smooth's
 * neighbourhood, which is what the snapshot discipline exists to prevent. A
 * zero-length normal is rejected, as it is for flatten. */
clay_result clay_voxel_sculpt_scrape(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush, const float normal[3],
                                     float offset_cells);

/* Drag SURFACE material along a direction, leaving the interior where it was.
 * That is the difference from grab, which translates every cell in its region:
 * grab moves a lump, smudge smears a skin. */
clay_result clay_voxel_sculpt_smudge(clay_voxel_grid* grid, const int32_t cell[3],
                                     const clay_brush_params* brush,
                                     const float displacement[3]);

/* Carve modulated by a caller-supplied alpha: `alpha_width * alpha_height`
 * samples in [0, 1], row-major, projected onto the plane perpendicular to
 * `direction`. `index` 0 carves and a non-zero one deposits.
 *
 * The engine decodes no images and never will at this boundary: a host that
 * has an alpha has already loaded the PNG, and handing over the samples costs
 * it nothing while costing the engine no format zoo. */
clay_result clay_voxel_sculpt_carve_alpha(clay_voxel_grid* grid, const int32_t cell[3],
                                          const clay_brush_params* brush, const float* alpha,
                                          int32_t alpha_width, int32_t alpha_height,
                                          const float direction[3], int32_t index);

/* -- repair ---------------------------------------------------------------- */

/* What a pre-bake check wants to know, without performing the repair: a
 * destructive operation whose input is somebody's sculpt should be askable
 * before it is answerable. */
typedef struct clay_repair_report {
    uint32_t struct_size; /* = sizeof(clay_repair_report); required */
    size_t enclosed_voids; /* empty regions the outside cannot reach */
    size_t void_cells;     /* their total size */
    size_t largest_void;
    int32_t airtight; /* non-zero when there are no enclosed voids at all */
} clay_repair_report;

clay_result clay_voxel_repair_report(const clay_voxel_grid* grid,
                                     clay_repair_report* out_report);

/* Seal perforations by the same pocket rule the fill-cavities verb uses. Only
 * ever adds cells, so no material is lost. `passes` must be > 0; `mask` may be
 * NULL, and where one is given a fully masked cell is left alone — a repair
 * that ignored freeze would be the one destructive operation here that does. */
clay_result clay_voxel_repair_close_holes(clay_voxel_grid* grid, int32_t passes,
                                          const clay_mask* mask);

/* Fill every empty cell the outside cannot reach, coloured from the shell that
 * encloses it. Reachability is over EMPTY cells by face adjacency, seeded
 * outside the occupied bounds, so enclosure is decided rather than guessed at
 * from a local neighbourhood. */
clay_result clay_voxel_repair_fill_voids(clay_voxel_grid* grid, const clay_mask* mask);

/* -- queries --------------------------------------------------------------- */

clay_result clay_voxel_occupied_count(const clay_voxel_grid* grid, size_t* out_count);
/* Inclusive cell bounds of the occupied voxels. *out_has_bounds is 0 for an
 * empty grid, and out_min/out_max are then left alone. */
clay_result clay_voxel_bounds(const clay_voxel_grid* grid, int32_t out_min[3],
                              int32_t out_max[3], int32_t* out_has_bounds);

/* 6-connected flood select from a seed cell, over the seed's palette index
 * when same_color is non-zero or over any occupied cell otherwise, via the
 * size-query pattern: call with out_cells == NULL to receive the cell count
 * in *count, then again with a buffer of count*3 int32 values. *count is the
 * capacity in CELLS going in and the count written coming out; a buffer that
 * is too small gets CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in
 * *count. An empty seed cell selects nothing, which is not an error. The
 * traversal order is deterministic, so both calls agree, and the selection is
 * capped at 1048576 cells. */
clay_result clay_voxel_flood_select(const clay_voxel_grid* grid, const int32_t seed[3],
                                    int32_t same_color, int32_t* out_cells, size_t* count);

/* The grid as a step field for SDF compositing: -voxel_size/2 inside an
 * occupied cell and +voxel_size/2 elsewhere (a bound, not a distance —
 * classify accordingly). points_xyz is count*3 floats, out_values count. */
clay_result clay_voxel_sample_step_field(const clay_voxel_grid* grid, const float* points_xyz,
                                         size_t count, float* out_values);

/* -- meshing and rasterization --------------------------------------------- */

/* Greedy mesh (merged quads per axis slice, palette color per face) behind
 * the same owner handle as clay_document_mesh, freed with clay_mesh_destroy.
 * An empty grid yields an empty mesh rather than an error. */
clay_result clay_voxel_mesh(const clay_voxel_grid* grid, clay_mesh** out_mesh);

/* Rasterize a document's SDF content into the grid: cells whose centre lands
 * inside are set, colored from the field through the nearest palette entry
 * (added as needed). region_min/region_max bound the work in world space and
 * are both NULL to use the document's own bounds; passing one without the
 * other is rejected, as is a region that is empty, unbounded, or carries any
 * non-finite bound — an infinity or a NaN, which is what a region derived
 * from a camera frustum or a degenerate selection box arrives as. A rejected
 * call leaves the grid exactly as it was. */
clay_result clay_voxel_rasterize(clay_voxel_grid* grid, const clay_document* doc,
                                 const float region_min[3], const float region_max[3]);

/* -- voxel picking --------------------------------------------------------- */

/* The cube face a picking ray entered a cell through. The neighbour across it
 * is where a voxel placed by that click goes, which is what
 * clay_voxel_raycast's out_adjacent already holds. */
typedef enum clay_voxel_face {
    CLAY_VOXEL_FACE_POS_X = 0,
    CLAY_VOXEL_FACE_NEG_X = 1,
    CLAY_VOXEL_FACE_POS_Y = 2,
    CLAY_VOXEL_FACE_NEG_Y = 3,
    CLAY_VOXEL_FACE_POS_Z = 4,
    CLAY_VOXEL_FACE_NEG_Z = 5
} clay_voxel_face;

/* Cell picking by ray (origin plus a direction normalized here). *out_hit is
 * 0/1 and the rest is written only on a hit: out_cell is the first occupied
 * cell along the ray, *out_face the clay_voxel_face it was entered through,
 * out_adjacent the neighbour across that face, and *out_t the world distance
 * to the entry point. Every out pointer but out_hit may be NULL. */
clay_result clay_voxel_raycast(const clay_voxel_grid* grid, const float origin[3],
                               const float dir[3], int32_t* out_hit, int32_t out_cell[3],
                               int32_t* out_face, int32_t out_adjacent[3], float* out_t);

/* The cell under the ray on the build plane y = plane_cell — where a voxel
 * goes when the ray hits none. Build planes are Y-normal, so the returned
 * cell's y is always plane_cell. *out_hit is 0 when the ray runs parallel to
 * the plane or the plane lies behind its origin, which is not an error. */
clay_result clay_voxel_build_plane_pick(const clay_voxel_grid* grid, const float origin[3],
                                        const float dir[3], int32_t plane_cell,
                                        int32_t* out_hit, int32_t out_cell[3]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLAY_H */
