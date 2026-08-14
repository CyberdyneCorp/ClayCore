#pragma once

// Mesh interchange (file-io spec): dependency-free OBJ+MTL with the
// documented vertex-color extension ("v x y z r g b"), PLY (binary little
// endian + ascii) with vertex colors, FBX (import via ufbx, minimal binary
// writer), and the platform-consumable buffer view for app-side USDZ.
//
// QUADS (mesh/quad_mesh.h). A mesh carrying quads is written AS QUADS by OBJ
// (`f a b c d`), PLY (`element face` counting quads, each row `4 a b c d`) and
// FBX (four indices per polygon). A mesh carrying none is written exactly as
// it always was, in every format.
//
// glTF/GLB IS THE EXCEPTION AND IT IS NOT A DEFECT: glTF 2.0 defines no quad
// primitive mode, so a conforming file cannot carry one and the writer keeps
// writing the triangulation (mode 4). "I exported GLB and got triangles" is
// the likeliest report this feature can produce, and the answer is the format,
// not the writer. MeshBufferView is triangles too, for the same kind of
// reason: it is the GPU readback path and a GPU draws triangles.
//
// The READERS are unchanged, so a quad file this library wrote re-imports as
// TRIANGLES with no quads. That asymmetry is stated rather than left to be
// discovered: preserving faces on import is a second direction with its own
// validation questions, and the readers already hold the face list, so it
// stays a cheap follow-up rather than a hidden gap.
//
// The triangles are not always the ones that were WRITTEN, and that differs by
// format. OBJ and PLY are read by this library's own parsers, which FAN a face
// on the same 0-2 diagonal the quad writer's triangles use, so a save/load
// round trip returns the identical triangle set. FBX is read through ufbx,
// whose ufbx_triangulate_face picks its own diagonal per quad — on a
// quad-meshed sphere roughly 40% of the quads come back split the other way,
// and the solid's measured volume moves by a couple of parts in a thousand
// because a non-planar quad's two diagonals cut different solids. Nothing is
// wrong with either triangulation; a caller comparing an FBX round trip
// against the mesh it saved must compare the SURFACE and not the index buffer.

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

// -- glTF / GLB ---------------------------------------------------------------
// Dependency-free glTF 2.0 binary (GLB) writer: POSITION plus optional
// NORMAL / COLOR_0 (vec3 float) / TEXCOORD_0, uint32 indices, one
// scene/node/mesh primitive. Full glTF-validator conformance runs in CI at
// integration time.
std::vector<std::uint8_t> save_glb(const mesh::Mesh& m);
IoStatus save_glb_file(const mesh::Mesh& m, const std::string& path);

// -- .clayspace mesh stream ---------------------------------------------------
// The payload a document's MESH chunk carries: u32 vertex count, u32 index
// count, u8 attribute mask (1 normals, 2 colors, 4 uvs), then the float arrays
// little-endian, then u32 indices. Uncompressed — triangle data does not
// run-length encode usefully, and a general compressor would be a dependency
// in readers that are deliberately dependency-free.
//
// A mesh carrying quads appends ONE MORE SECTION after the indices: u32 quad
// count, then four u32 per quad. No mask bit and no format version move, both
// deliberately. An unknown mask bit is REFUSED outright by the reader below
// and this repository's format minors are forward-refused, so either would
// make an older build reject a whole document rather than miss an optional
// section whose information is already present as triangles. The appended tail
// instead falls under the reader's "declared > body" bound, so an older build
// opens the document and reads the mesh as the triangles it already is. A mesh
// with no quads writes exactly the bytes it always wrote, and a mesh whose quad
// list is NOT the triangulation beside it writes none either — the reader
// refuses such a section, so emitting one would write a document this library
// cannot open.
//
// The reader claims the FIRST tail after the indices, so a section added later
// belongs AFTER the quads; bytes past the quad list are skipped the way bytes
// past the indices were.
//
// A free function rather than a member of mesh::Mesh (which VoxelGrid and
// MaskField would suggest): mesh::Mesh is a plain interchange struct with no
// invariants of its own, and io already owns every byte format that touches it.
//
// Written exactly as held and read back element for element, so the document
// round trip is an identity rather than a re-derivation.
inline constexpr std::uint8_t kMeshHasNormals = 1;
inline constexpr std::uint8_t kMeshHasColors = 2;
inline constexpr std::uint8_t kMeshHasUvs = 4;

std::vector<std::uint8_t> save_mesh_stream(const mesh::Mesh& m);
// Validates the declared counts against the bytes actually present BEFORE
// allocating, and every index against the vertex count before returning: a
// document's meshes are handed to a host as borrowed contiguous buffers, so an
// index past the vertex array in a file this library did not write becomes an
// out-of-bounds read in the host.
IoStatus load_mesh_stream(const std::uint8_t* data, std::size_t size, mesh::Mesh* out);

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
