/* C11 consumer smoke test (c-abi spec scenario "Pure C consumer"):
 * create a document, add a sphere edit, evaluate points, mesh, export OBJ,
 * save/load the document — every failure path returns an error code. */

#include <clay.h>

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

int main(void) {
    int32_t major = -1, minor = -1, patch = -1;
    clay_version(&major, &minor, &patch);
    REQUIRE(major == CLAY_ABI_MAJOR && minor >= 0 && patch >= 0);

    /* backend enumeration: size query first, then fill */
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

    /* build a small document */
    clay_document* doc = clay_document_create();
    REQUIRE(doc != NULL);
    clay_layer_id layer = 0;
    REQUIRE(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK);

    clay_item_desc sphere;
    memset(&sphere, 0, sizeof sphere);
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

    /* error paths */
    REQUIRE(clay_add_item(doc, 9999, &sphere, NULL) == CLAY_ERROR_NOT_FOUND);
    REQUIRE(strlen(clay_last_error()) > 0);
    REQUIRE(clay_remove_node(doc, layer, 424242) == CLAY_ERROR_NOT_FOUND);
    clay_item_desc bad = sphere;
    bad.prim = 999;
    REQUIRE(clay_add_item(doc, layer, &bad, NULL) == CLAY_ERROR_INVALID_ARGUMENT);

    /* evaluate points */
    float pts[9] = {0, 0, 0, 3, 0, 0, 0.9f, 0, 0};
    float dist[3] = {99, 99, 99};
    float colors[9];
    REQUIRE(clay_eval_points(doc, NULL, pts, 3, dist, colors) == CLAY_OK);
    REQUIRE(dist[0] < -0.9f && dist[0] > -1.1f); /* center of the sphere */
    REQUIRE(dist[1] > 1.0f);                     /* outside everything */
    REQUIRE(dist[2] < 0.0f);                     /* inside the blend region */
    REQUIRE(clay_eval_points(doc, "no_such_backend", pts, 3, dist, NULL) ==
            CLAY_ERROR_NOT_FOUND);

    /* raycast */
    float origin[3] = {0, 0, -5}, direction[3] = {0, 0, 1};
    int32_t hit = 0;
    float t = 0, pos[3], normal[3];
    REQUIRE(clay_raycast(doc, origin, direction, &hit, &t, pos, normal) == CLAY_OK);
    REQUIRE(hit == 1);
    REQUIRE(t > 3.9f && t < 4.1f);
    REQUIRE(normal[2] < -0.9f);

    /* mesh + validate + export */
    clay_mesh_params mp;
    memset(&mp, 0, sizeof mp);
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

    /* save/load round trip */
    REQUIRE(clay_document_save(doc, "c_api_smoke.clayspace") == CLAY_OK);
    clay_document* loaded = NULL;
    REQUIRE(clay_document_load("c_api_smoke.clayspace", &loaded) == CLAY_OK);
    float dist2[3];
    REQUIRE(clay_eval_points(loaded, NULL, pts, 3, dist2, NULL) == CLAY_OK);
    REQUIRE(dist2[0] == dist[0]); /* identical evaluation after reload */
    clay_document_destroy(loaded);
    REQUIRE(clay_document_load("missing_file.clayspace", &loaded) == CLAY_ERROR_NOT_FOUND);
    REQUIRE(loaded == NULL);

    clay_document_destroy(doc);
    printf("c-api smoke: OK\n");
    return 0;
}
