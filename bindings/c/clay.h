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
#define CLAY_ABI_MINOR 2
#define CLAY_ABI_PATCH 0

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
 * break on a minor bump, and 0.2.0 did break (see below). */
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
    CLAY_PRIM_LNORM_SPHERE = 30       /* params: r n, n >= 2 (bound) */
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
    CLAY_OP_TRANSITION_RADIAL = 13  /* morph over an XZ radius */
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
 *   DISPLACE  amplitude frequency   sine displacement of the field */
typedef enum clay_deform {
    CLAY_DEFORM_TWIST = 0,
    CLAY_DEFORM_BEND = 1,
    CLAY_DEFORM_TAPER = 2,
    CLAY_DEFORM_DISPLACE = 3
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

/* Point chain of a CLAY_PRIM_STROKE item: count points of x, y, z, radius,
 * replacing any previous chain. One point is a sphere and none contributes
 * nothing, as in the Python bindings. blend_k (>= 0) smooths consecutive
 * segments of the chain itself. */
clay_result clay_item_set_stroke_points(clay_item* item, const float* xyzr, size_t count);
clay_result clay_item_add_stroke_point(clay_item* item, const float position[3], float radius);
clay_result clay_item_set_stroke_blend_k(clay_item* item, float k);

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

/* Single raycast (origin + normalized direction). *out_hit is 0/1. */
clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]);

/* -- meshing (owner-handle pattern) ---------------------------------------- */

typedef struct clay_mesh clay_mesh; /* opaque */

typedef struct clay_mesh_params {
    uint32_t struct_size;   /* = sizeof(clay_mesh_params); required, see above */
    float voxel_size;       /* world units per cell; <= 0 picks from resolution */
    int32_t resolution;     /* used when voxel_size <= 0: cells across the largest extent */
    int32_t decimate;       /* 0/1 */
    float decimate_ratio;   /* target triangle ratio when decimate != 0 */
} clay_mesh_params;

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLAY_H */
