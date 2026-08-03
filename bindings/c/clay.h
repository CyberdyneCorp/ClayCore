/* clay.h — the flat, stable, versioned C ABI of claycore (c-abi spec).
 *
 * Rules of the boundary:
 *  - opaque handles, integer error codes, caller-owned buffers
 *  - buffers are either caller-allocated via the size-query pattern or
 *    returned behind an owner handle with an explicit clay_*_destroy
 *  - no C++ types, no exceptions, fixed-size integer types, UTF-8 strings
 *  - no variadics, no bitfields (FFI-general: Swift, C#, Rust, ctypes)
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
#define CLAY_ABI_MINOR 1
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

/* Library/ABI version (compare majors at init; see c-abi spec). */
void clay_version(int32_t* major, int32_t* minor, int32_t* patch);

/* Thread-local detail message for the last failing call on this thread. */
const char* clay_last_error(void);

/* -- enums (values are ABI-stable) ---------------------------------------- */

typedef enum clay_prim {
    CLAY_PRIM_SPHERE = 0,           /* params: r */
    CLAY_PRIM_BOX = 1,              /* params: bx by bz */
    CLAY_PRIM_ROUND_BOX = 2,        /* params: bx by bz r */
    CLAY_PRIM_BOX_FRAME = 3,        /* params: bx by bz e */
    CLAY_PRIM_TORUS = 4,            /* params: R r */
    CLAY_PRIM_CAPSULE = 5,          /* params: ax ay az bx by bz r */
    CLAY_PRIM_CAPPED_CYLINDER = 6,  /* params: r h */
    CLAY_PRIM_ROUNDED_CYLINDER = 7, /* params: ra rb h */
    CLAY_PRIM_CAPPED_CONE = 8,      /* params: h r1 r2 */
    CLAY_PRIM_ROUND_CONE = 9,       /* params: r1 r2 h */
    CLAY_PRIM_ELLIPSOID = 10,       /* params: rx ry rz (bound) */
    CLAY_PRIM_OCTAHEDRON = 11,      /* params: s */
    CLAY_PRIM_HEX_PRISM = 12,       /* params: hx hy */
    CLAY_PRIM_PYRAMID = 13          /* params: h */
} clay_prim;

typedef enum clay_op {
    CLAY_OP_ADD = 0,
    CLAY_OP_SUBTRACT = 1,
    CLAY_OP_INTERSECT = 2,
    CLAY_OP_PAINT = 3
} clay_op;

typedef enum clay_blend {
    CLAY_BLEND_HARD = 0,
    CLAY_BLEND_QUADRATIC = 1,
    CLAY_BLEND_CUBIC = 2,
    CLAY_BLEND_CIRCULAR = 3,
    CLAY_BLEND_CHAMFER = 4
} clay_blend;

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

clay_result clay_add_item(clay_document* doc, clay_layer_id layer,
                          const clay_item_desc* item, clay_node_id* out_node);
clay_result clay_remove_node(clay_document* doc, clay_layer_id layer, clay_node_id node);
clay_result clay_set_layer_mirror(clay_document* doc, clay_layer_id layer, int32_t axis_x,
                                  int32_t axis_y, int32_t axis_z, float mirror_k);

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
