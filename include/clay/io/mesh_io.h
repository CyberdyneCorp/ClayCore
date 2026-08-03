#pragma once

// Mesh interchange (file-io spec): dependency-free OBJ+MTL with the
// documented vertex-color extension ("v x y z r g b"), PLY (binary little
// endian + ascii) with vertex colors, FBX (import via ufbx, minimal binary
// writer), and the platform-consumable buffer view for app-side USDZ.

#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/mesh/mesh_data.h"

namespace clay {
namespace io {

// -- OBJ ---------------------------------------------------------------------
// Vertex-color extension: "v x y z r g b" (MeshLab/Blender-compatible).
std::string save_obj(const mesh::Mesh& m, const std::string& object_name = "claycore",
                     const std::string& mtl_name = {});
std::string save_mtl(const std::string& material_name = "clay_default");
IoStatus load_obj(const std::string& text, mesh::Mesh* out, const ImportBudget& budget = {});

IoStatus save_obj_file(const mesh::Mesh& m, const std::string& path, bool with_mtl = true);
IoStatus load_obj_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget = {});

// -- PLY ---------------------------------------------------------------------
std::vector<std::uint8_t> save_ply(const mesh::Mesh& m, bool binary = true);
IoStatus load_ply(const std::uint8_t* data, std::size_t size, mesh::Mesh* out,
                  const ImportBudget& budget = {});

IoStatus save_ply_file(const mesh::Mesh& m, const std::string& path, bool binary = true);
IoStatus load_ply_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget = {});

// -- FBX ---------------------------------------------------------------------
// Import via ufbx (meshes flattened to world space, vertex colors kept,
// units normalized to meters). Export: minimal FBX 7.4 binary — mesh,
// normals, vertex colors, Y-up meters (Unity/Unreal/Blender conventions).
std::vector<std::uint8_t> save_fbx(const mesh::Mesh& m, const std::string& name = "claycore");
IoStatus load_fbx(const std::uint8_t* data, std::size_t size, mesh::Mesh* out,
                  const ImportBudget& budget = {});

IoStatus save_fbx_file(const mesh::Mesh& m, const std::string& path);
IoStatus load_fbx_file(const std::string& path, mesh::Mesh* out, const ImportBudget& budget = {});

// -- platform buffer view (file-io spec: USDZ exclusion) ---------------------
// Contiguous typed arrays, zero conversion — what Model I/O and engine
// importers consume directly.
struct MeshBufferView {
    const float* positions = nullptr;  // count * 3
    const float* normals = nullptr;    // count * 3 or null
    const float* colors = nullptr;     // count * 3 or null
    const float* uvs = nullptr;        // count * 2 or null
    std::size_t vertex_count = 0;
    const std::uint32_t* indices = nullptr;
    std::size_t index_count = 0;
};
MeshBufferView buffer_view(const mesh::Mesh& m);

}  // namespace io
}  // namespace clay
