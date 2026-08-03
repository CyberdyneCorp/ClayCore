// C ABI implementation (c-abi spec): opaque handles over the C++ modules,
// thread-local error details, no exceptions cross this boundary (the core
// builds with -fno-exceptions on GCC/Clang).

#include "clay.h"

#include <cstring>
#include <string>

#include "clay/eval/backend.h"
#include "clay/io/clayspace.h"
#include "clay/io/mesh_io.h"
#include "clay/mesh/decimate.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/validate.h"
#include "clay/scene/bounds.h"
#include "clay/scene/tape.h"

using namespace clay;

namespace {

thread_local std::string g_last_error;

clay_result fail(clay_result code, std::string detail) {
    g_last_error = std::move(detail);
    return code;
}

clay_result from_io(const io::IoStatus& s) {
    g_last_error = s.detail;
    switch (s.error) {
        case io::IoError::Ok: return CLAY_OK;
        case io::IoError::FileNotFound: return CLAY_ERROR_NOT_FOUND;
        case io::IoError::ForwardVersion: return CLAY_ERROR_FORWARD_VERSION;
        case io::IoError::BudgetExceeded: return CLAY_ERROR_BUDGET_EXCEEDED;
        case io::IoError::Unsupported: return CLAY_ERROR_UNSUPPORTED;
        default: return CLAY_ERROR_IO;
    }
}

}  // namespace

struct clay_document {
    io::ClaySpaceDoc doc;
};

struct clay_mesh {
    mesh::Mesh data;
};

extern "C" {

void clay_version(int32_t* major, int32_t* minor, int32_t* patch) {
    if (major) *major = CLAY_ABI_MAJOR;
    if (minor) *minor = CLAY_ABI_MINOR;
    if (patch) *patch = CLAY_ABI_PATCH;
}

const char* clay_last_error(void) { return g_last_error.c_str(); }

clay_document* clay_document_create(void) { return new clay_document(); }

void clay_document_destroy(clay_document* doc) { delete doc; }

clay_result clay_document_save(const clay_document* doc, const char* path) {
    if (!doc || !path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or path");
    return from_io(io::save_clayspace_file(doc->doc, path));
}

clay_result clay_document_load(const char* path, clay_document** out_doc) {
    if (!path || !out_doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null path or out pointer");
    auto* doc = new clay_document();
    io::IoStatus s = io::load_clayspace_file(path, &doc->doc);
    if (!s.ok()) {
        delete doc;
        *out_doc = nullptr;
        return from_io(s);
    }
    *out_doc = doc;
    return CLAY_OK;
}

clay_result clay_add_sdf_layer(clay_document* doc, const char* name,
                               clay_layer_id* out_layer) {
    if (!doc || !name) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or name");
    scene::Layer& layer = doc->doc.document.add_sdf_layer(name);
    if (out_layer) *out_layer = layer.id;
    return CLAY_OK;
}

clay_result clay_add_item(clay_document* doc, clay_layer_id layer_id,
                          const clay_item_desc* item, clay_node_id* out_node) {
    if (!doc || !item) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document or item");
    if (item->prim < 0 || item->prim > CLAY_PRIM_PYRAMID)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unknown primitive type");
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");

    scene::Node node;
    node.prim.type = static_cast<scene::PrimType>(item->prim);
    std::memcpy(node.prim.params, item->params, sizeof node.prim.params);
    node.xform.position = kernel::cf3(item->position[0], item->position[1], item->position[2]);
    node.xform.rotation = math::Quat{item->rotation[0], item->rotation[1], item->rotation[2],
                                     item->rotation[3] == 0 && item->rotation[0] == 0 &&
                                             item->rotation[1] == 0 && item->rotation[2] == 0
                                         ? 1.0f
                                         : item->rotation[3]};
    node.xform.scale = item->scale <= 0 ? 1.0f : item->scale;
    node.op = static_cast<scene::Op>(item->op);
    node.blend = scene::Blend{static_cast<scene::BlendProfile>(item->blend), item->blend_k};
    node.rounding = item->rounding;
    node.color = kernel::cf3(item->color[0], item->color[1], item->color[2]);
    node.mirror = item->mirror != 0;
    scene::NodeId id = layer->sdf->insert(std::move(node));
    if (out_node) *out_node = id;
    return CLAY_OK;
}

clay_result clay_remove_node(clay_document* doc, clay_layer_id layer_id, clay_node_id node) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer || !layer->sdf) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    if (layer->sdf->remove(node).empty())
        return fail(CLAY_ERROR_NOT_FOUND, "node not found");
    return CLAY_OK;
}

clay_result clay_set_layer_mirror(clay_document* doc, clay_layer_id layer_id, int32_t axis_x,
                                  int32_t axis_y, int32_t axis_z, float mirror_k) {
    if (!doc) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null document");
    scene::Layer* layer = doc->doc.document.find_layer(layer_id);
    if (!layer) return fail(CLAY_ERROR_NOT_FOUND, "layer not found");
    layer->mirror_axes = static_cast<std::uint8_t>((axis_x ? scene::kMirrorX : 0) |
                                                   (axis_y ? scene::kMirrorY : 0) |
                                                   (axis_z ? scene::kMirrorZ : 0));
    layer->mirror_k = mirror_k;
    return CLAY_OK;
}

clay_result clay_list_backends(char* buffer, size_t* size) {
    if (!size) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null size");
    std::string names;
    for (eval::Backend* b : eval::Registry::instance().all()) {
        if (!names.empty()) names += ",";
        names += b->name();
    }
    size_t needed = names.size() + 1;
    if (!buffer) {
        *size = needed;
        return CLAY_OK;
    }
    if (*size < needed) {
        *size = needed;
        return fail(CLAY_ERROR_BUFFER_TOO_SMALL, "backend list needs " + std::to_string(needed));
    }
    std::memcpy(buffer, names.c_str(), needed);
    *size = needed;
    return CLAY_OK;
}

clay_result clay_eval_points(const clay_document* doc, const char* backend,
                             const float* points_xyz, size_t count, float* out_distances,
                             float* out_colors_rgb) {
    if (!doc || !points_xyz || !out_distances)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null buffer");
    eval::Backend* b = eval::Registry::instance().find(backend ? backend : "cpu");
    if (!b) return fail(CLAY_ERROR_NOT_FOUND, std::string("backend not registered: ") +
                                                  (backend ? backend : "cpu"));
    scene::Tape tape = scene::compile_document(doc->doc.document);
    eval::PointQuery q{points_xyz, count, 1e-4f};
    eval::PointResults out{out_distances, nullptr, out_colors_rgb};
    eval::Status s = b->eval_points(tape, q, out);
    if (s != eval::Status::Ok) return fail(CLAY_ERROR_BACKEND, "eval_points failed");
    return CLAY_OK;
}

clay_result clay_raycast(const clay_document* doc, const float origin[3], const float dir[3],
                         int32_t* out_hit, float* out_t, float out_position[3],
                         float out_normal[3]) {
    if (!doc || !origin || !dir || !out_hit)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    eval::Backend* b = eval::Registry::instance().find("cpu");
    scene::Tape tape = scene::compile_document(doc->doc.document);
    float ray[6] = {origin[0], origin[1], origin[2], dir[0], dir[1], dir[2]};
    eval::RayQuery q{ray, 1, 0.0f, 1e6f, 1e-4f, 256};
    eval::RayHit hit;
    if (b->raycast(tape, q, &hit) != eval::Status::Ok)
        return fail(CLAY_ERROR_BACKEND, "raycast failed");
    *out_hit = hit.hit;
    if (out_t) *out_t = hit.t;
    if (out_position) std::memcpy(out_position, hit.pos, sizeof hit.pos);
    if (out_normal) std::memcpy(out_normal, hit.normal, sizeof hit.normal);
    return CLAY_OK;
}

clay_result clay_document_mesh(const clay_document* doc, const clay_mesh_params* params,
                               clay_mesh** out_mesh) {
    if (!doc || !params || !out_mesh)
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "null argument");
    scene::Tape tape = scene::compile_document(doc->doc.document);
    if (tape.empty()) return fail(CLAY_ERROR_INVALID_ARGUMENT, "empty document");
    math::Aabb region = tape.bounds;
    if (region.empty() || region.is_infinite())
        return fail(CLAY_ERROR_INVALID_ARGUMENT, "unbounded scene");
    float voxel = params->voxel_size;
    if (voxel <= 0) {
        int res = params->resolution > 0 ? params->resolution : 128;
        kernel::cfloat3 ext = region.extent();
        voxel = kernel::cmax(ext.x, kernel::cmax(ext.y, ext.z)) / static_cast<float>(res);
    }
    mesh::Mesh m = mesh::mesh_tape(tape, region, voxel);
    if (m.empty()) return fail(CLAY_ERROR_BACKEND, "meshing produced no triangles");
    if (params->decimate) {
        mesh::DecimateOptions opts;
        opts.target_ratio = params->decimate_ratio > 0 ? params->decimate_ratio : 0.5f;
        m = mesh::decimate(m, opts);
    }
    auto* handle = new clay_mesh();
    handle->data = std::move(m);
    *out_mesh = handle;
    return CLAY_OK;
}

void clay_mesh_destroy(clay_mesh* mesh) { delete mesh; }

size_t clay_mesh_vertex_count(const clay_mesh* mesh) {
    return mesh ? mesh->data.positions.size() : 0;
}
size_t clay_mesh_index_count(const clay_mesh* mesh) {
    return mesh ? mesh->data.indices.size() : 0;
}
const float* clay_mesh_positions(const clay_mesh* mesh) {
    return mesh && !mesh->data.positions.empty() ? &mesh->data.positions[0].x : nullptr;
}
const float* clay_mesh_normals(const clay_mesh* mesh) {
    return mesh && mesh->data.normals.size() == mesh->data.positions.size()
               ? &mesh->data.normals[0].x
               : nullptr;
}
const float* clay_mesh_colors(const clay_mesh* mesh) {
    return mesh && mesh->data.colors.size() == mesh->data.positions.size()
               ? &mesh->data.colors[0].x
               : nullptr;
}
const uint32_t* clay_mesh_indices(const clay_mesh* mesh) {
    return mesh && !mesh->data.indices.empty() ? mesh->data.indices.data() : nullptr;
}

clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                               int32_t* out_manifold) {
    if (!mesh) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh");
    mesh::ValidationReport r = mesh::validate(mesh->data);
    if (out_watertight) *out_watertight = r.watertight ? 1 : 0;
    if (out_manifold) *out_manifold = r.manifold ? 1 : 0;
    return CLAY_OK;
}

clay_result clay_mesh_save(const clay_mesh* mesh, const char* path) {
    if (!mesh || !path) return fail(CLAY_ERROR_INVALID_ARGUMENT, "null mesh or path");
    std::string p(path);
    std::size_t dot = p.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot + 1);
    if (ext == "obj") return from_io(io::save_obj_file(mesh->data, p));
    if (ext == "ply") return from_io(io::save_ply_file(mesh->data, p));
    if (ext == "fbx") return from_io(io::save_fbx_file(mesh->data, p));
    return fail(CLAY_ERROR_UNSUPPORTED, "unknown extension: " + ext);
}

}  // extern "C"
