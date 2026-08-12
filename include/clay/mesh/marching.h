#pragma once

// Default SDF mesher: marching tetrahedra over a Freudenthal 6-tet cube
// decomposition with globally consistent face diagonals. No ambiguous sign
// configurations exist, so output is watertight and 2-manifold by
// construction (meshing spec). Vertices are welded on canonical lattice-edge
// keys; triangle orientation is outward (toward positive field).

#include <cstdint>
#include <functional>

#include "clay/brick/cache.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/tape.h"

namespace clay {
namespace mesh {

enum class NormalMode : std::uint8_t {
    None,
    Face,      // area-weighted face normals
    Gradient,  // field gradient via the tape (blend-faithful)
};

struct MeshingOptions {
    NormalMode normals = NormalMode::Gradient;
    bool colors = true;         // sample the tape's color field per vertex
    float gradient_eps = 1e-4f;
};

// Core lattice mesher: sample(i,j,k) returns the field at lattice point
// (i,j,k); cells [cell_min, cell_max) are marched (cell (i,j,k) spans
// lattice points i..i+1 etc). World position = origin + index * spacing.
Mesh mesh_lattice(const std::function<float(int, int, int)>& sample, int cell_min[3],
                  int cell_max[3], kernel::cfloat3 origin, float spacing);

// Ceiling on the dense sample grid a single mesh_tape call will allocate.
// The grid is sized from the caller's voxel size, so without a ceiling an
// over-fine size ends the process in the allocator rather than returning —
// the library builds without exceptions.
//
// Set from what the API PROMISES, not from what is comfortable: the documented
// headline call meshes at resolution 512, which is 514^3 = 136M lattice points
// over a cubic region. The ceiling has to clear that, so it is 2^28 (268M
// samples, about 1 GB of float) — a guard against a runaway request, not a
// working budget.
inline constexpr std::size_t kMaxGridSamples = 1u << 28;

// Mesh a tape over a world region at the given voxel size (dense evaluation
// through the CPU reference; backends provide accelerated variants via
// Backend::mesh). Returns an empty mesh for an empty or infinite region, for a
// voxel size that is not finite and positive, and for one so fine that the
// region needs more than kMaxGridSamples lattice points.
Mesh mesh_tape(const scene::Tape& tape, const math::Aabb& region, float voxel_size,
               const MeshingOptions& options = {});

// What one brick key contributed to a brick mesh: contiguous, and together
// with the other keys' ranges a partition of the output.
//
// Welding spans brick SEAMS, so a triangle in one key's index range may
// reference a vertex in an EARLIER key's vertex range — the first key to reach
// a shared seam vertex owns it. A consumer may therefore overwrite a key's
// ranges in a GPU buffer, which is what these are for, but may not free one
// key's vertices without checking its neighbours'. Breaking the weld to make
// the ranges independent would produce a seam-duplicated mesh, and this is
// also the export path, where watertightness is the contract.
struct BrickMeshRange {
    brick::BrickKey key;
    std::uint32_t vertex_first = 0, vertex_count = 0;
    std::uint32_t index_first = 0, index_count = 0;
};

// Mesh a filled brick cache: marches only cells owned by surface bricks
// (the band width >= 1 voxel guarantees no crossing escapes into
// inside/outside bricks). Attributes come from the document when given —
// gradient normals and colours are evaluated through PER-BRICK culled tapes,
// the same culling the refill path uses, so their cost follows the bricks
// being meshed rather than the size of the document (issue #73). Inside a
// brick's band-dilated cull region the culled tape's band-clamped results are
// bit-identical to the full tape's, and the vertices sit on the surface with
// gradient taps of gradient_eps << band, so the attributes match a full-tape
// evaluation exactly.
//
// `keys` names the bricks to march; nullptr means every surface brick, which
// is the whole-surface export path. A key that stores no lattice contributes
// nothing and is not an error — a drained dirty set routinely contains bricks
// that turned out uniform.
//
// Marching a SUBSET samples across the subset's boundary exactly as the whole
// does, because BrickCache::sample answers for any key. So the triangles
// produced for a cell are identical either way; a subset differs only in that a
// vertex shared with a cell outside the subset is emitted again by whichever
// mesh reaches it, at a bit-identical position. A duplicated seam vertex, never
// a crack.
//
// A subset also returns the STRADDLERS: every whole-mesh triangle with at
// least one corner inside a requested brick's closed box, including those
// whose cell is owned by an unrequested surface brick. Each straddler is
// attributed to the lexicographically lowest (x, then y, then z) requested
// key whose closed box contains one of its corners, and lands in that key's
// ranges after the key's own cells. Without them no sequence of subset meshes
// could maintain a complete surface (issue #66): the omitted triangles lived
// in cells the request never named, and dilating the request only moved the
// boundary. A consumer holding per-brick geometry dedupes by triangle, since
// a straddler touching two requested keys is attributed to one of them per
// call and may move between shares as the requested set changes.
//
// `out_ranges` (may be nullptr) receives one entry per key, in the order given.
//
// `cull_index` (scene/cull_index.h) may share the caller's per-revision cull
// index so the attribute pass reuses its cached bounds; nullptr builds one
// for the call when attributes need the document. Either way the attributes
// are unchanged — the index only accelerates the per-brick culled compiles.
//
// `lod` selects the LEVEL to march, as BrickCache::sample_lod does: 0 is the
// evaluated bricks, 1 their mips. A mip is the same dim³ lattice with every
// second point kept, so a level changes exactly two things — the spacing
// doubles and the keys are coarse ones naming 2x2x2 blocks — and the march,
// the welding and the straddler attribution are the ones level 0 uses,
// unchanged. `keys` at level 1 is therefore a list of COARSE keys, and nullptr
// means every built mip. A level with no mip built holds no bricks and meshes
// empty; distinguishing "not built" from "no surface" is the C boundary's job,
// which has an error to report it with. Anything above 1 has no bricks at all
// and returns an empty mesh rather than clamping to a level the caller did not
// ask for.
//
// FIELD ATTRIBUTES ARE LEVEL 0 ONLY. Colours and gradient normals ride the
// per-brick culled tapes above, whose bit-identity argument is that a vertex
// sits on the field's surface (|d| ~ 0, well inside the band where a culled
// tape and the full one agree). A coarse vertex sits on the MIP's surface,
// which can be most of a coarse cell away from the field's, so the argument
// does not hold and the numbers would be silently approximate. They are
// skipped here and refused outright at the C boundary. Face normals come from
// the triangles and are unaffected.
Mesh mesh_bricks(const brick::BrickCache& cache, const scene::Document* doc_for_attributes,
                 const MeshingOptions& options = {},
                 const std::vector<brick::BrickKey>* keys = nullptr,
                 std::vector<BrickMeshRange>* out_ranges = nullptr,
                 const scene::CullIndex* cull_index = nullptr, int lod = 0);

// Attribute helpers (meshing spec: vertex attributes).
void apply_tape_attributes(Mesh& m, const scene::Tape& tape, const MeshingOptions& options);
void compute_face_normals(Mesh& m);
// Box-projection UVs from the dominant normal axis; scale = world units per
// UV tile.
void uv_box_project(Mesh& m, float scale = 1.0f);

}  // namespace mesh
}  // namespace clay
