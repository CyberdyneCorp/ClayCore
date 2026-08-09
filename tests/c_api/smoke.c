/* C11 consumer smoke test (c-abi spec scenario "Pure C consumer"):
 * create a document, add a sphere edit, evaluate points, mesh, export OBJ,
 * save/load the document — every failure path returns an error code. The
 * sections below it cover the item builder: a composed edit, the modifier
 * chain, payloads of any length, every clay_prim value, the versioned
 * descriptor structs, and the error paths of all of them. */

#include <clay.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s (last error: %s)\n", __FILE__, \
                    __LINE__, #cond, clay_last_error());                     \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* Same, plus a printf-style line saying what diverged. */
#define REQUIREF(cond, ...)                                                  \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s (last error: %s)\n  ",         \
                    __FILE__, __LINE__, #cond, clay_last_error());           \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* -- item builder ---------------------------------------------------------- */

/* A composed edit: a primitive, a chain of warps, an array — added to a
 * layer and evaluated. */
static int check_item_builder(void) {
    clay_document* composed = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(composed, "composed", &layer) == CLAY_OK);

    float box_params[3] = {0.4f, 0.9f, 0.4f};
    float place[3] = {0.0f, 0.0f, 0.0f};
    float axis[3] = {0.0f, 1.0f, 0.0f};
    float rgb[3] = {0.3f, 0.6f, 0.2f};
    float twist[1] = {0.8f};
    float bend[1] = {0.35f};
    float taper[4] = {-1.0f, 1.0f, 1.0f, 0.4f};
    clay_item* item = clay_item_create(CLAY_PRIM_BOX, box_params, 3);
    REQUIRE(item != NULL);
    REQUIRE(clay_item_set_position(item, place) == CLAY_OK);
    REQUIRE(clay_item_set_rotation(item, axis, 0.3f) == CLAY_OK);
    REQUIRE(clay_item_set_scale(item, 1.1f) == CLAY_OK);
    REQUIRE(clay_item_set_color(item, rgb) == CLAY_OK);
    REQUIRE(clay_item_set_rounding(item, 0.02f) == CLAY_OK);
    REQUIRE(clay_item_set_mirror(item, 0) == CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TWIST, twist, 1, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_BEND, bend, 1, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TAPER, taper, 4, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_item_set_repeat_radial(item, 5, 1.4f) == CLAY_OK);
    clay_node_id composed_node = 0;
    REQUIRE(clay_layer_add_item(composed, layer, item, &composed_node) == CLAY_OK);
    REQUIRE(composed_node != 0);
    clay_item_destroy(item);

    float pts[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f};
    float d[2] = {99.0f, 99.0f};
    float rgb_out[6] = {0.0f};
    REQUIRE(clay_eval_points(composed, NULL, pts, 2, d, rgb_out) == CLAY_OK);
    REQUIREF(d[0] < 0.0f, "the composed edit is empty at the origin: d = %g", (double)d[0]);
    REQUIREF(d[1] > 0.0f, "the composed edit fills (0, 0, 4): d = %g", (double)d[1]);
    REQUIREF(rgb_out[0] > 0.29f && rgb_out[0] < 0.31f,
             "the item's colour did not reach the field: red = %g", (double)rgb_out[0]);
    clay_document_destroy(composed);
    return 0;
}

/* Every way of getting one wrong: parameter counts, unknown enumerators,
 * out-of-range values, payloads on the wrong primitive. */
static int check_builder_rejections(void) {
    float box_params[3] = {0.4f, 0.9f, 0.4f};
    float twist[1] = {0.8f};
    float taper[4] = {-1.0f, 1.0f, 1.0f, 0.4f};
    clay_item* item = clay_item_create(CLAY_PRIM_BOX, box_params, 3);
    REQUIRE(item != NULL);
    REQUIRE(clay_item_create(CLAY_PRIM_BOX, box_params, 2) == NULL);
    REQUIRE(strlen(clay_last_error()) > 0);
    REQUIRE(clay_item_create(999, box_params, 3) == NULL);
    REQUIRE(clay_item_create(CLAY_PRIM_BOX, NULL, 3) == NULL);
    float flat_taper[4] = {1.0f, 1.0f, 1.0f, 1.0f}; /* y1 == y0 */
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TAPER, taper, 3, 0) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TAPER, flat_taper, 4, 0) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_add_deformer(item, CLAY_DEFORM_TWIST, twist, 1, CLAY_EASE_COUNT) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_add_deformer(item, 99, twist, 1, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_scale(item, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_blend(item, CLAY_BLEND_QUADRATIC, -1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_blend(item, 99, 0.1f) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_rounding(item, -0.1f) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_op(item, 250) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_repeat_radial(item, 1, 0.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    float wide[3] = {1.0f, 2.0f, 3.0f};
    float counts[3] = {1.0f, 1.0f, 1.0f};
    REQUIRE(clay_item_set_repeat_grid(item, wide, counts) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_repeat_grid(item, wide, NULL) == CLAY_OK); /* infinite grids may differ */
    REQUIRE(clay_item_set_repeat_radial(item, 5, 1.4f) == CLAY_OK); /* one repeat: last call wins */
    /* payload setters check the primitive they belong to */
    REQUIRE(clay_item_set_stroke_points(item, NULL, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_profile(item, CLAY_PROFILE_CIRCLE, twist, 1) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    clay_item_destroy(item);
    return 0;
}

/* A transition op needs its parameters, of its own kind, and no other op
 * accepts them. */
static int check_transition_pairing(void) {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "morph", &layer) == CLAY_OK);
    float r[1] = {0.6f};
    float from[3] = {0.0f, -1.0f, 0.0f};
    float to[3] = {0.0f, 1.0f, 0.0f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(item != NULL);
    REQUIRE(clay_item_set_op(item, CLAY_OP_TRANSITION_LINEAR) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_transition_linear(item, from, from, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_transition_radial(item, 1.0f, 1.0f, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_transition_linear(item, from, to, CLAY_EASE_COUNT) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_transition_linear(item, from, to, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, NULL) == CLAY_OK);
    REQUIRE(clay_item_set_transition_radial(item, 0.2f, 1.5f, CLAY_EASE_LINEAR) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT); /* radial parameters, linear op */
    REQUIRE(clay_item_set_op(item, CLAY_OP_TRANSITION_RADIAL) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, NULL) == CLAY_OK);
    REQUIRE(clay_item_set_op(item, CLAY_OP_ADD) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, item, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT); /* parameters without a morph op */
    clay_item_destroy(item);
    clay_document_destroy(doc);
    return 0;
}

/* Variable-length payloads: the caller's arrays need not outlive the call. */
static int check_builder_payloads(void) {
    clay_document* composed = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(composed, "payloads", &layer) == CLAY_OK);

    float chain[12] = {-0.6f, 0.0f, 0.0f, 0.20f, 0.0f, 0.3f, 0.0f, 0.15f,
                       0.6f,  0.0f, 0.0f, 0.10f};
    float tip[3] = {0.9f, 0.2f, 0.0f};
    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, NULL, 0);
    REQUIRE(stroke != NULL);
    REQUIRE(clay_item_set_stroke_points(stroke, chain, 3) == CLAY_OK);
    REQUIRE(clay_item_add_stroke_point(stroke, tip, 0.1f) == CLAY_OK);
    REQUIRE(clay_item_add_stroke_point(stroke, tip, -1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_stroke_blend_k(stroke, 0.04f) == CLAY_OK);
    REQUIRE(clay_item_set_stroke_blend_k(stroke, -0.1f) == CLAY_ERROR_INVALID_ARGUMENT);
    memset(chain, 0, sizeof chain);
    clay_node_id stroke_node = 0;
    REQUIRE(clay_layer_add_item(composed, layer, stroke, &stroke_node) == CLAY_OK);
    clay_item_destroy(stroke);

    /* Reading the placed chain back is what a host that reloaded this document
     * would do, and here it also shows the payload really was copied: `chain`
     * was zeroed above and the builder destroyed. */
    size_t point_count = 0;
    int32_t stroke_closed = -1;
    REQUIRE(clay_layer_stroke_points(composed, layer, stroke_node, NULL, &point_count, NULL,
                                     NULL, NULL, &stroke_closed, NULL) == CLAY_OK);
    REQUIRE(point_count == 4);
    REQUIRE(stroke_closed == 0);
    float read_back[16] = {0};
    size_t capacity = 1;
    REQUIRE(clay_layer_stroke_points(composed, layer, stroke_node, read_back, &capacity, NULL,
                                     NULL, NULL, NULL, NULL) == CLAY_ERROR_BUFFER_TOO_SMALL);
    REQUIRE(capacity == 4);
    REQUIRE(clay_layer_stroke_points(composed, layer, stroke_node, read_back, &capacity, NULL,
                                     NULL, NULL, NULL, NULL) == CLAY_OK);
    REQUIREF(read_back[0] == -0.6f && read_back[3] == 0.20f,
             "the first control point survived: (%g, %g, %g, %g)", read_back[0], read_back[1],
             read_back[2], read_back[3]);

    float half_depth[1] = {0.25f};
    float bad_depth[1] = {0.0f};
    float outline[8] = {-0.4f, -0.4f, 0.4f, -0.4f, 0.4f, 0.4f, -0.4f, 0.4f};
    REQUIRE(clay_item_create(CLAY_PRIM_EXTRUDE, bad_depth, 1) == NULL);
    clay_item* lift = clay_item_create(CLAY_PRIM_EXTRUDE, half_depth, 1);
    REQUIRE(lift != NULL);
    REQUIRE(clay_item_set_profile_polygon(lift, outline, 2) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_profile(lift, CLAY_PROFILE_POLYGON, outline, 1) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_profile(lift, 99, outline, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_profile_polygon(lift, outline, 4) == CLAY_OK);
    memset(outline, 0, sizeof outline);
    REQUIRE(clay_layer_add_item(composed, layer, lift, NULL) == CLAY_OK);
    clay_item_destroy(lift);

    float pts[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f};
    float d[2] = {99.0f, 99.0f};
    REQUIRE(clay_eval_points(composed, NULL, pts, 2, d, NULL) == CLAY_OK);
    REQUIREF(d[0] < 0.0f, "the stroke and the lift are empty at the origin: d = %g",
             (double)d[0]);
    REQUIREF(d[1] < 0.0f, "the stroke is empty at its own vertex (0, 0.3, 0): d = %g",
             (double)d[1]);
    clay_document_destroy(composed);
    return 0;
}

/* Payload length is a caller's choice: a long stroke and a many-sided
 * polygon go through the same setters as a short one. */
static int check_large_payloads(void) {
    enum { kPoints = 300, kVertices = 128 };
    float* chain = (float*)malloc((size_t)kPoints * 4 * sizeof(float));
    float* outline = (float*)malloc((size_t)kVertices * 2 * sizeof(float));
    REQUIRE(chain != NULL && outline != NULL);
    for (int i = 0; i < kPoints; ++i) { /* a straight rod of radius 0.1 */
        float t = (float)i / (float)(kPoints - 1);
        chain[i * 4] = -1.0f + 2.0f * t;
        chain[i * 4 + 1] = 0.0f;
        chain[i * 4 + 2] = 0.0f;
        chain[i * 4 + 3] = 0.1f;
    }
    for (int i = 0; i < kVertices; ++i) { /* a 128-gon of radius 0.5 */
        float a = 6.2831853f * (float)i / (float)kVertices;
        outline[i * 2] = 0.5f * cosf(a);
        outline[i * 2 + 1] = 0.5f * sinf(a);
    }

    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "payloads", &layer) == CLAY_OK);

    clay_item* stroke = clay_item_create(CLAY_PRIM_STROKE, NULL, 0);
    REQUIRE(stroke != NULL);
    REQUIRE(clay_item_set_stroke_points(stroke, chain, kPoints) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, stroke, NULL) == CLAY_OK);
    clay_item_destroy(stroke);

    float depth[1] = {0.2f};
    float where[3] = {3.0f, 0.0f, 0.0f};
    clay_item* lift = clay_item_create(CLAY_PRIM_EXTRUDE, depth, 1);
    REQUIRE(lift != NULL);
    REQUIRE(clay_item_set_profile_polygon(lift, outline, kVertices) == CLAY_OK);
    REQUIRE(clay_item_set_position(lift, where) == CLAY_OK);
    REQUIRE(clay_layer_add_item(doc, layer, lift, NULL) == CLAY_OK);
    clay_item_destroy(lift);

    free(chain); /* the builder copied both payloads in */
    free(outline);

    float pts[12] = {0.0f, 0.0f, 0.0f,  0.0f, 0.5f, 0.0f,
                     3.0f, 0.0f, 0.0f,  3.9f, 0.0f, 0.0f};
    float d[4];
    REQUIRE(clay_eval_points(doc, NULL, pts, 4, d, NULL) == CLAY_OK);
    REQUIREF(fabsf(d[0] + 0.1f) < 1e-3f, "%d-point rod: d at its axis is %g, expected -0.1",
             kPoints, (double)d[0]);
    REQUIREF(fabsf(d[1] - 0.4f) < 1e-3f, "%d-point rod: d at 0.5 above it is %g, expected 0.4",
             kPoints, (double)d[1]);
    REQUIREF(fabsf(d[2] + 0.2f) < 1e-3f, "%d-gon: d at its centre is %g, expected -0.2 (half depth)",
             kVertices, (double)d[2]);
    REQUIREF(d[3] > 0.3f, "%d-gon: d 0.9 from its centre is %g, expected the outside",
             kVertices, (double)d[3]);
    clay_document_destroy(doc);
    return 0;
}

/* -- every primitive ------------------------------------------------------- */

/* One edit of every clay_prim value. The table is indexed by the enum: entry
 * i is primitive i, and the sweep below fails if the library accepts a value
 * this table does not cover — which is what happens when a primitive is added
 * to the engine. */
static const struct {
    int32_t prim;
    size_t count;
    float params[7];
} kZoo[] = {{CLAY_PRIM_SPHERE, 1, {0.5f}},
            {CLAY_PRIM_BOX, 3, {0.4f, 0.4f, 0.4f}},
            {CLAY_PRIM_ROUND_BOX, 4, {0.4f, 0.3f, 0.3f, 0.05f}},
            {CLAY_PRIM_BOX_FRAME, 4, {0.4f, 0.4f, 0.4f, 0.05f}},
            {CLAY_PRIM_TORUS, 2, {0.5f, 0.15f}},
            {CLAY_PRIM_CAPSULE, 7, {-0.3f, 0.0f, 0.0f, 0.3f, 0.0f, 0.0f, 0.2f}},
            {CLAY_PRIM_CAPPED_CYLINDER, 2, {0.3f, 0.4f}},
            {CLAY_PRIM_ROUNDED_CYLINDER, 3, {0.3f, 0.05f, 0.4f}},
            {CLAY_PRIM_CAPPED_CONE, 3, {0.4f, 0.3f, 0.1f}},
            {CLAY_PRIM_ROUND_CONE, 3, {0.3f, 0.1f, 0.5f}},
            {CLAY_PRIM_ELLIPSOID, 3, {0.4f, 0.3f, 0.2f}},
            {CLAY_PRIM_OCTAHEDRON, 1, {0.5f}},
            {CLAY_PRIM_HEX_PRISM, 2, {0.3f, 0.2f}},
            {CLAY_PRIM_PYRAMID, 1, {0.6f}},
            {CLAY_PRIM_STROKE, 0, {0.0f}},
            {CLAY_PRIM_EXTRUDE, 1, {0.2f}},
            {CLAY_PRIM_REVOLVE, 1, {0.6f}},
            {CLAY_PRIM_CAPPED_TORUS, 4, {0.8415f, 0.5403f, 0.5f, 0.15f}},
            {CLAY_PRIM_LINK, 3, {0.3f, 0.4f, 0.15f}},
            {CLAY_PRIM_CYLINDER_INFINITE, 3, {0.0f, 0.0f, 0.3f}},
            {CLAY_PRIM_CONE, 3, {0.5f, 0.866f, 0.5f}},
            {CLAY_PRIM_PLANE, 4, {0.0f, 1.0f, 0.0f, 2.0f}},
            {CLAY_PRIM_CUT_SPHERE, 2, {0.5f, 0.1f}},
            {CLAY_PRIM_CUT_HOLLOW_SPHERE, 3, {0.5f, 0.1f, 0.05f}},
            {CLAY_PRIM_SOLID_ANGLE, 3, {0.5f, 0.866f, 0.5f}},
            {CLAY_PRIM_TETRAHEDRON, 1, {0.4f}},
            {CLAY_PRIM_DODECAHEDRON, 1, {0.4f}},
            {CLAY_PRIM_ICOSAHEDRON, 1, {0.4f}},
            {CLAY_PRIM_TRI_PRISM, 2, {0.3f, 0.2f}},
            {CLAY_PRIM_OCTAHEDRON_CHEAP, 1, {0.4f}},
            {CLAY_PRIM_LNORM_SPHERE, 2, {0.4f, 4.0f}},
            /* half-depth and ease; the profiles are out of line, added below */
            {CLAY_PRIM_LOFT, 2, {0.5f, 0.0f}},
            /* ease only: both the guide and the profiles are out of line */
            {CLAY_PRIM_SWEPT, 1, {0.0f}}};

#define ZOO_COUNT (sizeof kZoo / sizeof kZoo[0])

/* The item for one table entry, with the payload its primitive needs. */
static clay_item* zoo_item(size_t i) {
    static const float chain[8] = {-0.4f, 0.0f, 0.0f, 0.15f, 0.4f, 0.0f, 0.0f, 0.15f};
    static const float profile[1] = {0.3f};
    clay_item* one = clay_item_create(kZoo[i].prim, kZoo[i].params, kZoo[i].count);
    if (!one) return NULL;
    clay_result r = CLAY_OK;
    if (kZoo[i].prim == CLAY_PRIM_STROKE)
        r = clay_item_set_stroke_points(one, chain, 2);
    if (kZoo[i].prim == CLAY_PRIM_EXTRUDE || kZoo[i].prim == CLAY_PRIM_REVOLVE)
        r = clay_item_set_profile(one, CLAY_PROFILE_HEXAGON, profile, 1);
    if (kZoo[i].prim == CLAY_PRIM_SWEPT) {
        static const float guide[8] = {-0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.2f, 0.0f, 0.0f};
        static const float wide[1] = {0.25f};
        static const float narrow[1] = {0.1f};
        r = clay_item_set_curve_points(one, guide, 2, NULL, NULL, NULL);
        if (r == CLAY_OK)
            r = clay_item_add_loft_profile(one, CLAY_PROFILE_CIRCLE, wide, 1, NULL, 0);
        if (r == CLAY_OK)
            r = clay_item_add_loft_profile(one, CLAY_PROFILE_CIRCLE, narrow, 1, NULL, 0);
    }
    if (kZoo[i].prim == CLAY_PRIM_LOFT) {
        static const float wide[1] = {0.5f};
        static const float narrow[1] = {0.15f};
        r = clay_item_add_loft_profile(one, CLAY_PROFILE_CIRCLE, wide, 1, NULL, 0);
        if (r == CLAY_OK)
            r = clay_item_add_loft_profile(one, CLAY_PROFILE_CIRCLE, narrow, 1, NULL, 0);
    }
    if (r != CLAY_OK) {
        clay_item_destroy(one);
        return NULL;
    }
    return one;
}

/* 27 sample points spread over the unit cube. */
static size_t zoo_grid(float* out) {
    size_t n = 0;
    for (int ix = -1; ix <= 1; ++ix)
        for (int iy = -1; iy <= 1; ++iy)
            for (int iz = -1; iz <= 1; ++iz) {
                out[n * 3] = 0.7f * (float)ix;
                out[n * 3 + 1] = 0.7f * (float)iy;
                out[n * 3 + 2] = 0.7f * (float)iz;
                ++n;
            }
    return n;
}

/* One primitive alone in a document: it evaluates, in range, and its field
 * varies — a primitive that contributed nothing would read the same
 * everywhere. */
static int check_one_primitive(size_t i) {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "one", &layer) == CLAY_OK);
    clay_item* one = zoo_item(i);
    REQUIREF(one != NULL, "clay_prim %d: the table entry does not build", (int)kZoo[i].prim);
    REQUIREF(clay_layer_add_item(doc, layer, one, NULL) == CLAY_OK, "clay_prim %d does not add",
             (int)kZoo[i].prim);
    clay_item_destroy(one);

    float pts[27 * 3];
    float d[27];
    size_t n = zoo_grid(pts);
    REQUIREF(clay_eval_points(doc, NULL, pts, n, d, NULL) == CLAY_OK,
             "clay_prim %d does not evaluate", (int)kZoo[i].prim);
    float lo = d[0], hi = d[0];
    for (size_t k = 1; k < n; ++k) {
        if (d[k] < lo) lo = d[k];
        if (d[k] > hi) hi = d[k];
    }
    REQUIREF(lo > -1e6f && hi < 1e6f, "clay_prim %d evaluates out of range: [%g, %g]",
             (int)kZoo[i].prim, (double)lo, (double)hi);
    REQUIREF(hi - lo > 1e-4f, "clay_prim %d reads %g everywhere: it contributes no field",
             (int)kZoo[i].prim, (double)lo);
    clay_document_destroy(doc);
    return 0;
}

/* All of them in one document: it evaluates, and the bounded subset meshes. */
static int check_primitive_zoo_document(void) {
    clay_document* all = clay_document_create();
    clay_document* bounded = clay_document_create();
    clay_layer_id all_layer = 0, bounded_layer = 0;
    REQUIRE(clay_add_sdf_layer(all, "zoo", &all_layer) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(bounded, "zoo", &bounded_layer) == CLAY_OK);
    for (size_t i = 0; i < ZOO_COUNT; ++i) {
        clay_item* one = zoo_item(i);
        REQUIREF(one != NULL, "clay_prim %d: the table entry does not build", (int)kZoo[i].prim);
        REQUIRE(clay_layer_add_item(all, all_layer, one, NULL) == CLAY_OK);
        /* the two unbounded primitives make a scene that cannot be meshed */
        if (kZoo[i].prim != CLAY_PRIM_PLANE && kZoo[i].prim != CLAY_PRIM_CYLINDER_INFINITE)
            REQUIRE(clay_layer_add_item(bounded, bounded_layer, one, NULL) == CLAY_OK);
        clay_item_destroy(one);
    }
    float pts[6] = {0.0f, 0.0f, 0.0f, 0.7f, 0.2f, 0.1f};
    float d[2] = {0.0f, 0.0f};
    REQUIRE(clay_eval_points(all, NULL, pts, 2, d, NULL) == CLAY_OK);
    REQUIREF(d[0] > -1e6f && d[0] < 1e6f, "the zoo evaluates to %g at the origin", (double)d[0]);
    REQUIREF(d[1] > -1e6f && d[1] < 1e6f, "the zoo evaluates to %g off centre", (double)d[1]);

    clay_mesh_params mp;
    memset(&mp, 0, sizeof mp);
    mp.struct_size = (uint32_t)sizeof mp;
    mp.resolution = 32;
    clay_mesh* mesh = NULL;
    REQUIRE(clay_document_mesh(bounded, &mp, &mesh) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(mesh) > 0);
    REQUIRE(clay_mesh_index_count(mesh) % 3 == 0);
    clay_mesh_destroy(mesh);
    mesh = NULL;
    /* the unbounded ones are why that subset exists */
    REQUIRE(clay_document_mesh(all, &mp, &mesh) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(mesh == NULL);
    clay_document_destroy(all);
    clay_document_destroy(bounded);
    return 0;
}

static int check_every_primitive(void) {
    for (size_t i = 0; i < ZOO_COUNT; ++i)
        REQUIREF(kZoo[i].prim == (int32_t)i, "table slot %d holds clay_prim %d: the table is "
                 "indexed by the enum", (int)i, (int)kZoo[i].prim);

    /* driven from the library, not from the table: every primitive value it
     * accepts must be covered above, so a new one is caught here. The first
     * two probe values are a unit sine and cosine, which is what the angle
     * primitives take. */
    float probe[7] = {0.6f, 0.8f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    int highest = -1;
    for (int32_t p = 0; p < 64; ++p) {
        for (size_t n = 0; n <= 7; ++n) {
            clay_item* one = clay_item_create(p, probe, n);
            if (!one) continue;
            clay_item_destroy(one);
            highest = (int)p;
            break;
        }
    }
    REQUIREF(highest + 1 == (int)ZOO_COUNT,
             "the library accepts clay_prim values up to %d, the table covers %d: give the new "
             "primitive an entry", highest, (int)ZOO_COUNT);

    for (size_t i = 0; i < ZOO_COUNT; ++i)
        if (check_one_primitive(i) != 0) return 1;
    return check_primitive_zoo_document();
}

/* -- versioned descriptor structs ------------------------------------------ */

/* struct_size is the caller's declared layout, and setting it is mandatory: a
 * longer one means fields this build does not know, and anything shorter than
 * the original layout — zero included, which is what a descriptor from ABI
 * 0.1.0 leaves there — is rejected. Unset fields take their documented
 * defaults. */
static int check_struct_versioning(void) {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "versioned", &layer) == CLAY_OK);

    clay_item_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.prim = CLAY_PRIM_SPHERE;
    desc.params[0] = 1.0f;
    desc.rotation[3] = 1.0f;
    desc.scale = 1.0f;
    /* a descriptor that never declares its layout is not read */
    REQUIRE(clay_add_item(doc, layer, &desc, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    desc.struct_size = (uint32_t)sizeof desc;
    REQUIRE(clay_add_item(doc, layer, &desc, NULL) == CLAY_OK);
    desc.struct_size = 4; /* shorter than any layout that ever shipped */
    REQUIRE(clay_add_item(doc, layer, &desc, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    desc.struct_size = 0x3d23d70a; /* the bits of 0.04f: not a struct size */
    REQUIRE(clay_add_item(doc, layer, &desc, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    desc.struct_size = (uint32_t)sizeof desc;

    /* a caller from the future: the tail this build cannot name is ignored */
    struct {
        clay_item_desc desc;
        float tail[4];
    } future;
    memset(&future, 0, sizeof future);
    future.desc = desc;
    future.desc.struct_size = (uint32_t)sizeof future;
    for (int i = 0; i < 4; ++i) future.tail[i] = 1234.5f;
    REQUIRE(clay_add_item(doc, layer, &future.desc, NULL) == CLAY_OK);

    /* the same rule on the meshing descriptor, and the defaults it documents:
     * declaring the layout and nothing else meshes exactly as an explicit
     * resolution 128 does */
    clay_mesh_params zeroed, spelled_out;
    memset(&zeroed, 0, sizeof zeroed);
    memset(&spelled_out, 0, sizeof spelled_out);
    spelled_out.struct_size = (uint32_t)sizeof spelled_out;
    spelled_out.resolution = 128;
    clay_mesh* from_defaults = NULL;
    clay_mesh* from_fields = NULL;
    REQUIRE(clay_document_mesh(doc, &zeroed, &from_defaults) == CLAY_ERROR_INVALID_ARGUMENT);
    zeroed.struct_size = (uint32_t)sizeof zeroed;
    REQUIRE(clay_document_mesh(doc, &zeroed, &from_defaults) == CLAY_OK);
    REQUIRE(clay_document_mesh(doc, &spelled_out, &from_fields) == CLAY_OK);
    REQUIREF(clay_mesh_vertex_count(from_defaults) == clay_mesh_vertex_count(from_fields),
             "a zeroed clay_mesh_params gave %zu vertices, an explicit resolution 128 gave %zu",
             clay_mesh_vertex_count(from_defaults), clay_mesh_vertex_count(from_fields));
    REQUIRE(clay_mesh_vertex_count(from_defaults) > 0);
    clay_mesh_destroy(from_defaults);
    clay_mesh_destroy(from_fields);

    clay_mesh_params truncated = spelled_out;
    truncated.struct_size = 4;
    clay_mesh* rejected = NULL;
    REQUIRE(clay_document_mesh(doc, &truncated, &rejected) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(rejected == NULL);
    clay_document_destroy(doc);
    return 0;
}

/* -- error paths ----------------------------------------------------------- */

/* Null handles are arguments, not crashes, and a released handle is released
 * once: a C caller clears the pointer, so the second destroy is a null no-op.
 * (Destroying the same non-null handle twice is a double free, as it is in
 * any C API, and is not something the library can detect.) */
static int check_error_paths(void) {
    float three[3] = {1.0f, 1.0f, 1.0f};
    REQUIRE(clay_add_item(NULL, 0, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_position(NULL, three) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_scale(NULL, 1.0f) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_add_deformer(NULL, CLAY_DEFORM_TWIST, three, 1, 0) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_repeat_grid(NULL, three, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_item_set_transition_radial(NULL, 0.0f, 1.0f, 0) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_layer_add_item(NULL, 0, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_eval_gradients(NULL, NULL, three, 1, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_layer_eval_points(NULL, 0, NULL, three, 1, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_layer_eval_gradients(NULL, 0, NULL, three, 1, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_safe_step_scale(NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_layer_safe_step_scale(NULL, 0, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_raycast_many(NULL, NULL, 1, NULL, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_raycast_attributed(NULL, three, three, NULL, NULL, NULL, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_snap_to_surface(NULL, three, 1, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_layer_bounds(NULL, 0, NULL, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_layer_selection_bounds(NULL, 0, NULL, 0, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_raycast(NULL, three, three, NULL, NULL, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_build_plane_pick(NULL, three, three, 0, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_mesh(NULL, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_mesh_validate(NULL, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_mesh_save(NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_save(NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(strlen(clay_last_error()) > 0);
    /* the accessors answer for a null mesh instead of dereferencing it */
    REQUIRE(clay_mesh_vertex_count(NULL) == 0);
    REQUIRE(clay_mesh_index_count(NULL) == 0);
    REQUIRE(clay_mesh_positions(NULL) == NULL);
    REQUIRE(clay_mesh_indices(NULL) == NULL);

    float r[1] = {0.5f};
    clay_item* item = clay_item_create(CLAY_PRIM_SPHERE, r, 1);
    REQUIRE(item != NULL);
    clay_item_destroy(item);
    item = NULL;
    clay_item_destroy(item);
    clay_document* doc = clay_document_create();
    clay_document_destroy(doc);
    doc = NULL;
    clay_document_destroy(doc);
    clay_mesh_destroy(NULL);
    return 0;
}

/* -- the flat descriptor path ---------------------------------------------- */

/* backend enumeration: size query first, then fill */
static int check_backends(void) {
    size_t size = 0;
    REQUIRE(clay_list_backends(NULL, &size) == CLAY_OK && size >= 4);
    char small[2];
    size_t small_size = sizeof small;
    REQUIRE(clay_list_backends(small, &small_size) == CLAY_ERROR_BUFFER_TOO_SMALL);
    REQUIRE(small_size == size); /* required size reported */
    char* names = (char*)malloc(size);
    REQUIRE(clay_list_backends(names, &size) == CLAY_OK);
    REQUIRE(strstr(names, "cpu") != NULL);
    free(names);
    return 0;
}

/* The edits the rest of the file evaluates, meshes and saves, added through
 * clay_item_desc exactly as a 0.1.0 consumer added them. */
static int check_flat_path(clay_document* doc, clay_layer_id layer) {
    clay_item_desc sphere;
    memset(&sphere, 0, sizeof sphere);
    sphere.struct_size = (uint32_t)sizeof sphere;
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 1.0f;
    sphere.rotation[3] = 1.0f;
    sphere.scale = 1.0f;
    sphere.color[0] = 0.8f;
    sphere.color[1] = 0.3f;
    sphere.color[2] = 0.2f;
    clay_node_id node = 0;
    REQUIRE(clay_add_item(doc, layer, &sphere, &node) == CLAY_OK);

    clay_item_desc box = sphere;
    box.prim = CLAY_PRIM_BOX;
    box.params[0] = box.params[1] = box.params[2] = 0.5f;
    box.position[0] = 0.9f;
    box.op = CLAY_OP_ADD;
    box.blend = CLAY_BLEND_QUADRATIC;
    box.blend_k = 0.1f;
    REQUIRE(clay_add_item(doc, layer, &box, NULL) == CLAY_OK);

    /* the backfilled primitives are reachable from the flat path too */
    clay_document* scratch = clay_document_create();
    clay_layer_id scratch_layer = 0;
    REQUIRE(clay_add_sdf_layer(scratch, "scratch", &scratch_layer) == CLAY_OK);
    clay_item_desc link = sphere;
    link.prim = CLAY_PRIM_LINK;
    link.params[0] = 0.3f; /* le r1 r2 */
    link.params[1] = 0.4f;
    link.params[2] = 0.15f;
    REQUIRE(clay_add_item(scratch, scratch_layer, &link, NULL) == CLAY_OK);
    clay_document_destroy(scratch);

    /* error paths */
    REQUIRE(clay_add_item(doc, 9999, &sphere, NULL) == CLAY_ERROR_NOT_FOUND);
    REQUIRE(strlen(clay_last_error()) > 0);
    REQUIRE(clay_remove_node(doc, layer, 424242) == CLAY_ERROR_NOT_FOUND);
    clay_item_desc bad = sphere;
    bad.prim = 999;
    REQUIRE(clay_add_item(doc, layer, &bad, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    /* out-of-line payloads and transition parameters do not fit the descriptor */
    bad = sphere;
    bad.prim = CLAY_PRIM_STROKE;
    REQUIRE(clay_add_item(doc, layer, &bad, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    bad = sphere;
    bad.prim = CLAY_PRIM_EXTRUDE;
    REQUIRE(clay_add_item(doc, layer, &bad, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    bad = sphere;
    bad.op = CLAY_OP_TRANSITION_RADIAL;
    REQUIRE(clay_add_item(doc, layer, &bad, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    bad = sphere;
    bad.blend = 99;
    REQUIRE(clay_add_item(doc, layer, &bad, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    return 0;
}

/* -- voxel grids ----------------------------------------------------------- */

/* A brush declaring the layout it was compiled against, hard-edged. */
static clay_brush_params cube_brush(int32_t size) {
    clay_brush_params b;
    memset(&b, 0, sizeof b);
    b.struct_size = (uint32_t)sizeof b;
    b.size = size;
    b.shape = CLAY_BRUSH_SHAPE_CUBE;
    b.falloff = CLAY_BRUSH_FALLOFF_CONSTANT;
    b.strength = 1.0f; /* no field of this descriptor has a default */
    return b;
}

/* Palette, single and batch edits, fills, mirror and the queries over a
 * caller-owned grid. */
static int check_voxel_edits(void) {
    const int32_t origin[3] = {0, 0, 0};
    const int32_t away[3] = {5, 0, 0};
    const int32_t batch[6] = {10, 0, 0, 11, 0, 0};
    const int32_t box_a[3] = {-2, -2, -2}, box_b[3] = {-1, -1, -1};
    float red[3] = {1.0f, 0.0f, 0.0f}, blue[3] = {0.0f, 0.0f, 1.0f};
    float back[3] = {0.0f, 0.0f, 0.0f};
    int32_t red_index = -1, blue_index = -1, read = -1, has_bounds = -1;
    int32_t lo[3], hi[3];
    size_t palette = 0, occupied = 0;
    float voxel_size = 0.0f;

    clay_voxel_grid* grid = clay_voxel_grid_create(0.25f);
    REQUIRE(grid != NULL);
    REQUIRE(clay_voxel_grid_create(0.0f) == NULL);
    REQUIRE(clay_voxel_size(grid, &voxel_size) == CLAY_OK);
    REQUIRE(voxel_size == 0.25f);

    /* index 0 is the empty slot: never added, never recolored */
    REQUIRE(clay_voxel_palette_size(grid, &palette) == CLAY_OK);
    REQUIRE(palette == 1);
    REQUIRE(clay_voxel_palette_add(grid, red, &red_index) == CLAY_OK);
    REQUIRE(clay_voxel_palette_add(grid, blue, &blue_index) == CLAY_OK);
    REQUIRE(red_index == 1 && blue_index == 2);
    REQUIRE(clay_voxel_palette_color(grid, red_index, back) == CLAY_OK);
    REQUIRE(back[0] == 1.0f && back[2] == 0.0f);
    /* a recolor is read back off the entry it recolored, and only that one */
    REQUIRE(clay_voxel_palette_color(grid, blue_index, back) == CLAY_OK);
    REQUIRE(back[0] == 0.0f && back[2] == 1.0f);
    REQUIRE(clay_voxel_palette_set(grid, blue_index, red) == CLAY_OK);
    REQUIRE(clay_voxel_palette_color(grid, blue_index, back) == CLAY_OK);
    REQUIREF(back[0] == 1.0f && back[2] == 0.0f, "the recolored entry reads %g %g %g",
             (double)back[0], (double)back[1], (double)back[2]);
    /* index 0 is the empty slot: recoloring it is a no-op, not an error */
    REQUIRE(clay_voxel_palette_set(grid, 0, red) == CLAY_OK);
    REQUIRE(clay_voxel_palette_color(grid, 0, back) == CLAY_OK);
    REQUIRE(back[0] == 0.0f);
    REQUIRE(clay_voxel_palette_set(grid, 300, red) == CLAY_ERROR_INVALID_ARGUMENT);

    /* set, get, paint (occupied cells only), erase */
    REQUIRE(clay_voxel_set(grid, origin, red_index) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, origin, &read) == CLAY_OK);
    REQUIRE(read == red_index);
    REQUIRE(clay_voxel_paint(grid, origin, blue_index) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, origin, &read) == CLAY_OK && read == blue_index);
    REQUIRE(clay_voxel_paint(grid, away, blue_index) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, away, &read) == CLAY_OK && read == 0);
    REQUIRE(clay_voxel_erase(grid, origin) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, origin, &read) == CLAY_OK && read == 0);

    REQUIRE(clay_voxel_set_many(grid, batch, 2, red_index) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    REQUIRE(occupied == 2);
    REQUIRE(clay_voxel_erase_many(grid, batch, 2) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied == 0);

    /* an empty grid has no bounds, and says so instead of inventing some */
    REQUIRE(clay_voxel_bounds(grid, lo, hi, &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 0);

    REQUIRE(clay_voxel_fill_box(grid, box_a, box_b, red_index) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied == 8);
    REQUIRE(clay_voxel_bounds(grid, lo, hi, &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 1);
    REQUIREF(lo[0] == -2 && hi[0] == -1, "box fill bounds x are %d..%d, expected -2..-1", lo[0],
             hi[0]);

    REQUIRE(clay_voxel_fill_line(grid, batch, batch + 3, red_index) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, batch, &read) == CLAY_OK && read == red_index);

    /* mirrored: the cell given plus its reflection about lattice 0 */
    const int32_t right[3] = {3, 1, 1};
    const int32_t left[3] = {-4, 1, 1};
    REQUIRE(clay_voxel_set_mirrored(grid, right, red_index, CLAY_MIRROR_X) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, left, &read) == CLAY_OK && read == red_index);
    REQUIRE(clay_voxel_paint_mirrored(grid, right, blue_index, CLAY_MIRROR_X) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, left, &read) == CLAY_OK && read == blue_index);
    REQUIRE(clay_voxel_set_mirrored(grid, right, red_index, 8) == CLAY_ERROR_INVALID_ARGUMENT);

    REQUIRE(clay_voxel_grid_destroy(grid) == CLAY_OK);
    return 0;
}

/* Brushes, the sculpting verbs, flood select's size-query, the step field and
 * greedy meshing. */
static int check_voxel_sculpting(void) {
    const int32_t centre[3] = {0, 0, 0};
    const int32_t outside[3] = {40, 40, 40};
    float white[3] = {1.0f, 1.0f, 1.0f};
    int32_t index = -1;
    size_t occupied = 0, selected = 0, capacity = 0;
    clay_brush_params cube5 = cube_brush(5);
    clay_brush_params sphere5 = cube_brush(5);
    sphere5.shape = CLAY_BRUSH_SHAPE_SPHERE;

    clay_voxel_grid* grid = clay_voxel_grid_create(0.1f);
    REQUIRE(grid != NULL);
    REQUIRE(clay_voxel_palette_add(grid, white, &index) == CLAY_OK);

    REQUIRE(clay_voxel_set_brush(grid, centre, &cube5, index) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    REQUIREF(occupied == 125, "a size-5 cube brush filled %zu cells, expected 125", occupied);

    /* a sphere of the same size is a subset of the cube */
    REQUIRE(clay_voxel_erase_brush(grid, centre, &cube5) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied == 0);
    REQUIRE(clay_voxel_set_brush(grid, centre, &sphere5, index) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    REQUIREF(occupied > 0 && occupied < 125, "a size-5 sphere brush filled %zu cells", occupied);

    /* Painting recolors what the footprint covers and leaves everything else,
     * so it is checked with an index the stamp above did not use: painting
     * with the same one is unobservable by construction. */
    const int32_t under_brush[3] = {1, 0, 0};
    int32_t recolored = -1, painted = -1, untouched = -1;
    float grey[3] = {0.5f, 0.5f, 0.5f};
    REQUIRE(clay_voxel_set(grid, outside, index) == CLAY_OK);
    REQUIRE(clay_voxel_palette_add(grid, grey, &recolored) == CLAY_OK && recolored != index);
    REQUIRE(clay_voxel_paint_brush(grid, centre, &sphere5, recolored) == CLAY_OK);
    REQUIRE(clay_voxel_get(grid, under_brush, &painted) == CLAY_OK);
    REQUIREF(painted == recolored, "a cell inside the painted footprint reads %d, expected %d",
             painted, recolored);
    REQUIRE(clay_voxel_get(grid, outside, &untouched) == CLAY_OK);
    REQUIREF(untouched == index, "paint_brush reached a cell outside its footprint (%d)",
             untouched);
    REQUIRE(clay_voxel_erase(grid, outside) == CLAY_OK);
    REQUIRE(clay_voxel_paint_brush(grid, centre, &sphere5, index) == CLAY_OK);

    /* a soft brush touches fewer cells than the hard one it softens */
    clay_voxel_grid* soft = clay_voxel_grid_create(0.1f);
    REQUIRE(soft != NULL);
    clay_brush_params faded = sphere5;
    faded.falloff = CLAY_BRUSH_FALLOFF_LINEAR;
    faded.seed = 7;
    size_t soft_count = 0;
    REQUIRE(clay_voxel_set_brush(soft, centre, &faded, 1) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(soft, &soft_count) == CLAY_OK);
    REQUIREF(soft_count > 0 && soft_count < occupied,
             "a linear falloff filled %zu cells, the hard brush %zu", soft_count, occupied);
    REQUIRE(clay_voxel_grid_destroy(soft) == CLAY_OK);

    /* the four verbs run over the same footprint */
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float zero[3] = {0.0f, 0.0f, 0.0f};
    REQUIRE(clay_voxel_sculpt_smooth(grid, centre, &sphere5) == CLAY_OK);
    REQUIRE(clay_voxel_sculpt_inflate(grid, centre, &sphere5, 1) == CLAY_OK);
    REQUIRE(clay_voxel_sculpt_inflate(grid, centre, &sphere5, -1) == CLAY_OK);
    REQUIRE(clay_voxel_sculpt_flatten(grid, centre, &sphere5, normal, 0.0f) == CLAY_OK);
    REQUIRE(clay_voxel_sculpt_flatten(grid, centre, &sphere5, zero, 0.0f) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_sculpt_pinch(grid, centre, &sphere5) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied > 0);

    /* flood select through the size-query pattern */
    REQUIRE(clay_voxel_flood_select(grid, centre, 1, NULL, &selected) == CLAY_OK);
    REQUIREF(selected == occupied, "flood select reports %zu cells of %zu occupied", selected,
             occupied);
    capacity = selected - 1;
    int32_t* cells = (int32_t*)malloc(selected * 3 * sizeof(int32_t));
    REQUIRE(cells != NULL);
    REQUIRE(clay_voxel_flood_select(grid, centre, 1, cells, &capacity) ==
            CLAY_ERROR_BUFFER_TOO_SMALL);
    REQUIRE(capacity == selected); /* the short call reports what it needs */
    REQUIRE(clay_voxel_flood_select(grid, centre, 1, cells, &capacity) == CLAY_OK);
    REQUIRE(capacity == selected);
    int32_t seeded = -1;
    REQUIRE(clay_voxel_get(grid, cells, &seeded) == CLAY_OK && seeded != 0);
    free(cells);
    /* an empty seed selects nothing, which is not an error */
    REQUIRE(clay_voxel_flood_select(grid, outside, 1, NULL, &selected) == CLAY_OK);
    REQUIRE(selected == 0);

    /* the step field is a bound: negative inside, positive outside */
    float points[6] = {0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 100.0f};
    float field[2] = {0.0f, 0.0f};
    REQUIRE(clay_voxel_sample_step_field(grid, points, 2, field) == CLAY_OK);
    REQUIRE(field[0] < 0.0f && field[1] > 0.0f);

    clay_mesh* mesh = NULL;
    REQUIRE(clay_voxel_mesh(grid, &mesh) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(mesh) > 0);
    REQUIRE(clay_mesh_index_count(mesh) % 3 == 0);
    REQUIRE(clay_mesh_colors(mesh) != NULL);
    clay_mesh_destroy(mesh);

    /* an empty grid meshes to nothing rather than failing */
    clay_voxel_grid* bare = clay_voxel_grid_create(0.1f);
    clay_mesh* empty = NULL;
    REQUIRE(bare != NULL);
    REQUIRE(clay_voxel_mesh(bare, &empty) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(empty) == 0);
    clay_mesh_destroy(empty);
    REQUIRE(clay_voxel_grid_destroy(bare) == CLAY_OK);

    REQUIRE(clay_voxel_grid_destroy(grid) == CLAY_OK);
    return 0;
}

/* The ownership rule: a document layer's grid is borrowed, survives a save and
 * load, and refuses to be destroyed by the caller. */
static int check_voxel_ownership(void) {
    const int32_t cell[3] = {1, 2, 3};
    float green[3] = {0.0f, 1.0f, 0.0f};
    int32_t index = -1, read = -1;
    clay_layer_id layer = 0, found_layer = 0;
    clay_voxel_grid* grid = NULL;
    clay_voxel_grid* again = NULL;

    clay_document* doc = clay_document_create();
    REQUIRE(doc != NULL);
    REQUIRE(clay_document_add_voxel_layer(doc, "voxels", 0.0f, &layer, &grid) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_add_voxel_layer(doc, "voxels", 0.2f, &layer, &grid) == CLAY_OK);
    REQUIRE(grid != NULL);

    /* the borrowed handle is the document's: destroying it is refused, and the
     * document keeps working afterwards */
    REQUIRE(clay_voxel_grid_destroy(grid) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_palette_add(grid, green, &index) == CLAY_OK);
    REQUIRE(clay_voxel_set(grid, cell, index) == CLAY_OK);

    /* looking the layer up again borrows the same grid */
    REQUIRE(clay_document_voxel_layer(doc, "voxels", &found_layer, &again) == CLAY_OK);
    REQUIRE(found_layer == layer && again == grid);
    REQUIRE(clay_document_voxel_layer(doc, "nothing", NULL, &again) == CLAY_ERROR_NOT_FOUND);

    /* an SDF layer is not a voxel layer, and vice versa */
    clay_layer_id sdf = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &sdf) == CLAY_OK);
    REQUIRE(clay_document_voxel_layer(doc, "body", NULL, &again) == CLAY_ERROR_NOT_FOUND);

    /* edits through the borrow are what the document saves */
    REQUIRE(clay_document_save(doc, "c_api_smoke_voxels.clayspace") == CLAY_OK);
    clay_document* loaded = NULL;
    REQUIRE(clay_document_load("c_api_smoke_voxels.clayspace", &loaded) == CLAY_OK);
    REQUIRE(clay_document_voxel_layer(loaded, "voxels", NULL, &again) == CLAY_OK);
    REQUIRE(clay_voxel_get(again, cell, &read) == CLAY_OK);
    REQUIREF(read == index, "the reloaded layer reads %d at the edited cell, wrote %d", read,
             index);

    clay_document_destroy(loaded);
    clay_document_destroy(doc);
    return 0;
}

/* Everything the voxel surface rejects, including the versioned brush. */
static int check_voxel_rejections(void) {
    const int32_t cell[3] = {0, 0, 0};
    float rgb[3] = {1.0f, 1.0f, 1.0f};
    clay_brush_params brush = cube_brush(3);
    clay_voxel_grid* grid = clay_voxel_grid_create(0.1f);
    REQUIRE(grid != NULL);

    /* null handles are arguments, not crashes */
    REQUIRE(clay_voxel_grid_destroy(NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_size(NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_set(NULL, cell, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_set(grid, NULL, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_set_brush(grid, cell, NULL, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_flood_select(grid, cell, 1, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_palette_add(grid, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_rasterize(grid, NULL, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_add_voxel_layer(NULL, NULL, 0.1f, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_document_voxel_layer(NULL, NULL, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);

    /* out-of-range palette indices are rejected, not truncated to a byte */
    REQUIRE(clay_voxel_set(grid, cell, 256) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_set(grid, cell, -1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_palette_color(grid, 256, rgb) == CLAY_ERROR_INVALID_ARGUMENT);

    /* the brush descriptor obeys the prefix rule, and its own fields */
    clay_brush_params unsized = brush;
    unsized.struct_size = 0;
    REQUIRE(clay_voxel_set_brush(grid, cell, &unsized, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    unsized.struct_size = 4; /* shorter than any layout that ever shipped */
    REQUIRE(clay_voxel_set_brush(grid, cell, &unsized, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    unsized.struct_size = 0x3D23D70A; /* the bits of 0.04f: not a struct size */
    REQUIRE(clay_voxel_set_brush(grid, cell, &unsized, 1) == CLAY_ERROR_INVALID_ARGUMENT);

    clay_brush_params bad = brush;
    bad.size = 0;
    REQUIRE(clay_voxel_set_brush(grid, cell, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    bad = brush;
    bad.shape = 99;
    REQUIRE(clay_voxel_set_brush(grid, cell, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    bad = brush;
    bad.falloff = 99;
    REQUIRE(clay_voxel_erase_brush(grid, cell, &bad) == CLAY_ERROR_INVALID_ARGUMENT);
    /* a bad size is reported before a bad shape, as it is in the Python
     * bindings, so the same mistake reads the same way through both */
    bad = brush;
    bad.size = -1;
    bad.shape = 99;
    REQUIRE(clay_voxel_set_brush(grid, cell, &bad, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(strstr(clay_last_error(), "size") != NULL);

    /* a caller from the future: the tail this build cannot name is ignored */
    struct {
        clay_brush_params brush;
        float tail[3];
    } future;
    memset(&future, 0, sizeof future);
    future.brush = brush;
    future.brush.struct_size = (uint32_t)sizeof future;
    REQUIRE(clay_voxel_set_brush(grid, cell, &future.brush, 1) == CLAY_OK);

    /* No field of this descriptor has a default, so a zeroed-but-sized brush
     * is rejected on the first field whose value means nothing — strength —
     * rather than silently stamping everything or silently stamping nothing.
     * A destructive verb makes the difference matter: at full coverage the
     * erase below would take the whole footprint away. */
    clay_brush_params zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    zeroed.struct_size = (uint32_t)sizeof zeroed;
    zeroed.size = 3;
    size_t stamped = 0, left = 0;
    clay_voxel_grid* a = clay_voxel_grid_create(0.1f);
    REQUIRE(a != NULL);
    REQUIRE(clay_voxel_set_brush(a, cell, &zeroed, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(strstr(clay_last_error(), "strength") != NULL);
    REQUIRE(clay_voxel_occupied_count(a, &stamped) == CLAY_OK && stamped == 0);
    zeroed.strength = -1.0f;
    REQUIRE(clay_voxel_set_brush(a, cell, &zeroed, 1) == CLAY_ERROR_INVALID_ARGUMENT);
    zeroed.strength = 1.0f;
    REQUIRE(clay_voxel_set_brush(a, cell, &zeroed, 1) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(a, &stamped) == CLAY_OK);
    REQUIREF(stamped == 27, "a size-3 cube brush filled %zu cells, expected 27", stamped);
    zeroed.strength = 0.0f;
    REQUIRE(clay_voxel_erase_brush(a, cell, &zeroed) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_occupied_count(a, &left) == CLAY_OK);
    REQUIREF(left == stamped, "a rejected erase took %zu of %zu cells", stamped - left, stamped);
    REQUIRE(clay_voxel_grid_destroy(a) == CLAY_OK);

    REQUIRE(clay_voxel_grid_destroy(grid) == CLAY_OK);
    return 0;
}

/* Rasterizing an SDF document into a voxel layer of the same document. */
static int check_voxel_rasterize(clay_document* doc) {
    float small[3] = {-0.5f, -0.5f, -0.5f}, big[3] = {0.5f, 0.5f, 0.5f};
    clay_voxel_grid* grid = NULL;
    size_t occupied = 0;

    clay_document* empty = clay_document_create();
    clay_voxel_grid* bare = clay_voxel_grid_create(0.1f);
    REQUIRE(empty != NULL && bare != NULL);
    REQUIRE(clay_voxel_rasterize(bare, empty, NULL, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    /* half a region is not a region */
    REQUIRE(clay_voxel_rasterize(bare, empty, small, NULL) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_occupied_count(bare, &occupied) == CLAY_OK && occupied == 0);
    REQUIRE(clay_voxel_grid_destroy(bare) == CLAY_OK);
    clay_document_destroy(empty);

    REQUIRE(clay_document_add_voxel_layer(doc, "rasterized", 0.05f, NULL, &grid) == CLAY_OK);
    REQUIRE(clay_voxel_rasterize(grid, doc, small, big) == CLAY_OK);
    REQUIRE(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK);
    REQUIREF(occupied > 0, "rasterizing the smoke document filled %zu cells", occupied);
    /* the field's colors arrive as palette entries */
    size_t palette = 0;
    REQUIRE(clay_voxel_palette_size(grid, &palette) == CLAY_OK);
    REQUIRE(palette > 1);
    return 0;
}

/* -- evaluation, meshing, file I/O ----------------------------------------- */

static int check_evaluation(clay_document* doc) {
    float pts[9] = {0, 0, 0, 3, 0, 0, 0.9f, 0, 0};
    float dist[3] = {99, 99, 99};
    float colors[9];
    REQUIRE(clay_eval_points(doc, NULL, pts, 3, dist, colors) == CLAY_OK);
    REQUIREF(dist[0] < -0.9f && dist[0] > -1.1f, "at the sphere's centre d = %g, expected -1",
             (double)dist[0]);
    REQUIREF(dist[1] > 1.0f, "outside everything d = %g, expected > 1", (double)dist[1]);
    REQUIREF(dist[2] < 0.0f, "inside the blend region d = %g, expected < 0", (double)dist[2]);
    REQUIRE(clay_eval_points(doc, "no_such_backend", pts, 3, dist, NULL) ==
            CLAY_ERROR_NOT_FOUND);

    /* raycast */
    float origin[3] = {0, 0, -5}, direction[3] = {0, 0, 1};
    int32_t hit = 0;
    float t = 0, pos[3], normal[3];
    REQUIRE(clay_raycast(doc, origin, direction, &hit, &t, pos, normal) == CLAY_OK);
    REQUIRE(hit == 1);
    REQUIREF(t > 3.9f && t < 4.1f, "the ray hit at t = %g, expected 4", (double)t);
    REQUIRE(normal[2] < -0.9f);
    return 0;
}

/* gradients, the batch raycast and the step scale: the entry points a viewer
 * calls per frame rather than per edit */
static int check_batch_evaluation(clay_document* doc) {
    /* off the sphere's centre on purpose: the gradient at the exact centre of
     * a ball is not a direction, and normalizing it gives NaN */
    float pts[9] = {0.5f, 0, 0, 3, 0, 0, 0.4f, 0.3f, 0.2f};
    float gradients[9];
    memset(gradients, 0, sizeof gradients);
    REQUIRE(clay_eval_gradients(doc, NULL, pts, 3, gradients) == CLAY_OK);
    for (int i = 0; i < 3; ++i) { /* a gradient is a direction: unit length */
        float g[3] = {gradients[i * 3], gradients[i * 3 + 1], gradients[i * 3 + 2]};
        float len = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        REQUIREF(len > 0.99f && len < 1.01f, "gradient %d has length %g, expected 1", i,
                 (double)len);
    }
    REQUIRE(clay_eval_gradients(doc, "no_such_backend", pts, 3, gradients) ==
            CLAY_ERROR_NOT_FOUND);

    float scale = 0.0f;
    REQUIRE(clay_safe_step_scale(doc, &scale) == CLAY_OK);
    REQUIREF(scale > 0.0f && scale <= 1.0f, "the safe step scale is %g, expected (0, 1]",
             (double)scale);

    /* two rays: one down +Z at the sphere, one that leaves the scene. The
     * second direction is not unit length on purpose — the batch form
     * normalizes, so t stays a world distance. */
    float rays[12] = {0, 0, -5, 0, 0, 1, 0, 40, -5, 0, 0, 3};
    int32_t hits[2] = {0, 0};
    float t[2] = {0, 0};
    float positions[6], normals[6];
    REQUIRE(clay_raycast_many(doc, rays, 2, hits, t, positions, normals) == CLAY_OK);
    REQUIRE(hits[0] == 1 && hits[1] == 0);
    REQUIREF(t[0] > 3.9f && t[0] < 4.1f, "the batch ray hit at t = %g, expected 4",
             (double)t[0]);
    REQUIRE(normals[2] < -0.9f);
    /* every out buffer is optional */
    REQUIRE(clay_raycast_many(doc, rays, 2, hits, NULL, NULL, NULL) == CLAY_OK);
    /* a ray with no direction has nowhere to go */
    float degenerate[6] = {0, 0, -5, 0, 0, 0};
    REQUIRE(clay_raycast_many(doc, degenerate, 1, hits, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    return 0;
}

/* One layer's own field, which is not the document's: what the layer beside it
 * holds cannot change what this one reads. */
static int check_layer_evaluation(void) {
    clay_document* doc = clay_document_create();
    clay_layer_id body = 0, beside = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &body) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(doc, "beside", &beside) == CLAY_OK);

    clay_item_desc sphere;
    memset(&sphere, 0, sizeof sphere);
    sphere.struct_size = (uint32_t)sizeof sphere;
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 1.0f;
    sphere.rotation[3] = 1.0f;
    sphere.scale = 1.0f;
    sphere.color[0] = 0.8f;
    REQUIRE(clay_add_item(doc, body, &sphere, NULL) == CLAY_OK);
    clay_item_desc other = sphere;
    other.params[0] = 0.5f;
    other.position[0] = 2.0f;
    REQUIRE(clay_add_item(doc, beside, &other, NULL) == CLAY_OK);

    /* a point inside the second layer's sphere: material to the document,
     * empty space to the first layer on its own */
    float at[3] = {2.0f, 0, 0};
    float whole = 0.0f, one_layer = 0.0f, colors[3];
    REQUIRE(clay_eval_points(doc, NULL, at, 1, &whole, NULL) == CLAY_OK);
    REQUIRE(clay_layer_eval_points(doc, body, NULL, at, 1, &one_layer, colors) == CLAY_OK);
    REQUIREF(whole < 0.0f, "the whole document reads %g at (2, 0, 0), expected inside",
             (double)whole);
    REQUIREF(one_layer > 0.0f, "the body layer reads %g at (2, 0, 0), expected empty space",
             (double)one_layer);

    float gradient[3] = {0, 0, 0};
    REQUIRE(clay_layer_eval_gradients(doc, body, NULL, at, 1, gradient) == CLAY_OK);
    REQUIREF(gradient[0] > 0.9f, "the layer gradient at (2, 0, 0) points (%g, %g, %g), "
             "expected +x", (double)gradient[0], (double)gradient[1], (double)gradient[2]);

    float scale = 0.0f;
    REQUIRE(clay_layer_safe_step_scale(doc, body, &scale) == CLAY_OK);
    REQUIREF(scale > 0.0f && scale <= 1.0f, "the layer step scale is %g, expected (0, 1]",
             (double)scale);

    /* a layer that is not there is an error, not an empty field */
    REQUIRE(clay_layer_eval_points(doc, 99999, NULL, at, 1, &one_layer, NULL) ==
            CLAY_ERROR_NOT_FOUND);
    REQUIRE(clay_layer_safe_step_scale(doc, 99999, &scale) == CLAY_ERROR_NOT_FOUND);
    clay_document_destroy(doc);
    return 0;
}

/* -- picking --------------------------------------------------------------- */

/* Attribution, snapping and the two bounds queries, over a document whose two
 * layers are far enough apart to attribute unambiguously. */
static int check_picking(void) {
    clay_document* doc = clay_document_create();
    clay_layer_id ball = 0, slab = 0;
    clay_node_id ball_node = 0, slab_node = 0;
    REQUIRE(clay_add_sdf_layer(doc, "ball", &ball) == CLAY_OK);
    REQUIRE(clay_add_sdf_layer(doc, "slab", &slab) == CLAY_OK);

    clay_item_desc sphere;
    memset(&sphere, 0, sizeof sphere);
    sphere.struct_size = (uint32_t)sizeof sphere;
    sphere.prim = CLAY_PRIM_SPHERE;
    sphere.params[0] = 1.0f;
    sphere.rotation[3] = 1.0f;
    sphere.scale = 1.0f;
    REQUIRE(clay_add_item(doc, ball, &sphere, &ball_node) == CLAY_OK);
    clay_item_desc box = sphere;
    box.prim = CLAY_PRIM_BOX;
    box.params[0] = box.params[1] = box.params[2] = 0.5f;
    box.position[0] = 3.0f;
    REQUIRE(clay_add_item(doc, slab, &box, &slab_node) == CLAY_OK);

    /* a ray down each shape attributes to its own layer and item */
    float dir[3] = {0, 0, 1};
    float at_ball[3] = {0, 0, -5}, at_slab[3] = {3, 0, -5};
    int32_t hit = 0;
    float t = 0, position[3], normal[3];
    clay_layer_id layer = 424242;
    clay_node_id node = 424242;
    REQUIRE(clay_raycast_attributed(doc, at_ball, dir, &hit, &t, position, normal, &layer,
                                    &node) == CLAY_OK);
    REQUIRE(hit == 1);
    REQUIREF(layer == ball && node == ball_node,
             "the sphere attributed to layer %u node %u, expected layer %u node %u", layer,
             node, ball, ball_node);
    REQUIRE(clay_raycast_attributed(doc, at_slab, dir, &hit, &t, position, normal, &layer,
                                    &node) == CLAY_OK);
    REQUIRE(hit == 1);
    REQUIREF(layer == slab && node == slab_node,
             "the box attributed to layer %u node %u, expected layer %u node %u", layer, node,
             slab, slab_node);

    /* a miss attributes nothing, and 0 is never a layer or a node id */
    float away[3] = {0, 40, -5};
    REQUIRE(clay_raycast_attributed(doc, away, dir, &hit, NULL, NULL, NULL, &layer, &node) ==
            CLAY_OK);
    REQUIRE(hit == 0 && layer == 0 && node == 0);
    float no_direction[3] = {0, 0, 0};
    REQUIRE(clay_raycast_attributed(doc, away, no_direction, &hit, NULL, NULL, NULL, NULL,
                                    NULL) == CLAY_ERROR_INVALID_ARGUMENT);

    /* snapping: a point off the sphere lands on it, normal pointing outward */
    float points[6] = {0, 2.0f, 0, 0, -2.0f, 0};
    float snapped[6], snapped_normals[6];
    int32_t ok[2] = {-1, -1};
    REQUIRE(clay_snap_to_surface(doc, points, 2, snapped, snapped_normals, ok) == CLAY_OK);
    REQUIRE(ok[0] == 1 && ok[1] == 1);
    REQUIREF(snapped[1] > 0.99f && snapped[1] < 1.01f,
             "the point above the sphere snapped to y = %g, expected 1", (double)snapped[1]);
    REQUIRE(snapped_normals[1] > 0.9f);
    REQUIREF(snapped[4] < -0.99f && snapped[4] > -1.01f,
             "the point below the sphere snapped to y = %g, expected -1", (double)snapped[4]);
    REQUIRE(snapped_normals[4] < -0.9f);
    REQUIRE(clay_snap_to_surface(doc, points, 2, snapped, NULL, NULL) == CLAY_OK);

    /* bounds: the box layer's box, then the same for that one node */
    float lo[3], hi[3];
    int32_t has_bounds = 0;
    REQUIRE(clay_layer_bounds(doc, slab, lo, hi, &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 1);
    REQUIREF(lo[0] > 2.4f && lo[0] < 2.6f && hi[0] > 3.4f && hi[0] < 3.6f,
             "the box layer spans x [%g, %g], expected [2.5, 3.5]", (double)lo[0],
             (double)hi[0]);
    float sel_lo[3], sel_hi[3];
    REQUIRE(clay_layer_selection_bounds(doc, slab, &slab_node, 1, sel_lo, sel_hi,
                                        &has_bounds) == CLAY_OK);
    REQUIRE(has_bounds == 1);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(sel_lo[i] == lo[i]);
        REQUIRE(sel_hi[i] == hi[i]);
    }
    /* an empty selection is an answer, a missing layer is not found */
    has_bounds = 1;
    REQUIRE(clay_layer_selection_bounds(doc, slab, NULL, 0, sel_lo, sel_hi, &has_bounds) ==
            CLAY_OK);
    REQUIRE(has_bounds == 0);
    REQUIRE(clay_layer_bounds(doc, 99999, lo, hi, &has_bounds) == CLAY_ERROR_NOT_FOUND);
    REQUIRE(clay_layer_selection_bounds(doc, 99999, &slab_node, 1, lo, hi, &has_bounds) ==
            CLAY_ERROR_NOT_FOUND);
    clay_document_destroy(doc);
    return 0;
}

/* Cell picking, the six entry faces clay_voxel_face names, and the build
 * plane a click resolves against when the ray hits nothing. */
static int check_voxel_picking(void) {
    struct shot {
        float origin[3];
        float dir[3];
        int32_t face;
        int32_t cell[3];
        int32_t adjacent[3];
    };
    static const struct shot shots[6] = {
        {{2.0f, 0.15f, 0.15f}, {-1, 0, 0}, CLAY_VOXEL_FACE_POS_X, {3, 1, 1}, {4, 1, 1}},
        {{-2.0f, 0.15f, 0.15f}, {1, 0, 0}, CLAY_VOXEL_FACE_NEG_X, {0, 1, 1}, {-1, 1, 1}},
        {{0.15f, 2.0f, 0.15f}, {0, -1, 0}, CLAY_VOXEL_FACE_POS_Y, {1, 3, 1}, {1, 4, 1}},
        {{0.15f, -2.0f, 0.15f}, {0, 1, 0}, CLAY_VOXEL_FACE_NEG_Y, {1, 0, 1}, {1, -1, 1}},
        {{0.15f, 0.15f, 2.0f}, {0, 0, -1}, CLAY_VOXEL_FACE_POS_Z, {1, 1, 3}, {1, 1, 4}},
        {{0.15f, 0.15f, -2.0f}, {0, 0, 1}, CLAY_VOXEL_FACE_NEG_Z, {1, 1, 0}, {1, 1, -1}}};
    clay_voxel_grid* grid = clay_voxel_grid_create(0.1f);
    REQUIRE(grid != NULL);
    int32_t index = 0;
    float rgb[3] = {0.5f, 0.5f, 0.5f};
    int32_t lo[3] = {0, 0, 0}, hi[3] = {3, 3, 3};
    REQUIRE(clay_voxel_palette_add(grid, rgb, &index) == CLAY_OK);
    REQUIRE(clay_voxel_fill_box(grid, lo, hi, index) == CLAY_OK);

    for (size_t i = 0; i < 6; ++i) {
        int32_t hit = 0, face = -1, cell[3] = {0, 0, 0}, adjacent[3] = {0, 0, 0};
        float t = 0;
        REQUIRE(clay_voxel_raycast(grid, shots[i].origin, shots[i].dir, &hit, cell, &face,
                                   adjacent, &t) == CLAY_OK);
        REQUIRE(hit == 1);
        REQUIREF(face == shots[i].face, "shot %zu entered through face %d, expected %d", i,
                 face, shots[i].face);
        for (int c = 0; c < 3; ++c) {
            REQUIREF(cell[c] == shots[i].cell[c],
                     "shot %zu hit cell (%d, %d, %d), expected (%d, %d, %d)", i, cell[0],
                     cell[1], cell[2], shots[i].cell[0], shots[i].cell[1], shots[i].cell[2]);
            REQUIREF(adjacent[c] == shots[i].adjacent[c],
                     "shot %zu places at (%d, %d, %d), expected (%d, %d, %d)", i, adjacent[0],
                     adjacent[1], adjacent[2], shots[i].adjacent[0], shots[i].adjacent[1],
                     shots[i].adjacent[2]);
        }
        REQUIRE(t > 0.0f);
    }

    /* a ray that leaves the block reports a miss, not an error */
    float away[3] = {5, 5, 5}, outward[3] = {1, 1, 1};
    int32_t hit = 1;
    REQUIRE(clay_voxel_raycast(grid, away, outward, &hit, NULL, NULL, NULL, NULL) == CLAY_OK);
    REQUIRE(hit == 0);

    /* the build plane: y is always the plane's own cell index */
    float origin[3] = {0.55f, 2.0f, 0.35f}, down[3] = {0, -1, 0};
    int32_t cell[3] = {0, 0, 0};
    REQUIRE(clay_voxel_build_plane_pick(grid, origin, down, 0, &hit, cell) == CLAY_OK);
    REQUIRE(hit == 1);
    REQUIREF(cell[0] == 5 && cell[1] == 0 && cell[2] == 3,
             "the build plane resolved to (%d, %d, %d), expected (5, 0, 3)", cell[0], cell[1],
             cell[2]);
    REQUIRE(clay_voxel_build_plane_pick(grid, origin, down, 2, &hit, cell) == CLAY_OK);
    REQUIRE(hit == 1 && cell[1] == 2);
    /* parallel to the plane, and with the plane behind: both are misses */
    float sideways[3] = {1, 0, 0}, up[3] = {0, 1, 0};
    hit = 1;
    REQUIRE(clay_voxel_build_plane_pick(grid, origin, sideways, 0, &hit, cell) == CLAY_OK);
    REQUIRE(hit == 0);
    hit = 1;
    REQUIRE(clay_voxel_build_plane_pick(grid, origin, up, 0, &hit, cell) == CLAY_OK);
    REQUIRE(hit == 0);

    /* rejections */
    float no_direction[3] = {0, 0, 0};
    REQUIRE(clay_voxel_raycast(grid, origin, no_direction, &hit, NULL, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_raycast(grid, NULL, down, &hit, NULL, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_raycast(grid, origin, down, NULL, NULL, NULL, NULL, NULL) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_build_plane_pick(NULL, origin, down, 0, &hit, cell) ==
            CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(clay_voxel_grid_destroy(grid) == CLAY_OK);
    return 0;
}

/* Mesher selection, and the gate the experimental one sits behind. */
static int check_meshers(clay_document* doc) {
    clay_mesh_params mp;
    memset(&mp, 0, sizeof mp);
    mp.struct_size = (uint32_t)sizeof mp;
    mp.resolution = 32;

    mp.mesher = CLAY_MESHER_MARCHING;
    clay_mesh* marching = NULL;
    REQUIRE(clay_document_mesh(doc, &mp, &marching) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(marching) > 0);

    mp.mesher = CLAY_MESHER_NETS;
    clay_mesh* nets = NULL;
    REQUIRE(clay_document_mesh(doc, &mp, &nets) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(nets) > 0);
    /* surface nets is the preview path: one vertex per crossing cell, so it
     * is always the smaller mesh at the same voxel size */
    REQUIREF(clay_mesh_vertex_count(nets) < clay_mesh_vertex_count(marching),
             "surface nets gave %zu vertices, marching %zu", clay_mesh_vertex_count(nets),
             clay_mesh_vertex_count(marching));

    /* dual contouring is refused without the opt-in and reachable with it */
    mp.mesher = CLAY_MESHER_DUAL_CONTOURING;
    clay_mesh* dc = NULL;
    REQUIRE(clay_document_mesh(doc, &mp, &dc) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(dc == NULL);
    REQUIRE(strstr(clay_last_error(), "experimental") != NULL);
    mp.experimental = 1;
    REQUIRE(clay_document_mesh(doc, &mp, &dc) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(dc) > 0);

    /* an unknown mesher is rejected rather than meshed with the default */
    mp.mesher = 99;
    clay_mesh* rejected = NULL;
    REQUIRE(clay_document_mesh(doc, &mp, &rejected) == CLAY_ERROR_INVALID_ARGUMENT);
    REQUIRE(rejected == NULL);

    clay_mesh_destroy(marching);
    clay_mesh_destroy(nets);
    clay_mesh_destroy(dc);
    return 0;
}

static int check_meshing(clay_document* doc) {
    clay_mesh_params mp;
    memset(&mp, 0, sizeof mp);
    mp.struct_size = (uint32_t)sizeof mp;
    mp.resolution = 48;
    clay_mesh* mesh = NULL;
    REQUIRE(clay_document_mesh(doc, &mp, &mesh) == CLAY_OK);
    REQUIRE(clay_mesh_vertex_count(mesh) > 100);
    REQUIRE(clay_mesh_index_count(mesh) % 3 == 0);
    REQUIRE(clay_mesh_positions(mesh) != NULL);
    REQUIRE(clay_mesh_colors(mesh) != NULL);
    REQUIRE(clay_mesh_indices(mesh) != NULL);
    int32_t watertight = 0, manifold = 0;
    REQUIRE(clay_mesh_validate(mesh, &watertight, &manifold) == CLAY_OK);
    REQUIRE(watertight == 1 && manifold == 1);
    REQUIRE(clay_mesh_save(mesh, "c_api_smoke.obj") == CLAY_OK);
    REQUIRE(clay_mesh_save(mesh, "c_api_smoke.xyz") == CLAY_ERROR_UNSUPPORTED);
    clay_mesh_destroy(mesh);
    return 0;
}

static int check_round_trip(clay_document* doc) {
    float pts[9] = {0, 0, 0, 3, 0, 0, 0.9f, 0, 0};
    float before[3], after[3];
    REQUIRE(clay_eval_points(doc, NULL, pts, 3, before, NULL) == CLAY_OK);
    REQUIRE(clay_document_save(doc, "c_api_smoke.clayspace") == CLAY_OK);
    clay_document* loaded = NULL;
    REQUIRE(clay_document_load("c_api_smoke.clayspace", &loaded) == CLAY_OK);
    REQUIRE(clay_eval_points(loaded, NULL, pts, 3, after, NULL) == CLAY_OK);
    for (int i = 0; i < 3; ++i) /* identical evaluation after reload */
        REQUIREF(after[i] == before[i], "sample %d reads %g after reload, %g before", i,
                 (double)after[i], (double)before[i]);
    clay_document_destroy(loaded);
    REQUIRE(clay_document_load("missing_file.clayspace", &loaded) == CLAY_ERROR_NOT_FOUND);
    REQUIRE(loaded == NULL);
    return 0;
}

int main(void) {
    int32_t major = -1, minor = -1, patch = -1;
    clay_version(&major, &minor, &patch);
    /* the init-time check the header documents: majors must agree, and while
     * the major is 0 so must the minors, because the ABI may still break on a
     * minor bump (0.2.0 did, on the two descriptor structs) */
    REQUIRE(major == CLAY_ABI_MAJOR && patch >= 0);
    REQUIRE(major != 0 || minor == CLAY_ABI_MINOR);
    if (check_backends() != 0) return 1;

    clay_document* doc = clay_document_create();
    REQUIRE(doc != NULL);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);

    if (check_flat_path(doc, layer) != 0) return 1;
    if (check_item_builder() != 0) return 1;
    if (check_builder_rejections() != 0) return 1;
    if (check_transition_pairing() != 0) return 1;
    if (check_builder_payloads() != 0) return 1;
    if (check_large_payloads() != 0) return 1;
    if (check_every_primitive() != 0) return 1;
    if (check_struct_versioning() != 0) return 1;
    if (check_error_paths() != 0) return 1;
    if (check_voxel_edits() != 0) return 1;
    if (check_voxel_sculpting() != 0) return 1;
    if (check_voxel_ownership() != 0) return 1;
    if (check_voxel_rejections() != 0) return 1;
    if (check_voxel_picking() != 0) return 1;
    if (check_evaluation(doc) != 0) return 1;
    if (check_batch_evaluation(doc) != 0) return 1;
    if (check_layer_evaluation() != 0) return 1;
    if (check_picking() != 0) return 1;
    if (check_meshers(doc) != 0) return 1;
    if (check_meshing(doc) != 0) return 1;
    if (check_round_trip(doc) != 0) return 1;
    if (check_voxel_rasterize(doc) != 0) return 1;

    clay_document_destroy(doc);
    printf("c-api smoke: OK\n");
    return 0;
}
