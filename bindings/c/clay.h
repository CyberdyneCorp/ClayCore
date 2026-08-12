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
#define CLAY_ABI_MINOR 29
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
    CLAY_PRIM_VOLUME = 33,
    /* A TREE of spheres, skinned by one sphere-swept cone per node-parent pair
     * — ZBrush's ZSpheres. The nodes are a stroke's points; the topology is the
     * parent array beside them. An armature whose parents form a line IS a
     * stroke, and evaluates identically to one. */
    CLAY_PRIM_ARMATURE = 34
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
     * blend_k saturates at the item's extent. A surface point moves only
     * while it stays inside the region (the item inflated by its rounding),
     * so the displacement equals blend_k only until blend_k reaches how far
     * that region extends past the surface along the normal — for a sphere
     * stamp centered on the surface, its radius plus the rounding. Past that
     * the falloff gives sharply diminishing returns, and radius + 2*rounding
     * is never reached; a host's depth slider mapped to blend_k should top
     * out near the extent rather than run past it.
     *
     * rounding = 0 therefore costs amplitude as well as edge softness: the
     * ceiling drops to the item's bare radius, and the rim becomes a step
     * whose declared steepness — amplitude over falloff width, the marcher's
     * cost model — is effectively unbounded. For a standard clay brush set
     * blend_k = rounding = the item's radius: the stamp then raises the
     * surface by exactly blend_k with a soft rim, and reaches no further
     * than 2*rounding outside the item.
     *
     * A pair rather than one signed amplitude, because blend_k is required
     * non-negative — and because add/subtract and engrave/emboss are pairs. */
    CLAY_OP_RELIEF = 14, /* build up: ZBrush Standard, ClayBuildup */
    CLAY_OP_INCISE = 15, /* cut in:   Crease, DamStandard */
    /* GROUPS ONLY (clay_layer_add_group): the group's children apply inline to
     * the chain outside it, exactly as if they had been added there. Every
     * other op makes the group a sub-expression that combines as a unit. An
     * item carrying it is refused. */
    CLAY_OP_INLINE = 255
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
    int32_t mirror;      /* layer-mirror participation: 0 and 1 follow the
                          * layer's mirror (the default — a zeroed field
                          * mirrors), -1 excludes this item from it. See
                          * clay_set_layer_mirror. */
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

/* Retransform a node. axis/angle give the rotation; scale must be > 0.
 * A GROUP is refused: the engine composes layer * item and nothing else, so a
 * group's transform never reaches its children — recording one would be an
 * undoable, saved edit that changes nothing. Transform the children. */
clay_result clay_layer_set_transform(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float position[3], const float rotation_axis[3],
                                     float rotation_angle, float scale);
/* Replace a node's primitive. Its deformers, repetition, profile and stroke
 * belong to the node, not to the primitive, and survive the edit. */
clay_result clay_layer_set_prim(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                int32_t prim, const float* params, size_t param_count);
clay_result clay_layer_set_color(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                 const float rgb[3]);
/* op is clay_op, blend is clay_blend; rounding must be >= 0. A GROUP takes the
 * same values clay_layer_add_group takes, CLAY_OP_INLINE included; an item
 * takes anything but that one. */
clay_result clay_layer_set_op_blend(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                    int32_t op, int32_t blend, float blend_k, float rounding);
/* Reparent or reorder. new_parent 0 moves the node to the layer root, and
 * index < 0 appends. Moving a node into its own subtree is refused: it would
 * close a cycle, and the subtree would leave the root list and be dropped on
 * the next save. */
clay_result clay_layer_move(clay_document* doc, clay_layer_id layer, clay_node_id node,
                            clay_node_id new_parent, int32_t index);
/* Append (x, y, z, radius) quadruples to a placed stroke: points_xyzr holds
 * count * 4 floats. */
clay_result clay_layer_append_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     const float* points_xyzr, size_t count);
/* Remove the last count points from a placed stroke. */
clay_result clay_layer_trim_stroke(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                   uint32_t count);
/* A GROUP is a node too, and every call above applies to one. Creating and
 * filling one is further down, with the item builder whose handle it takes:
 * see clay_layer_add_group. */

/* -- editing layers -------------------------------------------------------- */

clay_result clay_document_remove_layer(clay_document* doc, clay_layer_id layer);
/* Reorder a layer. The command vocabulary expresses this as remove-then-add,
 * so it is the one edit that is a pair rather than a single command. */
clay_result clay_document_move_layer(clay_document* doc, clay_layer_id layer, int32_t index);
/* A hidden layer contributes nothing to the field; showing it again restores
 * the original field exactly. */
clay_result clay_document_set_layer_visible(clay_document* doc, clay_layer_id layer,
                                            int32_t visible);
/* Rename a layer — the setter for the name clay_layer_name reads back. Through
 * 0.29.1 a layer was named by whichever call created it and nothing could
 * change it, so a host that let the artist rename a layer kept that name
 * beside the document and lost it on the next save (#92). clay_layer_name made
 * that visible rather than caused it: a reopened document now confidently
 * reports the stale creation name, which looks correct.
 *
 * A command like every other layer edit, so one rename is one undo step and a
 * ghosted or locked layer refuses it. A rename invisible to undo would be a
 * new inconsistency, not a fix.
 *
 * NULL and the empty string are refused: an empty name is what a cleared text
 * field submits, and the document's name is the only one left to lose. There
 * is no length limit — the saved layer record length-prefixes the name and
 * clay_layer_name sizes before it writes, so a long name costs the reader a
 * bigger buffer rather than a truncation.
 *
 * Names are NOT unique, here or at creation: clay_add_sdf_layer,
 * clay_document_add_voxel_layer and clay_document_add_mesh_layer each accept a
 * name another layer already carries, so refusing a duplicate here would buy a
 * guarantee the create calls do not keep. What a duplicate means is stated
 * instead: clay_document_voxel_layer and clay_document_mesh_layer answer with
 * the FIRST layer in stack order carrying the name, so renaming a voxel layer
 * onto a name already in use shadows the other layer's grid, and renaming it
 * away leaves the old name looking up nothing. Hold the id from creation or
 * from clay_document_layer_at when the lookup has to survive a rename — ids
 * are stable across save and load; names are not a key anything enforces. */
clay_result clay_document_set_layer_name(clay_document* doc, clay_layer_id layer,
                                         const char* name);
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
/* Symmetry. Each enabled axis reflects the layer's items through the plane
 * where that LOCAL coordinate is 0 (the layer transform moves the plane with
 * the layer), and every item participates: place a lump on one side and both
 * sides carry it, through evaluation, meshing and both raycast paths alike.
 * The mirror is a property of the layer that evaluation reads, not an edit
 * baked into the items — so it applies to items added before OR after this
 * call, and axes 0/0/0 turns it back off, restoring the unmirrored field.
 *
 * `mirror_k` is the Mirror Blend seam: 0 is a hard crease on the plane, and a
 * positive value smooth-blends an item with its own reflection over that
 * distance, welding the seam where an item crosses its mirror plane.
 *
 * Individual items opt OUT with mirror = -1 (clay_item_desc.mirror or
 * clay_item_set_mirror) — an asymmetric detail on an otherwise symmetric
 * layer. Through 0.27.3 the flag was an opt-IN whose default excluded every
 * item, which made this call a silent no-op unless each item also passed
 * mirror = 1 (#60); 1 is still accepted and means what it meant. Layers with
 * no mirror axes evaluate exactly as before, whatever the items' flags. */
clay_result clay_set_layer_mirror(clay_document* doc, clay_layer_id layer, int32_t axis_x,
                                  int32_t axis_y, int32_t axis_z, float mirror_k);

/* -- discovering layers ----------------------------------------------------
 *
 * The read half of the layer surface (#69). Everything a host can set about a
 * layer — name, visibility, protection, representation, stack position — is
 * readable back, so a document that comes back from clay_document_load comes
 * back whole rather than anonymous. Before this existed the only recourse was
 * probing ids against clay_layer_bounds, which recovers ids alone, in id
 * order — and id order is creation order, so a document reordered with
 * clay_document_move_layer EVALUATED differently after a reload.
 *
 * Reading is not editing: a ghosted, locked or hidden layer answers every
 * query below normally. */

/* How a layer carries its content. The values match the layer record's kind
 * byte in a saved document, and new kinds are appended, never renumbered. */
typedef enum clay_layer_representation {
    CLAY_LAYER_SDF = 0,   /* an edit tree (clay_add_sdf_layer) */
    CLAY_LAYER_VOXEL = 1, /* a sampled grid (clay_document_add_voxel_layer) */
    CLAY_LAYER_MESH = 2   /* imported triangles (clay_document_add_mesh_layer) */
} clay_layer_representation;

/* Count-then-index enumeration, in STACK ORDER — index 0 is the layer
 * evaluated first, exactly the index clay_document_move_layer takes. An index
 * of count or beyond is CLAY_ERROR_NOT_FOUND. Ids are stable across
 * save/load, so enumerate once and hold the ids; they are not dense — a
 * removal leaves a gap — which is why enumeration goes through an index
 * rather than the id space. */
clay_result clay_document_layer_count(const clay_document* doc, size_t* out_count);
clay_result clay_document_layer_at(const clay_document* doc, size_t index,
                                   clay_layer_id* out_layer);

/* Everything about one layer in one call. An OUTPUT descriptor: set
 * struct_size to the sizeof of the struct you compiled against and the
 * library fills what you declared (see descriptor struct versioning). */
typedef struct clay_layer_info {
    uint32_t struct_size;   /* = sizeof(clay_layer_info); required */
    clay_layer_id id;       /* the id queried, so the struct is self-contained */
    int32_t representation; /* clay_layer_representation */
    int32_t stack_index;    /* position in evaluation order; what
                             * clay_document_move_layer sets */
    int32_t visible;        /* what clay_document_set_layer_visible set */
    int32_t ghost;          /* both what clay_document_set_layer_protection */
    int32_t locked;         /* set; also clay_document_layer_protection */
} clay_layer_info;

clay_result clay_document_layer_info(const clay_document* doc, clay_layer_id layer,
                                     clay_layer_info* out_info);

/* The layer's UTF-8 name by the size-query pattern clay_list_backends uses:
 * call with buffer == NULL to receive the required size (incl. NUL) in
 * *size; call again with an adequate buffer to fill it. A buffer that is too
 * small gets CLAY_ERROR_BUFFER_TOO_SMALL with the needed size in *size and
 * writes nothing. The name is a string rather than a clay_layer_info field
 * because it is the one layer property without a fixed size. What a layer is
 * called after creation is clay_document_set_layer_name's to say. */
clay_result clay_layer_name(const clay_document* doc, clay_layer_id layer, char* buffer,
                            size_t* size);

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
/* Layer-mirror participation, the builder's spelling of clay_item_desc.mirror:
 * -1 excludes the item from the layer's mirror, 0 and 1 follow it (the
 * default). See clay_set_layer_mirror. */
clay_result clay_item_set_mirror(clay_item* item, int32_t mirror);

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
/* The topology half of an armature: one parent index per node, where the nodes
 * are the points clay_item_set_stroke_points takes. A node whose parent is
 * itself is a root. Split in two because an armature IS a stroke plus a tree,
 * and the points already had a setter worth reusing.
 *
 * An index outside the node range, or a parent chain that closes a cycle, is
 * refused: a cycle would make the field depend on traversal order rather than
 * on the tree. */
clay_result clay_item_set_armature_parents(clay_item* item, const uint32_t* parents,
                                           size_t count);
clay_result clay_item_add_stroke_point(clay_item* item, const float position[3], float radius);
/* Append one armature node under `parent`, the incremental authoring path that
 * clay_item_add_stroke_point is for a chain. `parent` < 0 continues from the
 * last node, which is what dragging a new sphere out of the previous one does.
 * Only armatures have children, so the name needs no qualifier. */
clay_result clay_item_add_child(clay_item* item, const float position[3], float radius,
                                int32_t parent);
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
 * bookkeeping granular commands would need, and its inverse is exact.
 *
 * Also edits a CLAY_PRIM_SWEPT node's guide, which is the same list, and keeps
 * the two rules clay_layer_add_item enforces on one: closing it is refused for
 * the reason clay_item_set_curve refuses it, and so is cutting it below two
 * points, which would leave the sweep with nothing to follow. */
/* Tree edits for a placed armature. ONE undo step whatever the edit, because
 * the command underneath is a whole-tree replace — an armature is tens of
 * nodes, so that costs less than granular bookkeeping and its inverse is
 * exactly the tree that was there.
 *
 *   CLAY_ARMATURE_ADD_CHILD   `value` is the position, under `target`
 *   CLAY_ARMATURE_MOVE        `value` is a DELTA, and `target`'s whole subtree
 *                             moves with it — an arm hangs from a shoulder
 *   CLAY_ARMATURE_SET_RADIUS  `radius` on `target`
 *   CLAY_ARMATURE_DELETE      `target` and everything under it
 *
 * `mirrored` applies to add-child only: it adds the reflection through x = 0 in
 * the same step, under the mirror of the parent where there is one. A node on
 * the plane is its own reflection and is added once. */
#define CLAY_ARMATURE_ADD_CHILD  0
#define CLAY_ARMATURE_MOVE       1
#define CLAY_ARMATURE_SET_RADIUS 2
#define CLAY_ARMATURE_DELETE     3
clay_result clay_layer_armature_edit(clay_document* doc, clay_layer_id layer, clay_node_id node,
                                     int32_t op, uint32_t target, const float value[3],
                                     float radius, int32_t mirrored);

clay_result clay_layer_set_stroke_points(clay_document* doc, clay_layer_id layer,
                                         clay_node_id node, const float* xyzr, size_t count,
                                         const int32_t* types, const float* in_handles_xyz,
                                         const float* out_handles_xyz, int32_t closed,
                                         float tolerance);

/* Read a placed stroke or curve's point list back — the exact arguments the
 * call above takes, so what comes out goes straight back in. The item builder
 * is deliberately write-only: the host that filled it already knows what it
 * put there. A host that RELOADS a document knows nothing, and without this it
 * cannot edit a curve — or a swept tube's guide — it did not author.
 *
 * A CLAY_PRIM_ARMATURE node answers here too: its nodes are the same x, y, z,
 * radius list the setter shares with the stroke, so this call serves the
 * geometry half of a rig and clay_layer_armature_parents below the topology
 * half, mirroring the split on the setter side. An armature has no curve
 * settings, so out_closed reads 0 and out_tolerance the default it never
 * consults.
 *
 * Size-query pattern, as clay_cut_polygon_from_curve uses: call with
 * out_xyzr == NULL to receive the point count in *count, then again with a
 * buffer of count*4 floats. *count is the capacity in POINTS going in and the
 * count written coming out; a buffer that is too small gets
 * CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and writes
 * nothing. The three optional arrays are sized off that same count and each may
 * be NULL exactly as on the setter, but a size query takes none of them —
 * nothing sizes them yet, so passing one is refused rather than ignored.
 * out_closed and out_tolerance may be NULL and are written on the size query
 * too, so one call answers "how many points, and is it closed".
 *
 * The points come back AS AUTHORED, not tessellated: tessellation happens when
 * the document is compiled, so a readback is the control points and round trips
 * through the setter unchanged.
 *
 * Reading is not editing: a ghosted, locked or hidden layer answers normally,
 * because protection refuses edits. */
clay_result clay_layer_stroke_points(const clay_document* doc, clay_layer_id layer,
                                     clay_node_id node, float* out_xyzr, size_t* count,
                                     int32_t* out_types, float* out_in_handles_xyz,
                                     float* out_out_handles_xyz, int32_t* out_closed,
                                     float* out_tolerance);

/* The topology half of a placed armature, as clay_item_set_armature_parents
 * took it: one parent index per node, a root naming itself. A separate call
 * rather than another parameter on the reader above because the setters are
 * already split, and for the setters' own reason: an armature IS a stroke plus
 * a tree, and parents are the other half of a different primitive, not an
 * optional attribute a curve happens to lack. A node that is not an armature
 * is therefore CLAY_ERROR_INVALID_ARGUMENT.
 *
 * Same size-query pattern as the call above, counted in NODES: out_parents ==
 * NULL receives the node count in *count, and a buffer that is too small gets
 * CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and writes
 * nothing. The count is the one the xyzr readback reports — the two halves are
 * parallel arrays — and it holds even for a tree authored with fewer parents
 * than points: the missing tail reads back as roots, which is exactly the
 * reading compilation makes of it, so what comes back is always the tree the
 * document EVALUATES. For any tree the setter accepted whole, the round trip
 * is exact.
 *
 * The indices are the ones clay_layer_armature_edit takes, which is what makes
 * a reloaded rig posable at all: read the tree, pick the subtree, edit by
 * index. Reading is not editing: a ghosted, locked or hidden layer answers
 * normally. */
clay_result clay_layer_armature_parents(const clay_document* doc, clay_layer_id layer,
                                        clay_node_id node, uint32_t* out_parents,
                                        size_t* count);

/* Parameters of a spatial morph, in world space. Required by the matching
 * transition op and rejected with any other op: linear morphs along the
 * segment a -> b, radial over the XZ radius from r0 to r1. */
clay_result clay_item_set_transition_linear(clay_item* item, const float a[3],
                                            const float b[3], int32_t ease);
clay_result clay_item_set_transition_radial(clay_item* item, float r0, float r1, int32_t ease);

/* Appends the composed edit to a layer. The builder is left untouched. */
clay_result clay_layer_add_item(clay_document* doc, clay_layer_id layer, const clay_item* item,
                                clay_node_id* out_node);

/* -- groups ----------------------------------------------------------------
 * A group is a node like any other — same id space, same commands, same undo —
 * whose children compile as ONE sub-expression. That is what makes "intersect
 * A with B, then union that into C" sayable from a host: the intersect applies
 * to A alone because it is inside the group, and the group's own op combines
 * the result with whatever the layer already holds. Without one, an op applies
 * to the whole accumulated field, so an intersect meant for a single panel
 * would trim everything added before it.
 *
 * Three rules the compiler already enforces (src/scene/tape_build.cpp,
 * compile_group), stated here because a host has to be able to predict them:
 *   - a carving group (subtract, intersect, the extended modes) with nothing
 *     beneath it in the chain emits nothing at all — as a carving ITEM does;
 *   - so does a group whose children all turned out to be hidden or culled,
 *     down to the last instruction: an empty subtree is rolled back rather
 *     than left as a combine against empty space;
 *   - CLAY_OP_INLINE reads no blend, no rounding and no colour off the group,
 *     which is why those are refused rather than quietly ignored.
 *
 * A group has no transform of its own; see clay_layer_set_transform. */

/* Creates an empty group and returns its id. parent 0 puts it at the layer
 * root and index < 0 appends; a parent that is not a group is refused, which
 * is also how a nested group is built — pass the outer group's id.
 *
 * op is clay_op, blend is clay_blend, and blend_k and rounding must both be
 * >= 0. The transition ops are refused: compile_group emits no transition
 * parameters, so a group carrying one would morph on defaults nobody wrote.
 * CLAY_OP_INLINE requires CLAY_BLEND_HARD with blend_k and rounding 0.
 *
 * The seed colour a CLAY_OP_SHELL or CLAY_OP_REPLACE group paints when it
 * starts a chain against empty space is clay_layer_set_color, as for any other
 * node. */
clay_result clay_layer_add_group(clay_document* doc, clay_layer_id layer, clay_node_id parent,
                                 int32_t index, int32_t op, int32_t blend, float blend_k,
                                 float rounding, clay_node_id* out_node);

/* clay_add_item and clay_layer_add_item append to the layer root and have no
 * argument that could say otherwise, so these are those two calls with a group
 * to put the edit in. index < 0 appends. One command each, hence one undo step
 * each: filling a group needs no clay_document_begin_undo_group. */
clay_result clay_add_item_in_group(clay_document* doc, clay_layer_id layer, clay_node_id group,
                                   int32_t index, const clay_item_desc* item,
                                   clay_node_id* out_node);
clay_result clay_layer_add_item_in_group(clay_document* doc, clay_layer_id layer,
                                         clay_node_id group, int32_t index,
                                         const clay_item* item, clay_node_id* out_node);

/* A group's children, in order, by the size-query pattern
 * clay_layer_stroke_points uses: call with out_children == NULL to receive the
 * count in *count, then again with a buffer of that many ids. *count is the
 * capacity going in and the count written coming out; a buffer that is too
 * small gets CLAY_ERROR_BUFFER_TOO_SMALL with the needed count in *count and
 * writes nothing.
 *
 * A node that is not a group is CLAY_ERROR_INVALID_ARGUMENT — which is also
 * how a host that RELOADED a document tells a group from an item, since before
 * clay_layer_node_prim below nothing else in this ABI answered that question.
 * Reading is not editing, so a ghosted, locked or hidden layer answers
 * normally. */
clay_result clay_layer_children(const clay_document* doc, clay_layer_id layer,
                                clay_node_id node, clay_node_id* out_children, size_t* count);

/* Which primitive a placed item carries — clay_prim, the value
 * clay_item_create took. This is what lets a host that RELOADED a document
 * FIND its armature (or any other item worth a typed reader) instead of
 * probing every node against readers that refuse until one does not: ask what
 * the node is, then call the reader that applies.
 *
 * A group carries no primitive and is CLAY_ERROR_INVALID_ARGUMENT — the dual
 * of clay_layer_children refusing an item, so between the two calls every node
 * answers exactly one question. Reading is not editing: a ghosted, locked or
 * hidden layer answers normally. */
clay_result clay_layer_node_prim(const clay_document* doc, clay_layer_id layer,
                                 clay_node_id node, int32_t* out_prim);


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

/* -- consolidating a degraded chain ----------------------------------------
 *
 * The region verbs — relax, flatten, snakehook, move, the mask brush — each
 * work once and none of them chain. A polish samples a document and hands back
 * a volume, so the second pass samples a VOLUME, and the declared Lipschitz
 * goes 1.7 -> 24 -> 39 over three passes. A move stroke never touches a volume
 * at all: each drag appends a grab to the deformer chain, and those multiply,
 * so the safe step scale decays by a constant factor per drag — 79x the
 * marching cost by nine.
 *
 * Two mechanisms, so the report names them separately. An aggregate step scale
 * says something is wrong; steepest_volume and longest_deformer_chain say
 * which thing, and therefore which of the two an app should show.
 *
 * The trigger is ADVISORY. Nothing here bakes on its own: consolidating
 * discards the parameters of everything it absorbs, and an engine that decided
 * on an artist's behalf that a sphere's radius is no longer editable would be
 * making the wrong person pay. `advise_below_step_scale` is the CALLER's
 * tolerance for marching cost, passed per call rather than stored in the
 * document, because that tolerance belongs to a viewport, a device and a frame
 * budget rather than to the artwork. */
typedef struct clay_field_report {
    uint32_t struct_size; /* = sizeof(clay_field_report); required */
    float lipschitz;      /* the compiled layer's declared bound */
    float safe_step_scale;
    float steepest_volume;         /* largest sample Lipschitz among volume items */
    int32_t longest_deformer_chain;
    int32_t item_count;
    int32_t advises_consolidation; /* safe_step_scale < advise_below_step_scale */
} clay_field_report;

/* What a layer's chain currently costs the marcher, and what is causing it.
 * Pass 0 for advise_below_step_scale to measure without asking for advice. */
clay_result clay_layer_field_report(const clay_document* doc, clay_layer_id layer,
                                    float advise_below_step_scale,
                                    clay_field_report* out_report);

typedef struct clay_consolidation_params {
    uint32_t struct_size; /* = sizeof(clay_consolidation_params); required */
    /* Required, and > 0. A document has no intrinsic scale to derive a
     * resolution from the way a mesh's own bounds give one, so guessing here
     * would fix the shape's resolution at a number nobody chose. */
    float cell_size;
    float band;    /* half-width of the band kept; <= 0 means three cells */
    float padding; /* past the layer's bounds; <= 0 means the band */
    /* Redistancing — replacing the baked samples with the distance to their
     * own zero set — is what actually bounds the Lipschitz. Baking alone does
     * NOT: resampling a steep field reproduces the steepness, and a finer cell
     * makes it worse rather than better. Spelled as a SKIP so that a zeroed
     * struct gets the sound behaviour rather than the fast one. */
    int32_t skip_redistance;
} clay_consolidation_params;

/* What consolidating spends, from what a volume already reports. */
typedef struct clay_consolidation_cost {
    uint32_t struct_size; /* = sizeof(clay_consolidation_cost); required */
    float cell_size;
    float band;
    uint64_t brick_count;
    uint64_t sample_count;
    uint64_t bytes;
    float sample_lipschitz; /* how fast the stored samples vary */
    float lipschitz;        /* what the compiler will declare for them */
    float safe_step_scale;
    float bounds_min[3];
    float bounds_max[3];
} clay_consolidation_cost;

/* What consolidating this layer WOULD cost, without the document changing.
 *
 * The numbers are the ones the real thing produces, because this is the real
 * thing with the result thrown away: an estimate that skipped the sampling
 * could not report a brick count, and the brick count is where the memory is.
 * A caller that means to go ahead should call clay_layer_consolidate and read
 * the same cost out of it rather than paying for two bakes.
 *
 * `region_min`/`region_max` are an optional world-space box, or NULL for the
 * layer's own bounds padded. Pin it when consolidating the SAME region
 * repeatedly: a volume's geometric bound is its whole sampled box, so each
 * bake would otherwise pad the previous padding. */
clay_result clay_layer_consolidation_cost(const clay_document* doc, clay_layer_id layer,
                                          const clay_consolidation_params* params,
                                          const float region_min[3], const float region_max[3],
                                          clay_consolidation_cost* out_cost);

/* Collapse a layer's edit list into one item carrying samples, as ONE undo
 * step whose inverse restores what it absorbed — ids, parameters, colours and
 * deformers intact, because the undo record carries the removed subtrees by
 * value. No new command was needed for this; the vocabulary could already say
 * it, which is why this is a policy rather than a verb.
 *
 * Refused on a protected layer, and refused BEFORE the bake, so a locked layer
 * does not cost a full resampling to say no.
 *
 * What survives: the surface, at `cell_size`. What does not: every parameter
 * of every item absorbed, and every colour but the first one's. Hidden items
 * are left alone — they contribute nothing to the field, so absorbing them
 * would spend their parameters on nothing.
 *
 * `out_cost` may be NULL. Undo grouping is the document's own, so this lands
 * as a single step when undo is enabled and as a plain edit when it is not. */
clay_result clay_layer_consolidate(clay_document* doc, clay_layer_id layer,
                                   const clay_consolidation_params* params,
                                   const float region_min[3], const float region_max[3],
                                   clay_consolidation_cost* out_cost);

/* Whether a layer is consolidated — its edit list is a single item carrying
 * samples — and at what resolution, so a host can stop offering parameter
 * edits there rather than failing them.
 *
 * Answered from the CONTENT, not from a stored provenance flag. The promise a
 * host makes is about what the region IS: samples at a fixed resolution, with
 * no parameters to offer. A mesh imported with clay_item_volume_from_mesh is
 * exactly as unparametric as a bake, so a flag marking one of them would split
 * two cases an app has to treat alike — and it would have to be serialised to
 * survive a save.
 *
 * *out_consolidated is 0/1; out_cost may be NULL and is left untouched when
 * the layer is not consolidated. */
clay_result clay_layer_consolidation_state(const clay_document* doc, clay_layer_id layer,
                                           int32_t* out_consolidated,
                                           clay_consolidation_cost* out_cost);

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
/* Frees a mesh the caller owns. A mesh BORROWED from a document layer (see
 * clay_document_mesh_layer) is owned by the document, and destroying one is a
 * no-op that leaves the document intact. It is a silent no-op rather than the
 * reported refusal clay_voxel_grid_destroy gives because this call returns no
 * status, and changing its signature would break every existing consumer for a
 * case that could not previously arise. */
void clay_mesh_destroy(clay_mesh* mesh);

size_t clay_mesh_vertex_count(const clay_mesh* mesh);
size_t clay_mesh_index_count(const clay_mesh* mesh);
/* Borrowed pointers, valid until clay_mesh_destroy. normals/colors may be
 * NULL when absent. */
const float* clay_mesh_positions(const clay_mesh* mesh);
const float* clay_mesh_normals(const clay_mesh* mesh);
const float* clay_mesh_colors(const clay_mesh* mesh);
/* count * 2, or NULL when absent. Meshers and the OBJ importer produce uvs and
 * the OBJ and GLB writers consume them; before this they were the one mesh
 * attribute the boundary could not read. */
const float* clay_mesh_uvs(const clay_mesh* mesh);
const uint32_t* clay_mesh_indices(const clay_mesh* mesh);

/* Where each attribute goes in ONE interleaved vertex, so a mesh reaches a
 * mapped GPU buffer in a single pass instead of an interleave into a staging
 * vector followed by a copy — two passes over geometry that was just produced,
 * on the frame path. This does for meshes what clay_brick_cache_read_bricks
 * does for bricks: the destination is the caller's.
 *
 * Offsets are BYTES from the start of a vertex, and -1 omits the attribute.
 * Every attribute is float32 here because it is float32 in the mesh: this
 * descriptor says WHERE a value goes, not what it is converted to. Format
 * conversion is a second axis and would mean choosing a format enumeration for
 * four attributes before anyone has asked for one.
 *
 * Widths: position 12 bytes, normal 12, colour 12, uv 8. stride 0 means
 * "tightly packed", computed as the end of the last attribute named — which is
 * only well defined because the offsets are yours, so a packed layout is the
 * one you described with no gaps rather than an order this header picks. */
typedef struct clay_vertex_layout {
    uint32_t struct_size; /* = sizeof(clay_vertex_layout); required */
    uint32_t stride;      /* bytes per vertex; 0 = tightly packed */
    int32_t position_offset;
    int32_t normal_offset;
    int32_t color_offset;
    int32_t uv_offset;
} clay_vertex_layout;

/* Copies count vertices into dst in the layout described, and the indices into
 * a caller's buffer. dst_bytes must be exactly stride * clay_mesh_vertex_count
 * and dst_count exactly clay_mesh_index_count — required rather than inferred,
 * as everywhere else here, and a short destination is refused rather than
 * truncated. Both counts are already queryable, so the two-call shape costs
 * nothing.
 *
 * Naming an attribute the mesh does not carry is REFUSED, not zero-filled: a
 * silently black or silently flat model is harder to diagnose than a returned
 * error. So is a layout whose attributes overlap, or a stride that does not
 * clear the attributes it was asked to hold — the two mistakes that produce a
 * buffer which is wrong without being obviously wrong. */
clay_result clay_mesh_copy_vertices(const clay_mesh* mesh, const clay_vertex_layout* layout,
                                    void* dst, size_t dst_bytes);
clay_result clay_mesh_copy_indices(const clay_mesh* mesh, uint32_t* dst, size_t dst_count);

/* The box enclosing the mesh's positions — how a host frames an imported
 * model. It is answered here rather than by clay_layer_bounds because that
 * query is derived from SDF shapes and would report an empty box for a mesh
 * layer. An empty mesh returns CLAY_ERROR_INVALID_ARGUMENT rather than an
 * inverted box. */
clay_result clay_mesh_bounds(const clay_mesh* mesh, float out_min[3], float out_max[3]);

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

/* -- mesh layers: a document CARRIES a mesh -------------------------------- */

/* The other reason to import a model. clay_item_volume_from_mesh samples one
 * into a field so it can be sculpted; a mesh LAYER keeps the triangles the
 * importer produced, verbatim, for display and re-export. A scan, a scale
 * reference, a kit part — geometry that must leave the pipeline as what it
 * entered as.
 *
 * A mesh layer is never evaluated. It is not compiled into a tape, takes no
 * part in a blend, contributes to no influence bound and is not pickable; its
 * geometry is held beside the document rather than in it, so that is
 * structural rather than a rule this ABI has to keep. clay_document_mesh is
 * unaffected: it means "mesh the field" and still does.
 *
 * Its transform, visibility, ghost, lock and ordering are the ordinary layer
 * ones, edited through the ordinary layer calls. */

typedef struct clay_mesh_layer_desc {
    uint32_t struct_size; /* = sizeof(clay_mesh_layer_desc); required */
    const char* name;     /* the layer's name; required */
    /* This layer's own ceiling, tighter than the loader's by default: what a
     * document may CARRY is a different question from what a file may decode
     * into, and the loader's defaults are 50M vertices. Zero means the
     * library's default for that field. */
    uint64_t max_vertices;
    uint64_t max_triangles;
    /* Uniform scale baked into the stored vertices, so unit conversion is
     * resolved once at import rather than approximated by a layer transform.
     * <= 0 means 1. Non-uniform scale is not expressible and is not
     * approximated. */
    float import_scale;
} clay_mesh_layer_desc;

/* Attaches an ALREADY-LOADED mesh (clay_mesh_load, clay_mesh_from_triangles,
 * or one this library meshed) as a new layer, copying its geometry. Taking a
 * handle rather than a path keeps import policy in one place and lets a host
 * attach geometry it generated itself.
 *
 * Goes through the same layer vocabulary every other layer creation uses, so
 * an enabled undo stack records it and it serializes with the document. Over
 * the descriptor's budget returns CLAY_ERROR_BUDGET_EXCEEDED and the document
 * is unchanged. `out_mesh` is BORROWED — see the lifetime note above
 * clay_document_add_voxel_layer. */
clay_result clay_document_add_mesh_layer(clay_document* doc, const clay_mesh* mesh,
                                         const clay_mesh_layer_desc* desc,
                                         clay_layer_id* out_layer, clay_mesh** out_mesh);

/* Borrows the mesh of an existing mesh layer by name; CLAY_ERROR_NOT_FOUND
 * when the document has no mesh layer carrying that name. Names are not
 * unique, so this answers with the FIRST mesh layer in stack order carrying
 * the name, and it follows clay_document_set_layer_name — see there. */
clay_result clay_document_mesh_layer(clay_document* doc, const char* name,
                                     clay_layer_id* out_layer, clay_mesh** out_mesh);

/* The layer a borrowed mesh belongs to — what the ordinary layer calls
 * (transform, visibility, ghost, lock, ordering, removal) take. A mesh the
 * caller owns belongs to no layer and returns CLAY_ERROR_NOT_FOUND. */
clay_result clay_mesh_layer(const clay_mesh* mesh, clay_layer_id* out_layer);

/* -- combining meshes for export -------------------------------------------
 *
 * `clay_document_mesh` means MESHING THE FIELD and keeps meaning exactly that.
 * It prices a dense grid from the tape's own bounds, and geometry that is not
 * in the tape would either inflate that grid or fall outside it — and it would
 * change what an existing call returns for an existing document. Voxel layers
 * are outside it for the same reason. Combining is therefore explicit, and
 * these are the three calls that do it. */

/* A copy of `mesh` with every position moved by the transform, and normals
 * rotated but not translated or scaled — a uniform scale leaves a direction
 * unchanged, and adding the position would turn a direction into a point.
 *
 * Takes a transform on the same terms as every other transform in this ABI:
 * `position` and `rotation_axis` are required and the axis must be non-zero,
 * `scale` must be > 0. A second convention for "no rotation" would be one
 * more thing to get wrong. Free the result with clay_mesh_destroy. */
clay_result clay_mesh_transform(const clay_mesh* mesh, const float position[3],
                                const float rotation_axis[3], float rotation_angle,
                                float scale, clay_mesh** out_mesh);

/* One mesh from many, with indices rebased onto the concatenated vertices.
 *
 * AN ATTRIBUTE PRESENT ON SOME INPUTS AND ABSENT ON OTHERS IS DROPPED from the
 * result. It is not padded and not truncated: a mesh whose normals, colors or
 * uvs are non-empty and a different length than its positions is malformed,
 * and no call in this ABI may return one. So concatenating a mesh that carries
 * uvs with one that does not yields a mesh with no uvs at all. Said here
 * because the alternative is discovering it in an exported file.
 *
 * `meshes` is `count` mesh pointers, none NULL. Free the result with
 * clay_mesh_destroy. */
clay_result clay_mesh_concat(const clay_mesh* const* meshes, size_t count,
                             clay_mesh** out_mesh);

/* The convenience call: mesh the document's field with `params`, then append
 * every VISIBLE mesh layer under its own layer transform, indices rebased.
 *
 * Hidden mesh layers are excluded, because hidden means "contributes nothing".
 * Ghost and lock do NOT change what is exported, consistent with neither flag
 * changing what a document evaluates to.
 *
 * The attribute-drop rule of clay_mesh_concat applies: the meshed field
 * carries normals, so a mesh layer without them costs the result its normals.
 * A document with no visible mesh layer returns exactly what
 * clay_document_mesh would. Free the result with clay_mesh_destroy. */
clay_result clay_document_mesh_combined(const clay_document* doc,
                                        const clay_mesh_params* params,
                                        clay_mesh** out_mesh);

/* -- importing a mesh as a field ------------------------------------------- */

/* Declared here rather than with the mask entry points below, because the
 * relax and flatten parameter blocks freeze against one. See -- masks --. */
typedef struct clay_mask clay_mask; /* opaque */

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
    /* How far inside the sampled box a CLAY_OP_REPLACE placement crossfades
     * from the surrounding field to this volume, in world units. <= 0 — and
     * any struct_size from before this field existed — is the hard replace,
     * byte-identical to what every prior release did.
     *
     * WHY: a hard replace holds BOTH fields live at the surface. A bake put
     * straight back ties with the field beneath it at every sample plane, and
     * min/max branch switching between two fields that touch is what
     * corrugates the normals at the cell wavelength (issue #67) — the zero
     * set is exact, the shading is not — while the box faces meet the outside
     * field at a hard edge. With a feather, deep inside the box the result IS
     * the volume (one gradient, no ties), outside the box the surrounding
     * field continues untouched, and the two crossfade over this margin.
     *
     * The blend's correction is clamped at the volume's BAND, which keeps the
     * declared field slope at max + band * 1.5 / feather (the document's safe
     * step scale drops to match — feather about one band is the sweet spot)
     * and keeps per-brick culling exact. It also means a verb that moved the
     * surface further than the band from what sits beneath is expressed only
     * up to the band across the feather margin: bake with a band that covers
     * the verb, which is the accuracy contract a volume already has.
     *
     * A feathered volume placed with CLAY_OP_REPLACE does not participate in
     * the layer mirror (the crossfade follows ONE sampled box); every other
     * op ignores the feather. Applies to every producer that takes this
     * struct: the mesh import, the document bake, and the _from verbs. */
    float feather;
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
    /* Optional freeze, exactly as the voxel verbs take one: the weight at a
     * sample is scaled by (1 - mask) at its WORLD position, so a fully masked
     * sample keeps its value verbatim. NULL for none. Appended after the
     * original layout, so struct_size decides whether it is read at all and a
     * caller compiled before it existed is unaffected. */
    const clay_mask* mask;
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
 *
 * WHERE A DOCUMENT EXISTS, use clay_item_volume_relax_from below instead: it
 * samples the document, which is exact everywhere, in one call. This call is
 * for a volume with no document behind it — an imported mesh: bring in a
 * scan, then smooth it. */
clay_result clay_item_volume_relax(clay_item* item, const clay_relax_params* params);

/* The same relax, sampled from a DOCUMENT rather than applied to an existing
 * volume — the counterpart clay_item_volume_flatten_from is to the flatten,
 * and the one to reach for when a document is what you have.
 *
 * The relationship to bake-then-relax is exact: this call samples the
 * document precisely as clay_item_volume_from_document would and relaxes
 * those samples in place, so inside the band the result is identical to
 * baking first — a test holds that. What it removes is the two-call round
 * trip and the temptation to relax a volume that was itself derived from
 * another volume, where the band inaccuracy the flatten note above describes
 * compounds. Relax moves the surface by less than a cell per pass, so unlike
 * a flatten it can never walk out of the band it was baked with.
 *
 * `relax` is the relax itself, validated exactly as the in-place form
 * validates it. `volume` gives the sampling — cell_size required and > 0,
 * band and padding defaulting as they do for clay_item_volume_from_document,
 * feather honoured the same way. `region_min`/`region_max` are the same
 * optional pair, with the same rule: both NULL means the document's own
 * bounds padded by the band, and one without the other is refused.
 *
 * Returns a NEW item carrying the relaxed volume; the document is not
 * modified. Free it with clay_item_destroy, or place it with
 * clay_layer_add_item. */
clay_result clay_item_volume_relax_from(const clay_document* doc,
                                        const clay_relax_params* relax,
                                        const clay_volume_params* volume,
                                        const float region_min[3],
                                        const float region_max[3],
                                        clay_item** out_item);

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
    /* Optional freeze; see clay_relax_params.mask. NULL for none. Appended
     * behind `mode`, which shipped first, so a descriptor sized to either
     * earlier layout still describes exactly what it carried. */
    const clay_mask* mask;
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
 * rather than ignored.
 *
 * WHERE A DOCUMENT EXISTS, use clay_item_volume_flatten_from below instead:
 * it samples the document, which has no band, so the accuracy limit above
 * does not apply. This call is for a volume with no document behind it — an
 * imported mesh. */
clay_result clay_item_volume_flatten(clay_item* item, const clay_flatten_params* params);

/* The same flatten, sampled from a DOCUMENT rather than from an existing
 * volume — and the one to reach for when a document is what you have.
 *
 * The difference is accuracy, and it is not small. The call above re-samples
 * the item's own volume, which reports a distance only inside the band it
 * carries and a lower BOUND outside it; a facet that moves further than that
 * band is placed against the bound rather than against the surface, and the
 * result is a wrong shape returned with CLAY_OK. A document has no band. It
 * is exact everywhere, so a facet may move as far as the caller likes.
 *
 * `flatten` is the flatten itself, validated exactly as the in-place form
 * validates it. `volume` gives the sampling of the RESULT — cell_size is
 * required and > 0, because a document has no intrinsic scale to derive one
 * from; band and padding default as they do for
 * clay_item_volume_from_document. `region_min`/`region_max` are the same
 * optional pair that call takes, with the same rule: both NULL means the
 * document's own bounds padded by the band, and one without the other is
 * refused.
 *
 * Returns a NEW item carrying the flattened volume; the document is not
 * modified. Free it with clay_item_destroy, or place it with
 * clay_layer_add_item. */
clay_result clay_item_volume_flatten_from(const clay_document* doc,
                                          const clay_flatten_params* flatten,
                                          const clay_volume_params* volume,
                                          const float region_min[3],
                                          const float region_max[3],
                                          clay_item** out_item);

typedef struct clay_topological_move_params {
    uint32_t struct_size;  /* = sizeof(clay_topological_move_params); required */
    float anchor[3];       /* a point on or near the surface — what a pick gives */
    float radius;          /* the reach, measured ALONG THE MATERIAL; > 0 */
    float displacement[3]; /* how far to drag, in world units */
    int32_t ease;          /* falloff curve index */
} clay_topological_move_params;

/* ZBrush's Move Topological: a drag whose falloff is weighted by distance along
 * the MATERIAL rather than through space, so a part close in space but far along
 * the surface is not dragged with it.
 *
 * `radius` is therefore a distance of travel across the surface, not a straight
 * line, and cannot step over a gap however narrow. Measured on two fingers 0.32
 * apart joined only through a palm: a Euclidean drag at radius 0.5 pulls the far
 * one, this does not, and raising the radius past the path through the palm
 * brings it into reach — which is what makes the weight a distance and not a
 * mask.
 *
 * The item must carry a volume; anything else is refused rather than ignored.
 * It re-samples that volume with the move applied, and declares the Lipschitz
 * the result measured. clay_layer_move_surface is the cheaper Euclidean move and
 * does not bake — prefer it unless the form has parts close in space and far
 * along the surface. */
clay_result clay_item_volume_move_topological(clay_item* item,
                                              const clay_topological_move_params* params);

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
/* clay_mask is declared above, with the volume parameters that freeze against
 * one; the entry points are in -- masks -- below. */

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
 * when the document has no such layer. The name is not a unique key — nothing
 * has ever required one — so this answers with the FIRST voxel layer in stack
 * order carrying the name, and it follows clay_document_set_layer_name: hold
 * the id when a lookup has to survive a rename. */
clay_result clay_document_voxel_layer(clay_document* doc, const char* name,
                                      clay_layer_id* out_layer, clay_voxel_grid** out_grid);

/* Cell size of the grid's ACTIVE resolution level (see below). */
clay_result clay_voxel_size(const clay_voxel_grid* grid, float* out_voxel_size);

/* -- resolution levels ----------------------------------------------------- */

/* A grid holds a stack of levels: level 0 is the coarsest and level k has half
 * the cell size of level k-1. One is ACTIVE, and every other entry point in
 * this file acts on it — the level is grid state rather than an argument, so
 * nothing here changed shape and a caller that never mentions a level gets a
 * one-level grid behaving exactly as it did before these calls existed.
 *
 * Adding a level subdivides every occupied cell into its eight children, so the
 * solid is unchanged. Editing at a level averages down into the coarser ones
 * and replays into the finer ones from the offsets they hold, which is what
 * makes a broad stroke at a coarse level leave fine detail standing. Dropping a
 * level discards the detail only that level held. */
clay_result clay_voxel_level_count(const clay_voxel_grid* grid, size_t* out_count);
clay_result clay_voxel_active_level(const clay_voxel_grid* grid, size_t* out_level);
/* CLAY_ERROR_INVALID_ARGUMENT for a level the grid does not have, grid
 * untouched. */
clay_result clay_voxel_set_active_level(clay_voxel_grid* grid, size_t level);
/* Appends a level at half the finest cell size and reports its index. The stack
 * is capped, and CLAY_ERROR_INVALID_ARGUMENT at the cap: every level costs
 * eight times the cells of the one below, so a stream naming an arbitrary count
 * would be a request to allocate one. */
clay_result clay_voxel_add_level(clay_voxel_grid* grid, size_t* out_level);
/* Drops the finest level. CLAY_ERROR_INVALID_ARGUMENT when only one is left,
 * since a grid always has at least one. */
clay_result clay_voxel_drop_level(clay_voxel_grid* grid);
/* Cell size and occupied cells of ONE level, so a host can report what each
 * level of a stack costs without making it active first. */
clay_result clay_voxel_level_voxel_size(const clay_voxel_grid* grid, size_t level,
                                        float* out_voxel_size);
clay_result clay_voxel_level_occupied_count(const clay_voxel_grid* grid, size_t level,
                                            size_t* out_count);

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
 * neighbour the same call already changed.
 *
 * EVERY verb here can be a perfectly valid call that changes nothing: a
 * flatten on an already-flat region, a smudge shorter than a cell, a dithered
 * stamp that misses every cell it was offered, a footprint over empty space.
 * All of them return CLAY_OK, because they did what they were asked. There is
 * no "valid but had no effect" result code and there will not be one — an
 * existing entry point returning a new non-zero value would turn a success
 * into a failure for every caller already compiled. Read
 * clay_voxel_change_count before and after instead; the difference is what
 * moved. */

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
 * material in whole cells rather than flowing, and one shorter than half a
 * cell rounds back to the cell it started in and moves nothing at all. That is
 * not an error and is not reported as one — the call did exactly what it was
 * asked, which is why a drag tool fed raw pointer deltas looks dead: at a
 * voxel_size of 0.12 every per-sample delta of a slow drag is sub-cell, and
 * every call returns CLAY_OK.
 *
 * The rounding is PER AXIS, so a pull of 0.4 cells on each of the three is
 * 0.69 cells long and still moves nothing. Half a cell on the largest
 * component is the necessary condition, not a sufficient one: away from the
 * centre the falloff shrinks the pull further, front_only halves it at the
 * centre outright (the front gate is 0.5 on the plane through it, so the dead
 * zone is twice as wide), and material that moves into a cell holding the same
 * index changes nothing either way.
 *
 * So there are two things to reach for. clay_voxel_size gives the cell size,
 * which is what a host accumulates its drag against before calling — the verb
 * is stateless and has no gesture to accumulate over, so that belongs in the
 * host. And clay_voxel_change_count, read before and after, says what actually
 * happened. clay_voxel_occupied_count is not a substitute: grab conserves
 * material, so a lump dragged a whole cell can leave it identical. */
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

/* Fill a world-space box with a value, and take the complement over one.
 *
 * These are the BOUNDED forms clay_mask_invert cannot be. Inverting the painted
 * region is the only thing a sparse unbounded lattice can do, and it is not
 * what "mask a limb, invert, sculpt everything else" means: the untouched
 * storage stays unmasked and the boundary lands on chunk edges rather than on
 * the painted region. So the caller supplies the finite region — it always has
 * one, from a grid's bounds or an item's.
 *
 * A cell belongs to the box when its CENTRE does. An empty or unbounded box is
 * refused: the cost of both calls is the box's volume in cells. */
clay_result clay_mask_fill(clay_mask* mask, const float box_min[3], const float box_max[3],
                           float value);
clay_result clay_mask_invert_within(clay_mask* mask, const float box_min[3],
                                    const float box_max[3]);

/* clay_mask_apply_stroke — painting a mask along a drag — lives with the other
 * stroke consumers below, since it needs clay_stroke_preset. */

/* -- mask extrude ---------------------------------------------------------- */

/* Mask a patch of a surface and pull it off as a solid of a chosen thickness.
 * ZBrush calls it Extract, 3DCoat reaches it through Extrude from a frozen
 * area, and it is what a mask is FOR once it can do more than freeze.
 *
 * THE MASK IS THE REGION. Relax and flatten both need a region_radius because
 * they have no other way to know where to act; this does not, because the
 * painted region bounds itself. */

/* Which side of the source surface the new material sits on. */
typedef enum clay_extrude_side {
    CLAY_EXTRUDE_OUTWARD = 0, /* the plate sits ON the surface */
    CLAY_EXTRUDE_INWARD = 1,  /* the pocket */
    CLAY_EXTRUDE_CENTRED = 2, /* straddles it */
} clay_extrude_side;

typedef struct clay_mask_extrude_params {
    uint32_t struct_size;  /* = sizeof(clay_mask_extrude_params); required */
    float thickness;       /* wall thickness in world units; must be > 0 */
    int32_t side;          /* clay_extrude_side */
    float threshold;       /* what counts as masked; <= 0 means 0.5 */
    float border_round;    /* rounding radius on the rim; 0 is a hard edge */
    int32_t border_smooth; /* smoothing passes on a COPY of the mask; the caller's is kept */
    float cell_size;       /* sampling of the result; <= 0 means the mask's own */
    float band;            /* <= 0 means three cells */
} clay_mask_extrude_params;

/* The mask, measured: signed distance to the boundary of the masked region,
 * negative inside, as an item carrying a volume.
 *
 * This is the conversion the extrude is built on, exposed because a host wants
 * to preview that border. A mask is a [0,1] scalar on a lattice and not a
 * distance field; composing one into a field expression directly would put a
 * step in the result and the Lipschitz bound would become a fiction.
 *
 * `pad` widens the sampled region past the masked one, `band` and `cell_size`
 * are the usual sampling controls, and <= 0 takes the default for each. The
 * returned item is owned by the caller until it is added to a layer. An empty
 * mask is refused. */
clay_result clay_mask_to_field(const clay_mask* mask, float threshold, float band, float pad,
                               float cell_size, clay_item** out_item);

/* Extrude the masked patch of a layer's surface into a new item carrying a
 * volume. The layer is sampled, so what comes back is a snapshot rather than
 * something that tracks the source — the same bargain flatten makes.
 *
 * Refused, with a typed error rather than an empty item, when the mask is
 * empty, the thickness is not positive, the wall is thinner than a cell, or the
 * masked region never reaches the surface. That last one is the common mistake
 * and the one an empty item would disguise.
 *
 * The mask and the source layer are both left unmodified. */
clay_result clay_document_mask_extrude(clay_document* doc, clay_layer_id layer,
                                       const clay_mask* mask,
                                       const clay_mask_extrude_params* params,
                                       clay_item** out_item);

/* The same verb on a voxel grid, in CELL space: the masked cells of the
 * source's surface, thickened, carrying the source's colours. It does not go
 * through a sampled field — a grid already knows which of its cells are on its
 * surface, so resampling would cost a conversion and lose the palette.
 *
 * The two agree to within a voxel, which is the point. cell_size and band are
 * ignored here; the grid's own resolution is the only one available.
 *
 * The returned grid is owned by the CALLER — destroy it with
 * clay_voxel_grid_destroy — and neither the source nor the mask is modified. */
clay_result clay_voxel_mask_extrude(const clay_voxel_grid* grid, const clay_mask* mask,
                                    const clay_mask_extrude_params* params,
                                    clay_voxel_grid** out_grid);

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
/* -- the Tube tool --------------------------------------------------------- */

/* Nomad Sculpt's Tubes: a drawn path becomes a rope, pipe, tentacle or hair
 * strand along it.
 *
 * `point_type` is the smooth/sharp toggle and is the curve's own point type
 * (CLAY_POINT_*), because a tube's path is the same kind of curve every other
 * item takes. The three radii are Nomad's handles and are interpolated by ARC
 * LENGTH, so a path whose control points bunch does not bunch the taper.
 *
 * With no profile the tube is a swept SPHERE — an exact distance field, so the
 * safe step scale stays 1. Give a profile and it becomes a swept item instead:
 * a square or hexagonal cross-section at the cost of a bound field. That choice
 * is the profile rather than a separate flag. */
typedef struct clay_tube_params {
    uint32_t struct_size;  /* = sizeof(clay_tube_params); required */
    int32_t point_type;    /* CLAY_POINT_HARD / _SPLINE / _BSPLINE / _BEZIER */
    float radius_start;
    float radius_mid;
    float radius_end;
    int32_t closed;
    float tolerance;
    float blend_k;         /* smoothing between consecutive segments; 0 is plain */
} clay_tube_params;

/* Resolve a path into a tube. `points_xyz` is count*3 floats.
 *
 * `profile` is CLAY_PROFILE_* with its parameters in profile_params, or < 0 for
 * a round tube. Returns NULL for fewer than two points, or a radius that is
 * positive nowhere. Free the result with clay_item_destroy. */
clay_item* clay_tube_create(const float* points_xyz, size_t count,
                            const clay_tube_params* params, int32_t profile,
                            const float* profile_params, size_t profile_param_count);

/* Which half of the frame a trim's outline covers. The OP still decides that
 * half's fate: CLAY_OP_SUBTRACT removes it, CLAY_OP_INTERSECT keeps only it. */
typedef enum clay_trim_side {
    CLAY_TRIM_BELOW = 0,
    CLAY_TRIM_ABOVE = 1,
    CLAY_TRIM_LEFT = 2,
    CLAY_TRIM_RIGHT = 3
} clay_trim_side;

/* ZBrush's Trim Curve: an OPEN stroke drawn across the form, flattened and then
 * closed against the frame's own bounds on the side it covers, so the result is
 * an ordinary polygon outline for clay_cut_create.
 *
 * NOT clay_cut_polygon_from_curve with a flag. That one tessellates CLOSED and
 * is a spline lasso: joining a trim stroke's endpoints cuts a sliver between
 * them instead of dividing the frame. Different shapes from the same points.
 *
 * `extent_xy` is how far the closing edge reaches in the frame's own units.
 * Size-query pattern, as the lasso flattener uses: call with out_xy == NULL to
 * receive the vertex count in *out_count. */
clay_result clay_cut_polygon_from_open_curve(const float* points_xyzr, size_t count,
                                             const int32_t* types, int32_t side,
                                             const float extent_xy[2], float tolerance,
                                             float* out_xy, size_t* out_count);

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
    /* How much a stamp deposits, [0, 1]. Reaches a voxel stroke as the brush
     * strength. On an SDF layer it reaches the ops that have an amount:
     * CLAY_OP_RELIEF and CLAY_OP_INCISE scale their amplitude by it, and
     * CLAY_OP_ADD scales its whole deposit — 0 deposits nothing, 1 is the item
     * exactly as authored. Every other op ignores it; see
     * clay_layer_apply_stroke. */
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

/* Resolve a stroke and paint it into a mask. The third stroke consumer, and
 * what makes masking the same gesture as sculpting: spacing, pressure, taper,
 * steady stroke and jitter reach a mask stroke because they are resolved before
 * anything knows what a stamp will become.
 *
 * `target` is where each cell moves TO — 1 masks and 0 releases — so painting
 * and erasing are the same call.
 *
 * The footprint comes from each stamp's WORLD radius, converted to MASK cells
 * here rather than by the caller: a caller doing it by hand gets a stroke whose
 * width tracks the mask's resolution instead of the brush's radius.
 *
 * There is no mask argument: a mask does not gate its own painting, so every
 * stamp runs and *out_applied is the stamp count. */
clay_result clay_mask_apply_stroke(clay_mask* mask, const float* samples_xyzpt,
                                   size_t sample_count, const clay_stroke_preset* preset,
                                   float target, int32_t shape, int32_t falloff,
                                   size_t* out_applied);

/* Resolve a stroke and append one edit per stamp to a layer, using `item` as
 * the stamp template scaled to each stamp's radius. The builder is left
 * untouched. With undo enabled the whole stroke is ONE step.
 *
 * The preset's strength reaches an item where scaling preserves what its op
 * means. CLAY_OP_RELIEF and CLAY_OP_INCISE scale their amplitude (blend_k) by
 * it. CLAY_OP_ADD scales its whole deposit — the stamp's scale, rounding and
 * blend together — so strength 0 authors no node at all, 1 is the item
 * exactly as authored, and the response is monotonic in between; the
 * clamped-accumulation division does not apply, because overlapping unions do
 * not add up the way relief amplitudes do. Every other op ignores strength:
 * its blend_k is a radius, a depth or a half-thickness, and scaling one would
 * change the shape rather than the amount.
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
 * surface, and `passes` iterations reach that many cells deep. An open face and
 * a wide shallow dent are left alone — smoothing is the verb for surface
 * irregularity. `passes` must be > 0; larger than the model's longest side is
 * clamped, since past that every cell in the window is already decided.
 *
 * A through-hole is left alone only where it is WIDER THAN ONE CELL. The rule
 * is local and counts neighbours, so it cannot tell a pinhole from a pocket: a
 * one-cell perforation has its four lateral neighbours occupied and fills, the
 * same way repair's close-holes seals a pierced wall. Anything two cells or
 * more across stays open.
 *
 * This began as a morphological closing, which cannot do the job: a ball of
 * radius r fits INTO a dent wider than r, so a larger element fills less, and
 * the erosion reaches through from the void behind a one-cell wall and reopens
 * every hole the dilation just sealed.
 *
 * The geometry it acts on is freehand voxel work, and the everyday source is
 * the soft stamp: occupancy is binary, so any strength or falloff below 1 is
 * DITHERED against a hash of the cell coordinate, which leaves a pepper of
 * single-cell holes through the material it just deposited. A narrow erase,
 * magnify or grab leaves the same. This is not cosmetic — greedy meshing emits
 * six faces around every one of those holes, so closing them measurably cuts
 * the mesh a bake has to carry.
 *
 * Whether a call did anything is answerable rather than guessable: bracket it
 * with clay_voxel_change_count. A boolean or a rasterized mesh does not —
 * the void it leaves is sealed, so no cell there has four occupied neighbours
 * and this verb sees nothing. That is clay_voxel_repair_fill_voids' job: this
 * one fills what is NARROW, that one fills what is SEALED. */
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
 * grab moves a lump, smudge smears a skin.
 *
 * It shares grab's nearest-cell rounding and so shares its dead zone: a
 * displacement under half a cell on every axis reads each cell's source as
 * itself and writes back what was already there. CLAY_OK, nothing moved. */
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

/* Cell writes that actually CHANGED a cell, since the grid was constructed.
 *
 * Why this exists: an edit that is entirely legal and entirely without effect
 * is normal here, not exotic. A sub-cell grab or smudge rounds back to where
 * it started; a flatten meets an already-flat region; a dithered stamp misses
 * every cell; a footprint lands on empty space. Each returns CLAY_OK, which is
 * correct and also unhelpful to a host trying to tell "the drag reached
 * nothing" from "the drag did something". Diffing the grid is the only other
 * way to ask, and clay_voxel_occupied_count cannot answer it at all: grab and
 * magnify conserve material, so the count is identical either way.
 *
 * A changed cell is a write that changed the stored palette index — 0 to n, n
 * to 0, n to m. Rewriting a cell with the index it already holds is not
 * counted, so clay_voxel_palette_set, which recolours without touching voxel
 * data, does not move it either.
 *
 * Monotone and never reset, so only the DIFFERENCE between two reads means
 * anything; a grid handed back by clay_voxel_mask_extrude starts wherever its
 * construction left it. Two guarantees, of deliberately different strength:
 *   - delta == 0 exactly when the grid is byte-identical to before the
 *     bracketed calls. Universal — every verb writes through one funnel.
 *   - delta is the exact number of changed cells for every verb that writes
 *     each cell at most once, which is all of them except pinch and magnify.
 *     Those two clear a cell and write its colour into a neighbour the same
 *     call may visit later, so a cell can be counted twice and a 0->n->0 round
 *     trip can leave delta > 0 with the grid unchanged. For those it is an
 *     upper bound.
 *
 * This is NOT the unit of the out_applied parameters elsewhere in this header:
 * those count stamps run or items warped, this counts cells changed.
 *
 * uint64 rather than size_t deliberately — it is never reset, and a 32-bit
 * host would wrap it in a long session. A NULL out_count is tolerated. */
clay_result clay_voxel_change_count(const clay_voxel_grid* grid, uint64_t* out_count);

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

/* -- influence bounds ------------------------------------------------------ */

/* The world-space box outside which an item cannot change the field.
 *
 * NOT clay_layer_bounds, which is deliberately tight — the box to frame a
 * camera on. This is the box to DIRTY, and it differs in exactly three ways,
 * which is what the engine actually does (src/scene/bounds.cpp):
 *   - a GROUP's bound is dilated by the group's own blend support, so a child
 *     blended into its siblings does not leave a stale seam;
 *   - an INVISIBLE node contributes nothing;
 *   - some nodes have NO finite influence and say so, rather than claiming
 *     one: a non-local op (intersect, the spatial morphs) anywhere in the
 *     subtree, an infinite grid repeat, or an unbounded primitive (a plane, an
 *     infinite cylinder).
 * A plain LOCAL item's influence bound IS its geometry bound — already
 * dilated by rounding and blend support, as every bound here is. Nothing is
 * dilated by a deformer chain's Lipschitz factor; the engine does not do that.
 *
 * Three states through two flags, spelled as clay_layer_bounds spells them:
 *   *out_has_bounds 0            nothing to dirty; out_min/out_max untouched
 *   1, *out_infinite 0           the finite box in out_min/out_max
 *   1, *out_infinite 1           unbounded; out_min/out_max are left alone,
 *                                and the honest response is to dirty
 *                                everything, which is what passing NULL to
 *                                clay_brick_cache_mark_dirty means
 * A node the layer does not hold, or one that is hidden, reports no bounds
 * rather than failing: a selection outlives the nodes in it. A layer the
 * document does not hold is CLAY_ERROR_NOT_FOUND. */
clay_result clay_layer_node_influence_bound(const clay_document* doc, clay_layer_id layer,
                                            clay_node_id node, float out_min[3],
                                            float out_max[3], int32_t* out_has_bounds,
                                            int32_t* out_infinite);
/* The union over a layer's root nodes — what a first, full fill dirties. A
 * layer that shows nothing reports *out_has_bounds 0. */
clay_result clay_layer_influence_bound(const clay_document* doc, clay_layer_id layer,
                                       float out_min[3], float out_max[3],
                                       int32_t* out_has_bounds, int32_t* out_infinite);

/* -- dense grid evaluation ------------------------------------------------- */

/* nx*ny*nz lattice samples at origin + spacing * (i, j, k), x fastest — the
 * shape every backend already implements (eval::GridQuery) and the one thing
 * this ABI could not ask for. clay_eval_points can sample the same points,
 * but only as an unstructured batch against a tape compiled for the whole
 * document; a grid carries its own region, so the tape can be CULLED to it,
 * and that culling is where the brick cache's measured win lives. */
typedef struct clay_grid_query {
    uint32_t struct_size; /* = sizeof(clay_grid_query); required */
    float origin[3];      /* world position of sample (0, 0, 0) */
    float spacing;        /* world units between samples; must be finite and > 0 */
    int32_t dims[3];      /* nx, ny, nz; each > 0, product <= CLAY_MAX_BATCH */
} clay_grid_query;

/* Evaluates the grid into out_values (dims product floats, x fastest), with
 * out_colors_rgb (three times that) optional as it is on clay_eval_points.
 * backend NULL = "cpu".
 *
 * region_min/region_max are an optional CULL region: items whose influence
 * cannot reach it are dropped from the compiled tape, so a small grid in a
 * large scene costs what the grid covers rather than what the document holds.
 * Both NULL compiles the whole document. Passing one without the other is
 * rejected, as it is on clay_voxel_rasterize, as is a region that is empty or
 * carries a non-finite bound. The culled tape is compiled per call and not
 * cached: consecutive bricks want different regions, so a cache keyed on the
 * document alone would thrash.
 *
 * value_count is the caller's buffer length in FLOATS and must equal the dims
 * product exactly — the one argument this ABI cannot check against the
 * caller's memory, so it is required rather than inferred. */
clay_result clay_eval_grid(const clay_document* doc, const char* backend,
                           const clay_grid_query* grid, const float region_min[3],
                           const float region_max[3], float* out_values,
                           float* out_colors_rgb, size_t value_count);

/* -- the host parity fixture ----------------------------------------------- */

/* The fixture a host preview runs to PROVE it evaluates the same field
 * claycore bakes: a table of composed tapes, probe points, and this library's
 * own reference distance and colour at each probe, as JSON.
 *
 * `clay parity-fixture` writes the same bytes, and until now that CLI was the
 * only way to get them — which is no use to an app whose tests link the
 * framework rather than shell out to a tool that is not in the bundle. This is
 * that gate, reachable from a test target: generate it, evaluate the same tapes
 * with your own shader, and assert agreement within the tolerances the JSON
 * carries.
 *
 * The case table is chosen for what a hand-written preview gets WRONG rather
 * than for coverage: every blend profile against every smooth boolean, every
 * extended combine mode, the material-mix weights, a deformer chain,
 * repetition, the out-of-line blob, and a composed document. The blend cases
 * are probed across the seam, so a support-k quadratic smin where the engine
 * uses 4k fails here rather than at bake time — which is the drift that started
 * all of this.
 *
 * Size-query pattern, exactly as clay_list_backends: call with buffer == NULL
 * for the required size including the NUL, then again with a buffer that large.
 * It is deterministic — no clock, no RNG beyond a fixed seed — so two calls in
 * one build produce identical bytes and a host can diff them.
 *
 * It is a few hundred kilobytes and builds the whole table on each call. That
 * is a test-time cost and this is a test-time entry point; do not call it per
 * frame. */
clay_result clay_parity_fixture_json(char* buffer, size_t* size);

/* -- device interop -------------------------------------------------------- */

/* Backends create and own their devices, which is right for a headless library
 * and wrong for a host that was going to draw on a GPU anyway: results computed
 * on our device are copied to host memory and uploaded again to the device the
 * host draws from. On macOS both claycore and a wgpu host are on Metal, and on
 * Linux both are on Vulkan — the same buffer could serve both sides.
 *
 * These calls let you lend claycore the device you already have.
 *
 * WHAT THIS DOES AND DOES NOT DO. It makes evaluation OUTPUT device-resident.
 * It does NOT make the brick CACHE device-resident: generations, staleness,
 * band classification, fp16 quantization and the memory budget are host code
 * over host memory, and that is where a submitted brick becomes a stored brick.
 * So a host taking this path has its bricks computed straight into its own
 * buffer and then owns quantizing and uploading them — at which point the cache
 * is not in the loop and neither are its guarantees. If you want the cache's
 * correctness, use clay_brick_cache_read_bricks; if you want no host copy, use
 * this. Both are complete paths; neither is both.
 *
 * NO VENDOR HEADER REACHES THIS ONE. Native objects cross as void*, positioned
 * per API. A header that included vulkan.h would break every bindings generator
 * that reads this one, and would make the surface it declares depend on how the
 * library was built. */

typedef enum clay_device_api {
    CLAY_DEVICE_API_METAL = 0,
    CLAY_DEVICE_API_VULKAN = 1,
    CLAY_DEVICE_API_CUDA = 2
} clay_device_api;

/* Which handle goes in which slot, by API. Unused slots are ignored and should
 * be NULL:
 *
 *   METAL   0 id<MTLDevice>   1 id<MTLCommandQueue>
 *   VULKAN  0 VkInstance      1 VkPhysicalDevice   2 VkDevice   3 VkQueue
 *           plus queue_family, which MUST support compute
 *   CUDA    0 CUcontext       1 CUstream  — DECLARED BUT NOT IMPLEMENTED: the
 *           CUDA backend has no adoption path, so clay_device_adopt refuses
 *           this API. Said plainly here rather than left to be discovered:
 *           the enumerator exists so the layout does not shift when it lands.
 *
 * A fixed void*[6] rather than a union, because a union of vendor types is a
 * vendor header and a union of void* is this with worse ergonomics. */
typedef struct clay_device_desc {
    uint32_t struct_size; /* = sizeof(clay_device_desc); required */
    int32_t api;          /* clay_device_api */
    void* handles[6];
    uint32_t queue_family; /* Vulkan only; ignored elsewhere */
} clay_device_desc;

typedef struct clay_device clay_device; /* opaque */

/* Adopts the caller's device. Returns NULL — with the detail in
 * clay_last_error() — when the named API's backend is not compiled in, when the
 * handles are incomplete for that API, or when the backend has no adoption
 * path. That is a capability report, not a failure: a caller whose adoption is
 * refused falls back to the ordinary backend-name calls and gets IDENTICAL
 * values. Adoption changes where work runs, never what it computes.
 *
 * OWNERSHIP. The library retains nothing it did not create and destroys nothing
 * it did not make: your device, queue and instance are yours, and releasing the
 * clay_device leaves them alone. The library creates, destroys and waits on no
 * synchronization primitive of yours, and submits to your queue only inside a
 * call you made. Work issued during a call has COMPLETED when that call
 * returns, so nothing is left in flight with no way to know when it lands.
 *
 * THREADING. Calls on one clay_device are yours to serialize, exactly as they
 * are for clay_brick_cache. A GPU queue is not free-threaded and adding a lock
 * here would be a threading policy you did not ask for.
 *
 * The device must outlive the clay_device. Releasing yours first and then
 * calling through this handle is a use-after-free this ABI cannot detect. */
clay_device* clay_device_adopt(const clay_device_desc* desc);
void clay_device_release(clay_device* device);

/* Which backend the handle resolved to ("vulkan", "metal", ...), borrowed and
 * valid until release, so a host can log or display what it actually got. */
const char* clay_device_backend_name(const clay_device* device);

/* A slice of a buffer YOU own and keep owning. `handle` is a VkBuffer /
 * MTLBuffer / CUdeviceptr per the device's API. `size` is what is available
 * from `offset`, is REQUIRED, and is checked against the lattice: a destination
 * too small is refused with nothing written, as everywhere else here. */
typedef struct clay_device_buffer {
    uint32_t struct_size; /* = sizeof(clay_device_buffer); required */
    void* handle;
    uint64_t offset; /* bytes */
    uint64_t size;   /* bytes available from offset */
} clay_device_buffer;

/* clay_eval_grid with the destination on the device. Same lattice, same cull
 * region rules, same x-fastest order, same float32 elements — only where the
 * results land differs, deliberately, so you can A/B the two and get
 * bit-identical values.
 *
 * out_colors_rgb may be NULL. Values stay float32 and are NOT quantized on the
 * device even though the brick cache stores fp16: quantization and band
 * classification belong to clay_brick_cache_submit, and a device path that did
 * them would be a second implementation of the step most able to drift. */
clay_result clay_eval_grid_device(const clay_document* doc, clay_device* device,
                                  const clay_grid_query* grid, const float region_min[3],
                                  const float region_max[3],
                                  const clay_device_buffer* out_values,
                                  const clay_device_buffer* out_colors_rgb);


/* -- the compiled tape ----------------------------------------------------- */

/* What `docs/06-host-gpu-previews.md` calls route 1: evaluate the document's
 * own field in your own shader, with OUR kernels rather than a copy of them.
 * The published kernel package (`dist/claycore-kernels/`, and in the
 * xcframework under `Headers/clay/kernel/`) gives you `ctape_eval`; this gives
 * you the three buffers to feed it.
 *
 *     ctape_eval(instrs, instr_count, params, blob, p)
 *
 * Without this a host drawing its own frames round-trips PIXELS through the
 * library every frame — computed in the engine, copied to host memory, then
 * uploaded to the GPU it was going to draw on. The tape is a few kilobytes and
 * changes once per edit; the pixels change 60 times a second.
 *
 * If you only want to draw the SURFACE, prefer the brick atlas below: it needs
 * no shader kernels at all, works in shading languages our dialect does not
 * target, and cannot drift for the same reason. Use the tape when you need the
 * field between the samples or away from the surface. */

/* An immutable SNAPSHOT of a compiled tape, which the caller releases.
 *
 * Editing the document CANNOT invalidate an export. The document holds its
 * compiled tape as a shared, const, revision-keyed object and installs a new
 * one on an edit rather than mutating the old, so an export is a reference to
 * the tape as it was — no invalidation callback, no revision to check before
 * dereferencing, no window in which a pointer you hold goes bad. That is why
 * the buffers below can be borrowed pointers rather than a copy into memory you
 * supplied: the lifetime rule is "an export is a snapshot", and the cost of the
 * export itself is a refcount. */
typedef struct clay_tape clay_tape; /* opaque */

/* Exactly kernel::CTapeInstr, asserted field by field with offsetof in
 * bindings/c/clay_c.cpp, because your evaluator is compiled from the header
 * that declares it. An array ELEMENT, not a versioned descriptor. */
typedef struct clay_tape_instr {
    uint32_t op;
    uint32_t param_offset;
} clay_tape_instr;

/* Exports the document's compiled tape. region_min/region_max are the same
 * optional CULL region clay_eval_grid takes, with the same rules: both NULL
 * compiles the whole document, one without the other is rejected, and an empty
 * or non-finite region is rejected. A host streaming a region wants the cull
 * the brick cache uses, so it is here rather than in a second entry point.
 *
 * COST DIFFERS between the two. With no region this is the document's cached
 * tape and the export is a refcount increment. WITH a region it COMPILES — a
 * culled tape is deliberately not cached, because consecutive bricks want
 * different regions and a cache keyed on the document alone would thrash. One
 * cull per brick per frame is an expensive thing to write by accident.
 *
 * Free with clay_tape_release. */
clay_result clay_tape_export(const clay_document* doc, const float region_min[3],
                             const float region_max[3], clay_tape** out_tape);
void clay_tape_release(clay_tape* tape);

/* The version of the tape ENCODING, which is the claycore version the kernel
 * package records in its VERSION file. The two are one number because they only
 * work together: your evaluator is `ctape_eval` from that package's headers, so
 * an opcode added on one side and absent on the other is a wrong answer rather
 * than a link error.
 *
 * The library cannot make this check — it does not know which package you
 * compiled — so publishing the number is its half and comparing it is yours.
 * On a mismatch, REFUSE. Do not reinterpret. */
uint32_t clay_tape_encoding_version(void);

/* Borrowed, valid until clay_tape_release, and unaffected by any edit. Each
 * writes its element count to *out_count, which may not be NULL. A tape with
 * no out-of-line payload has a blob count of 0 and may return NULL for it. */
const clay_tape_instr* clay_tape_instrs(const clay_tape* tape, size_t* out_count);
const float* clay_tape_params(const clay_tape* tape, size_t* out_count);
const float* clay_tape_blob(const clay_tape* tape, size_t* out_count);

/* What the three buffers cannot tell an evaluator, and what a host that
 * guesses gets wrong. Every out pointer is optional.
 *
 * out_safe_step_scale is what a sphere tracer multiplies its step by. It is
 * 1 / max(lipschitz, 1) and `csafe_step_scale` in the published headers
 * computes it — it is returned anyway because "a host that guesses its step
 * scale draws a wrong frame" and a host recomputing a one-line formula is a
 * host that can get it wrong. Four bytes to remove the question.
 *
 * out_bounds_* is the union of item influence bounds: what to clip against. A
 * host that guesses these draws a slow frame instead of a wrong one.
 *
 * out_revision is the document revision the tape was compiled at, so telling
 * whether the copy you uploaded is still current is an integer comparison
 * rather than a comparison of buffers. It is opaque and only equality is
 * meaningful.
 *
 * NOTE on the blob: sampled volumes ride in it, so a document carrying one has
 * a blob that is megabytes rather than kilobytes, and this export publishes the
 * whole tape with no delta encoding. Compare out_revision AND the blob count
 * between edits: a stroke that touches no volume leaves the blob alone and you
 * can skip the re-upload. A finer answer needs to know WHICH region of a
 * sampled volume an edit changed, which is a property of the edit rather than
 * of the tape, and belongs with add-multi-resolution and
 * add-consolidation-policy rather than here. */
clay_result clay_tape_info(const clay_tape* tape, int32_t* out_is_exact, float* out_lipschitz,
                           float* out_safe_step_scale, float out_bounds_min[3],
                           float out_bounds_max[3], uint64_t* out_revision);

/* -- the brick cache ------------------------------------------------------- */

/* What makes sculpting INCREMENTAL. The field is kept as a sparse grid of
 * dim^3 fp16 bricks in a narrow band around the surface; an edit dirties the
 * bricks its influence bound reaches and only those are re-evaluated. Every
 * other brick is left BIT-IDENTICAL, which is the property the whole design
 * exists to provide.
 *
 * The cache evaluates nothing and owns no thread. It hands out plain-data
 * REQUESTS — a brick key, a generation, and the lattice to sample — and takes
 * the resulting distances back. Which backend runs them, on which thread, in
 * what order, and how many per frame is the host's business. There is exactly
 * one path:
 *
 *     mark_dirty -> take_dirty -> eval_requests -> submit
 *
 * The generation is what makes an in-flight request safe. Re-dirtying a brick
 * bumps its generation, so a result computed against the older scene is
 * REJECTED at submit rather than overwriting newer state. That is an ordinary
 * outcome of an interactive session, not a failure, so it arrives through
 * out_results and the call still returns CLAY_OK — the same choice
 * clay_document_undo makes for "nothing to undo".
 *
 * THREADING. A cache handle takes no lock and adds none: the C++ class is a
 * hash map and a vector with no synchronization, and a lock here would be a
 * threading policy the consumer did not ask for. So every call on ONE handle
 * must be serialized by the host, const readers included — a concurrent
 * submit may be rehashing the map a reader is walking. Two handles share
 * nothing. clay_brick_cache_eval_requests takes no cache at all and is
 * free-threaded: it may run on any number of threads against one const
 * document, which is safe for the same reason clay_eval_points is, and is NOT
 * safe concurrently with a mutating clay_document_* / clay_layer_* call.
 *
 * The cache knows nothing about a document, and no edit invalidates anything
 * in it. A host that edits and does not mark the edit's influence bound dirty
 * keeps stale bricks, and nothing here can detect that: prefer
 * clay_brick_cache_mark_dirty_nodes, which computes the bound itself. */

typedef struct clay_brick_cache clay_brick_cache; /* opaque */

/* What a brick holds. Inside and Outside are implicit — the state IS the
 * data, no lattice is allocated — which is why empty space costs nothing. */
typedef enum clay_brick_state {
    CLAY_BRICK_INSIDE = 0,  /* uniformly d <= -band */
    CLAY_BRICK_OUTSIDE = 1, /* uniformly d >= +band */
    CLAY_BRICK_SURFACE = 2, /* the band crosses it: dim^3 fp16 samples */
    /* This boundary's own value, with no engine counterpart (as clay_mesher
     * has none): the cache holds nothing for the key — it was never marked, or
     * was marked and never submitted. clay_brick_cache_read_bricks reports it
     * and leaves that brick's slice of the output untouched. */
    CLAY_BRICK_MISSING = 3
} clay_brick_state;

/* What became of a submission. Only ACCEPTED changed the cache. */
typedef enum clay_brick_submit {
    CLAY_BRICK_SUBMIT_ACCEPTED = 0,
    /* The brick was re-dirtied while this request was in flight, or the cache
     * no longer tracks it. Expected, and not an error: drop the values and
     * wait for the request the next clay_brick_cache_take_dirty hands you. */
    CLAY_BRICK_SUBMIT_STALE = 1,
    /* Storing this brick would put the cache over its memory budget. Every
     * brick already stored stays valid and sampleable, and the ceiling is
     * never breached; the brick simply stays unevaluated. */
    CLAY_BRICK_SUBMIT_BUDGET_EXCEEDED = 2
} clay_brick_submit;

/* Every field belongs to the original layout and none has a default: a
 * struct_size shorter than this is rejected outright, as it is for
 * clay_brush_params. Zero is not a resolution and not a voxel size, so a
 * zeroed descriptor is a mistake rather than a request — start from
 * clay_brick_config_defaults and override what you care about. */
typedef struct clay_brick_config {
    uint32_t struct_size; /* = sizeof(clay_brick_config); required */
    int32_t dim;          /* lattice samples per brick axis; 8 or 16 */
    float voxel_size;     /* world units between lattice samples; finite and > 0 */
    int32_t band_voxels;  /* half-width of the kept band, in voxels; > 0 */
    /* Bytes of SURFACE-brick payload the cache may hold; 0 is unlimited,
     * exactly as brick::BrickConfig has it. It bounds the fp16 lattices and
     * nothing else: the per-brick bookkeeping grows with how much space has
     * ever been marked dirty, which clay_brick_stats.tracked_bricks reports
     * and this does not cover. */
    uint64_t memory_budget;
    /* 0/1: carry an RGBA8 colour lattice beside the distances, so a host can
     * upload a colour atlas without meshing. Appended after the original
     * layout, so a caller compiled against the older struct keeps the
     * distance-only cache it already had.
     *
     * Chosen HERE and not per call, because a colour lattice has to be
     * evaluated to exist: a per-call flag would let a host ask a distance-only
     * cache for colours it never computed, and the only truthful answer would
     * be an error for a mistake made three calls earlier. When this is set,
     * clay_brick_cache_submit REQUIRES colours and clay_brick_cache_read_bricks
     * can return them; when it is not, both refuse to deal in colour at all.
     *
     * It costs two more bytes per sample than the fp16 distance, inside the
     * SAME memory_budget — so a colour cache holds about a third of the bricks
     * a distance-only one holds at the same ceiling. That is why it is opt-in
     * rather than the default. */
    int32_t colors;
} clay_brick_config;

/* Fills a descriptor with the engine's defaults, struct_size included: 8^3
 * bricks, 0.05 world units per voxel, a 3-voxel band, no budget, no colour. */
clay_result clay_brick_config_defaults(clay_brick_config* out_config);

/* Everything a host needs to draw a progress bar or decide whether to raise
 * the budget, in one versioned descriptor so a counter can be appended later
 * without a new entry point. Reading it walks the tracked bricks to count the
 * surface ones, so it is O(tracked) rather than free — once a frame, not once
 * a brick. */
typedef struct clay_brick_stats {
    uint32_t struct_size;    /* = sizeof(clay_brick_stats); required */
    uint64_t tracked_bricks; /* keys the cache holds bookkeeping for */
    uint64_t surface_bricks; /* of those, the ones storing an fp16 lattice */
    uint64_t dirty_bricks;   /* waiting to be handed out, plus any drained but
                              * not yet taken by clay_brick_cache_take_dirty */
    uint64_t memory_usage;   /* bytes of surface-brick payload held */
    uint64_t memory_budget;  /* what the cache was created with; 0 = unlimited */
} clay_brick_stats;

/* One evaluation request, and the only thing that crosses between the cache
 * and the host's evaluator.
 *
 * An array ELEMENT, not a versioned descriptor: it carries no struct_size for
 * the same reason clay_stamp does not — a caller receives thousands at once,
 * its layout is fixed, and changing that layout is a break rather than
 * something to negotiate. The layout is byte-for-byte brick::BrickRequest,
 * asserted with offsetof in bindings/c/clay_c.cpp, so a drain is one memcpy
 * and no field is transcribed.
 *
 * origin/spacing/dims/band are derivable from the key and the config, and are
 * carried anyway so that the host and the library cannot disagree about them.
 * band matters as much as the lattice does: the tape must be culled against the
 * brick DILATED by the band, because a sample keeps its true distance whenever
 * that distance is within the band, so an item a band outside the brick still
 * decides samples inside it. Culling on the bare brick drops it and the brick
 * is then classified empty instead of carrying the surface's approach.
 * Pass the request back to clay_brick_cache_submit UNMODIFIED. */
typedef struct clay_brick_request {
    int32_t key[3];      /* brick coordinate; brick (x,y,z) starts at key * dim * voxel_size */
    uint32_t generation; /* what submit checks; opaque to the host */
    float origin[3];     /* world position of lattice sample (0, 0, 0) */
    float spacing;       /* = config.voxel_size */
    int32_t dims[3];     /* = config.dim on every axis */
    float band;          /* = config.band_voxels * config.voxel_size */
} clay_brick_request;

/* A cache the caller owns. Returns NULL on invalid configuration, with the
 * detail in clay_last_error(). Free with clay_brick_cache_destroy. There is
 * no borrowed form: a cache belongs to whoever made it, never to a document,
 * so this takes no document and destroy returns void as
 * clay_document_destroy does rather than the clay_result
 * clay_voxel_grid_destroy needs to refuse a borrow.
 *
 * Destroying a cache invalidates every clay_brick_request still in flight, in
 * the sense that submitting one afterwards is a use-after-free this ABI
 * cannot detect; a request is otherwise a self-contained value. */
clay_brick_cache* clay_brick_cache_create(const clay_brick_config* config);
void clay_brick_cache_destroy(clay_brick_cache* cache);

/* The configuration the cache was created with, and its counters. Both
 * descriptors are OUTPUTS, so struct_size is the caller declaring how much of
 * the struct exists; the library writes only that prefix. */
clay_result clay_brick_cache_config(const clay_brick_cache* cache,
                                    clay_brick_config* out_config);
clay_result clay_brick_cache_stats(const clay_brick_cache* cache, clay_brick_stats* out_stats);

/* Marks every brick whose band-dilated volume meets the region, tracking
 * bricks it has not seen before. Pass an edit's INFLUENCE bound, not its
 * geometry bound — clay_layer_node_influence_bound is where one comes from,
 * and clay_brick_cache_mark_dirty_nodes does both steps for you.
 *
 * region_min and region_max BOTH NULL dirty everything the cache tracks —
 * what a host does after an edit whose influence is infinite, and the only
 * way to say it: an explicit region carrying an infinity is rejected, because
 * a region derived from a camera frustum or a degenerate selection box
 * arrives that way by accident rather than on purpose. Passing one without
 * the other is rejected, as it is on clay_voxel_rasterize.
 *
 * A region is also refused when it spans more than CLAY_MAX_BATCH bricks, or
 * when a brick coordinate in it would not fit in an int32_t. Three floats
 * name that many bricks easily — a region of 1e6 world units at a brick size
 * of 0.4 is 1e19 of them — and each one costs a tracked entry, so the span is
 * computed in 64-bit and checked BEFORE anything is inserted. A refused call
 * leaves the cache exactly as it was. */
clay_result clay_brick_cache_mark_dirty(clay_brick_cache* cache, const float region_min[3],
                                        const float region_max[3]);

/* The same, with the bound computed from the document — the documented
 * default, because the region is the single most likely thing to get silently
 * wrong and a bound that is too tight leaves visibly stale bricks at a blend
 * seam. `nodes` is count node ids; one the layer does not hold, or a hidden
 * one, contributes nothing. A node whose influence is infinite dirties
 * everything the cache tracks.
 *
 * *out_marked (may be NULL) receives how many ids contributed a bound. Every
 * bound is computed and span-checked before any of them is marked, so a
 * refusal leaves the cache untouched rather than half-dirtied. */
clay_result clay_brick_cache_mark_dirty_nodes(clay_brick_cache* cache,
                                              const clay_document* doc, clay_layer_id layer,
                                              const clay_node_id* nodes, size_t count,
                                              size_t* out_marked);
/* The union over a layer — what a first, full fill marks. A layer that shows
 * nothing marks nothing, which is not an error. */
clay_result clay_brick_cache_mark_dirty_layer(clay_brick_cache* cache,
                                              const clay_document* doc, clay_layer_id layer);

/* Drains dirty bricks into requests, bumping each brick's generation.
 *
 * Capacity in, count out — the clay_layer_apply_stroke shape, not a size
 * query: *count is the caller's buffer capacity in REQUESTS going in and the
 * number written coming out, and *out_remaining (may be NULL) is how many are
 * still queued afterwards. out_requests == NULL is CLAY_ERROR_INVALID_ARGUMENT
 * and never a size query, so there is no BUFFER_TOO_SMALL retry loop. Call it
 * in a loop until *out_remaining is 0, or bound the work per frame by stopping
 * early — the rest stay queued.
 *
 * Your capacity bounds what is COPIED OUT, not what the library allocates. The
 * engine's drain is all-or-nothing, so the first call after a large mark stages
 * every queued request inside the library — asking for one request after a
 * million-brick mark still stages the million, at sizeof(clay_brick_request)
 * each, and the staging is held until the queue is fully drained. Size the
 * regions you mark accordingly; a frame budget on this call does not bound the
 * memory. Removing that cliff needs a capacity-aware drain in the engine.
 *
 * A request is a VALUE and stays submittable until its brick is dirtied
 * again, at which point submitting it is merely STALE. It may be copied,
 * queued, sent to any number of worker threads and held across frames; it
 * contains no pointer. */
clay_result clay_brick_cache_take_dirty(clay_brick_cache* cache,
                                        clay_brick_request* out_requests, size_t* count,
                                        size_t* out_remaining);

/* Evaluates `count` requests against a document and nothing else: for each
 * one it derives the cull region from the request's own lattice, compiles the
 * per-brick culled tape and runs it through `backend` (NULL = "cpu"). Brick i
 * fills out_values[i * dim^3 ...] whatever order the work is done in, so
 * values_capacity must be exactly count * dim^3 floats.
 *
 * Each request is culled against its own brick DILATED by its own band, which
 * is what clay_brick_cache_cull_region returns. The band rides on the request
 * rather than being passed in, so there is no way to supply the wrong one: a
 * band of zero would silently drop items that decide samples inside the brick
 * and classify it empty, and a value a caller has to remember to thread through
 * is a value that will one day arrive as zero.
 *
 * It takes NO cache handle: it does not submit, does not touch a cache and
 * starts no thread, so it is free-threaded and any number of threads may run
 * it against one const document at once. The host still calls
 * clay_brick_cache_submit, and still decides how many requests to run and
 * where. Fan out over REQUESTS, one brick per worker: the CPU backend already
 * splits a single grid's z axis over a process-wide pool, and a brick is 8
 * slices of 64 samples, so a per-brick call is already a small dispatch. That
 * is measured, not reasoned: on an M2 Max it takes a 216-brick fill from
 * 24.7 ms to 8.2 ms on twelve workers.
 *
 * out_colors_rgb (may be NULL) receives the field's colour at the same lattice
 * points, count * dim^3 * 3 floats with brick i at i * dim^3 * 3, and
 * colors_capacity must be exactly that or 0 when it is NULL. It is what a
 * colour-carrying cache's clay_brick_cache_submit wants; a distance-only cache
 * needs neither and passing NULL costs nothing to compute.
 *
 * ROUTE BY BATCH SIZE. The whole batch reaches the backend as batched
 * evaluations, so a GPU backend runs it as a single device submission rather
 * than one round trip per brick — but a submission still has a fixed cost
 * (~0.25 ms on an M-series Mac) that count * dim^3 samples must cover.
 * Measured there at dim 8: below ~16 bricks "cpu" wins (a single brick is
 * 0.01 ms on the CPU against the submission's 0.3 ms); at a dab's 27 bricks
 * "metal" is ~2x ahead; at 4096+ bricks it is 30x and up. So route small
 * residual batches to "cpu" and anything from a dab upward to "metal", or
 * simply pass "metal" whenever a batch holds a dab's worth of bricks. Both
 * backends produce the same field; this is speed, not results. Re-measure
 * before hardcoding the threshold on a device that is not an M-series Mac. */
clay_result clay_brick_cache_eval_requests(const clay_document* doc, const char* backend,
                                           const clay_brick_request* requests, size_t count,
                                           float* out_values, size_t values_capacity,
                                           float* out_colors_rgb, size_t colors_capacity);

/* clay_brick_cache_eval_requests with the destination on the device — the call
 * a host refilling a brick atlas actually wants. Brick i occupies
 * out_values[i * dim^3 ...] floats and out_colors_rgb[i * dim^3 * 3 ...] at the
 * same fixed stride the host-memory form uses, in ONE of your buffers, so a
 * whole drain lands in the allocation you will draw from.
 *
 * Each request is culled against its own brick dilated by its own band, exactly
 * as the host-memory form does, and the same values come out. What differs is
 * only where they land — and what you then owe: these are float32 distances,
 * not the cache's classified, band-clamped fp16 bricks. Nothing was submitted
 * to a cache and no brick state was decided. If you want that, submit them and
 * read them back; this call is for hosts that would rather do the conversion
 * themselves than pay the copy.
 *
 * BATCHED like the host-memory form: the whole batch reaches the adopted
 * backend as batched device evaluations — one device submission per chunk of
 * requests, not one per brick — so a drain costs about what the host-memory
 * "metal" route costs, without the copy back. The per-submission floor
 * (~0.25 ms on an M-series Mac) still applies per CALL, so route by batch
 * size exactly as clay_brick_cache_eval_requests documents. */
clay_result clay_brick_cache_eval_requests_device(const clay_document* doc, clay_device* device,
                                                  const clay_brick_request* requests,
                                                  size_t count,
                                                  const clay_device_buffer* out_values,
                                                  const clay_device_buffer* out_colors_rgb);

/* Submits the evaluated distances for `count` requests: each brick's values
 * are classified (inside / outside / surface), clamped to the band and
 * quantized to fp16. `values` is count * dim^3 floats in the grids' own
 * order, x fastest, brick i at i * dim^3, and values_capacity must equal that
 * product exactly. count == 1 is the per-worker case and costs nothing.
 *
 * out_results (count clay_brick_submit values) and out_accepted (how many were
 * ACCEPTED) are both optional, but not both at once: a caller that skips both
 * cannot tell whether anything landed. The call returns CLAY_OK for all three
 * outcomes — a stale result is the generation counter doing its job, and a
 * budget refusal is the ceiling doing its job. The return code is reserved for
 * a malformed call: a null pointer, a values_capacity that is not exactly
 * count * dim^3, or a request whose spacing or dims do not match the cache's
 * configuration, which means it was modified or came from a different cache.
 *
 * `colors_rgb` is count * dim^3 * 3 floats in the same order, and it is
 * REQUIRED when the cache was created with clay_brick_config.colors and
 * REFUSED when it was not — there is no cache that takes colour optionally,
 * because a brick with a colour lattice and a brick without one are not the
 * same brick to read back. colors_capacity is checked exactly, or must be 0
 * when colors_rgb is NULL. Classification is decided by the DISTANCES alone:
 * colour never makes a brick a surface, and a uniform brick keeps one colour
 * rather than a lattice.
 *
 * The values are not scanned for NaN, and a request's origin is not
 * re-derived from its key. A backend that produces a NaN classifies the brick
 * as surface and stores a NaN half; validating dim^3 floats per brick on the
 * refill path would cost a pass over every brick in the scene to catch a
 * backend bug, which is the cost this cache exists to remove. */
clay_result clay_brick_cache_submit(clay_brick_cache* cache,
                                    const clay_brick_request* requests, size_t count,
                                    const float* values, size_t values_capacity,
                                    const float* colors_rgb, size_t colors_capacity,
                                    int32_t* out_results, size_t* out_accepted);

/* The world box a brick's lattice covers, and the box its evaluation should be
 * CULLED against — the brick dilated by the band. Hand the second one to
 * clay_eval_grid as the cull region when driving evaluation yourself;
 * clay_brick_cache_eval_requests does it for you. Any key answers, tracked or
 * not: these are arithmetic on the configuration. */
clay_result clay_brick_cache_brick_bounds(const clay_brick_cache* cache, const int32_t key[3],
                                          float out_min[3], float out_max[3]);
clay_result clay_brick_cache_cull_region(const clay_brick_cache* cache, const int32_t key[3],
                                         float out_min[3], float out_max[3]);

/* One decoded lattice sample, band-clamped: -band inside, +band outside, and
 * +band for a brick the cache has not evaluated. i, j, k are in [0, dim). The
 * direct mirror of the C++ sample(); read a whole brick with
 * clay_brick_cache_read_bricks. */
clay_result clay_brick_cache_sample(const clay_brick_cache* cache, const int32_t key[3],
                                    int32_t i, int32_t j, int32_t k, float* out_value);

/* Reads whole bricks in their STORED form — IEEE binary16 values per key, the
 * engine's own bits unconverted, which is what a GPU texture upload wants and
 * half the bytes of the decoded form. `keys_xyz` is count packed int32 triples,
 * exactly as clay_voxel_flood_select returns cells.
 *
 * The stride is FIXED at (dim + 2*apron)^3 — call it W below: brick i occupies
 * out_halves[i * W ...] whatever its state, so out_halves can be an MTLBuffer's
 * contents and the upload is one memcpy with no packing pass. values_capacity
 * must be exactly count * W, or 0 when out_halves is NULL and only the states
 * are wanted.
 *
 * `apron` is the halo, in voxels, taken from the NEIGHBOURING bricks and
 * written around each brick's own lattice. 0 is the brick alone and what this
 * call did before the apron existed. Hardware trilinear filtering across a
 * brick boundary needs a one-voxel halo, and without one a host either fetches
 * neighbours in the shader at every brick edge or repacks tiles on the CPU —
 * which throws away the one-memcpy property this call is for. The halo is
 * defined for EVERY neighbour: an implicit or never-evaluated brick answers the
 * same band value a single sample of it reports, so no element is undefined and
 * a tile at the edge of the sculpted region filters against the band rather
 * than against whatever was in the buffer. apron is bounded by dim and a wider
 * one is rejected rather than clamped — past that the tile is mostly neighbour
 * and what the host wants is a coarser lod, not a fatter apron.
 *
 * out_states (count clay_brick_state values) says what each key held — the
 * KEY's own state, never the halo's. It may be NULL when only the payload is
 * wanted, but not together with out_halves and out_colors_rgba: a call that
 * writes nothing has nothing to report. A uniform brick is FILLED with the band
 * value of its sign, so an uploader never branches; CLAY_BRICK_MISSING means
 * the cache holds nothing for that key and its whole W elements are LEFT
 * UNTOUCHED — the one state where the buffer is not written, and the rule is
 * about the key rather than its neighbourhood.
 *
 * out_colors_rgba (may be NULL) receives count * W * 4 bytes of RGBA8 in the
 * same padded layout — directly uploadable as rgba8unorm, which WebGPU filters
 * as it filters r16float. Alpha is 255 and RESERVED: it is not a mask and not
 * coverage. It requires a cache created with clay_brick_config.colors and is
 * refused otherwise, and colors_capacity must be exactly count * W * 4 or 0
 * when it is NULL.
 *
 * `lod` is 0 for the full-resolution brick or 1 for its mip. A value above 1
 * is rejected rather than clamped: there is one mip level, and silently
 * answering level 0 for a request for level 4 would put a brick twice the
 * intended size on screen. A mip carries NO colour — it subsamples distances,
 * and averaging colour across its 2x2x2 block would be a filtering policy
 * chosen on your behalf — so colours at lod 1 are refused, not approximated. */
clay_result clay_brick_cache_read_bricks(const clay_brick_cache* cache, int32_t lod,
                                         const int32_t* keys_xyz, size_t count, int32_t apron,
                                         int32_t* out_states, uint16_t* out_halves,
                                         size_t values_capacity, uint8_t* out_colors_rgba,
                                         size_t colors_capacity);

/* Every brick that stores samples, through the size-query pattern, as packed
 * int32 triples exactly like clay_voxel_flood_select: call with out_keys_xyz
 * == NULL for the count in *count, then again with a buffer of count*3
 * values. This is the ONE size query in this section — the drain is not, and
 * refuses a NULL buffer. The order is the cache's own and is not stable
 * across calls that mutate it. */
clay_result clay_brick_cache_surface_bricks(const clay_brick_cache* cache,
                                            int32_t* out_keys_xyz, size_t* count);

/* -- LOD mips -------------------------------------------------------------- */

/* A coarse brick covering 2x2x2 full-resolution bricks, subsampled from them
 * rather than evaluated: same lattice size, twice the spacing. Read it with
 * clay_brick_cache_read_bricks at lod 1.
 *
 * Buildable only when all eight children are evaluated AND clean. *out_built
 * is 0 when they are not — an ordinary "not yet", reported like every other
 * expected negative here rather than as a failure. Dirtying any child drops
 * the mip, so a mip is never downsampled from stale data. */
clay_result clay_brick_cache_build_mip(clay_brick_cache* cache, const int32_t coarse_key[3],
                                       int32_t* out_built);
/* 1 when a valid mip exists for the coarse key, 0 when the region is only
 * available at full resolution — the cheap way to ask before reading. */
clay_result clay_brick_cache_current_lod(const clay_brick_cache* cache,
                                         const int32_t coarse_key[3], int32_t* out_lod);

/* -- what the bricks are for ----------------------------------------------- */

/* How a brick mesh is built. Only the fields mesh::MeshingOptions actually
 * has: this is NOT clay_mesh_params, whose voxel_size, resolution and mesher
 * mean nothing here — the lattice is the cache's and the mesher is the
 * marching one, both already decided. */
typedef enum clay_normal_mode {
    CLAY_NORMAL_NONE = 0,     /* no normals */
    CLAY_NORMAL_FACE = 1,     /* area-weighted face normals; needs no document */
    /* The field gradient, so blends read smooth. Evaluated through PER-BRICK
     * culled tapes — the same culling the refill path uses — so its cost
     * follows the bricks being meshed, not the size of the document: re-meshing
     * a fixed dab's worth of bricks costs the same on a fresh document and on
     * one carrying two hundred earlier strokes (issue #73). Inside a brick's
     * band-dilated cull region the culled tape is band-clamp identical to the
     * whole document's, and mesh vertices sit on the surface with gradient
     * taps of gradient_eps << band, so the normals equal a full-tape
     * evaluation exactly. */
    CLAY_NORMAL_GRADIENT = 2
} clay_normal_mode;

typedef struct clay_brick_mesh_params {
    uint32_t struct_size; /* = sizeof(clay_brick_mesh_params); required */
    int32_t normals;      /* clay_normal_mode */
    int32_t colors;       /* 0/1: sample the document's colour field per vertex */
    float gradient_eps;   /* tetrahedron-tap half-width; <= 0 means the default */
} clay_brick_mesh_params;

/* What one brick key contributed to a mesh. An array ELEMENT, not a versioned
 * descriptor: a caller receives one per key and thousands at a time, its layout
 * is fixed, and changing that layout is a break rather than something to
 * negotiate — the same reasoning clay_brick_request carries.
 *
 * The ranges are contiguous and together PARTITION the mesh, so a host can
 * write a key's slice into a sub-range of a GPU buffer instead of rebuilding
 * it. But vertices are welded on canonical lattice-edge keys and that welding
 * spans brick SEAMS, so a triangle in one key's index range may reference a
 * vertex in an EARLIER key's vertex range — whichever key reached a shared seam
 * vertex first owns it. You may OVERWRITE a key's ranges; you may not free one
 * key's vertices without checking its neighbours'. Breaking the weld to make
 * the ranges independent would produce a seam-duplicated mesh, and this is also
 * the export path, where watertightness is the contract.
 *
 * A key's ranges carry its own cells' triangles first, then the straddlers
 * attributed to it — see clay_brick_cache_mesh for the attribution rule. */
typedef struct clay_brick_mesh_range {
    int32_t key[3];
    uint32_t vertex_first, vertex_count;
    uint32_t index_first, index_count;
} clay_brick_mesh_range;

/* Meshes the cache's surface bricks — marching only the cells those bricks
 * own, which is the point: a re-mesh costs what the SURFACE covers, not what
 * the scene's bounding box does.
 *
 * `keys_xyz` (count packed int32 triples, exactly as
 * clay_brick_cache_surface_bricks returns them) names the bricks to march, and
 * NULL with a key_count of 0 means every surface brick — what this call did
 * before the key list existed, and what an export wants. Hand it what
 * clay_brick_cache_take_dirty reported and a re-mesh costs the dab rather than
 * the model. A key that stores no lattice contributes nothing and is NOT an
 * error: a drained dirty set routinely contains bricks that turned out uniform.
 *
 * Marching a subset samples ACROSS the subset's boundary exactly as the whole
 * does, so the triangles produced for a cell are identical either way. A subset
 * differs only in that a vertex shared with a cell outside it is emitted again
 * by whichever mesh reaches it, at a bit-identical position — a duplicated seam
 * vertex, never a crack.
 *
 * A subset returns every whole-mesh triangle with at least one corner inside
 * a requested brick's closed box — including the STRADDLERS, whose cell is
 * owned by an unrequested brick. Each straddler is attributed to the
 * lexicographically lowest (x, then y, then z) requested key whose closed box
 * contains one of its corners, and lands in that key's ranges after the key's
 * own triangles. This is what makes a subset able to MAINTAIN a surface:
 * without the straddlers those triangles lived in cells no request named, so
 * no sequence of subset calls could reconstruct the whole, and dilating the
 * request only moved the boundary. The cost is that a host holding geometry
 * per brick dedupes by triangle: a straddler touching two requested keys is
 * attributed to one of them per call, and may move to another key's share
 * when a later request names a different set — its content is identical
 * wherever it lands, so keeping either copy is right.
 *
 * out_ranges (may be NULL) receives key_count clay_brick_mesh_range values in
 * the order the keys were given. It REQUIRES keys_xyz: with no key list there
 * is no count for the caller to have sized this buffer from, and inferring one
 * from the cache's current surface set is the kind of length this ABI refuses
 * to infer anywhere else. Wanting ranges means wanting to patch a buffer, which
 * means having a key list — so the combination that is refused is not one a
 * host has a use for.
 *
 * `doc` may be NULL, and then no tape is compiled: the mesh has positions and
 * face normals and no colours. Gradient normals and colours are attributes of
 * the FIELD, so asking for either without a document is rejected rather than
 * quietly downgraded. When a document IS passed, those attributes are
 * evaluated through per-brick CULLED tapes rather than the whole document's,
 * so their cost follows the bricks named — see CLAY_NORMAL_GRADIENT. Behind the same owner handle as clay_document_mesh and
 * freed with clay_mesh_destroy. A cache holding no surface bricks yields an
 * EMPTY mesh rather than an error, as clay_voxel_mesh does for an empty grid:
 * a cache that has not been filled yet is an ordinary state of a session. */
clay_result clay_brick_cache_mesh(const clay_brick_cache* cache, const clay_document* doc,
                                  const clay_brick_mesh_params* params,
                                  const int32_t* keys_xyz, size_t key_count,
                                  clay_brick_mesh_range* out_ranges, clay_mesh** out_mesh);

/* Raycast the cached bricks: trilinear samples inside surface bricks, a brick
 * DDA across the rest — so the cost is the ray's path through the band rather
 * than a march against the whole document's tape. Position and normal only;
 * clay_raycast_attributed is the call that names what was hit.
 *
 * The direction is normalized here, and a zero-length one is rejected. A ray
 * that hits nothing sets *out_hit to 0 and is not an error; every out pointer
 * but out_hit may be NULL. */
clay_result clay_brick_cache_raycast(const clay_brick_cache* cache, const float origin[3],
                                     const float dir[3], int32_t* out_hit, float* out_t,
                                     float out_position[3], float out_normal[3]);

/* The same, for many rays: rays_origin_dir is count packed six-float rays and
 * the outputs are the ones clay_raycast_many takes, in the same shapes and each
 * one optional. It is the batched form of the call above and nothing more —
 * one call shape for the cache and for the document is the whole value of it.
 *
 * The batch fans out across the engine's shared worker pool — the same one the
 * CPU backend evaluates points with — and returns only when every ray is done,
 * so no engine thread touches the cache after the call returns. The cache still
 * owns no thread, and serializing mutations against this call is still the
 * host's job. Each slot holds byte-for-byte what the single-ray call above
 * reports for the same ray, in order. */
clay_result clay_brick_cache_raycast_many(const clay_brick_cache* cache,
                                          const float* rays_origin_dir, size_t count,
                                          int32_t* out_hits, float* out_t,
                                          float* out_positions_xyz, float* out_normals_xyz);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLAY_H */
