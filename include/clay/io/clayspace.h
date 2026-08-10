#pragma once

// .clayspace document container (file-io spec): binary chunked format.
//
//   magic "CLAY" | u16 major | u16 minor | chunks...
//   chunk: fourcc u32 | u64 payload size | payload
//
// Chunks: 'SCNE' scene command/document payload, 'VOXL' one voxel layer
// (u32 layer id + VoxelGrid stream), 'MASK' one layer's mask field (u32 layer
// id + MaskField stream), 'MESH' one mesh layer's triangles (u32 layer id +
// mesh stream, see io/mesh_io.h), 'THMB' thumbnail bytes (PNG,
// passthrough), 'CAMB' camera bookmarks (passthrough). Unknown chunks are
// skipped (backward-open); a higher major version refuses to load
// (forward-refuse) with no partial document.
//
// Minor 1 packs a layer's ghost and lock flags into the byte its visibility
// flag already occupied. A minor-0 document therefore loads with both off,
// with no version handling; a minor-1 document read by a build that predates
// the flags loses them, and a layer that is both hidden and ghosted reads as
// visible there. That is the whole extent of the incompatibility, and it is
// why the minor moved.
//
// Minor 2 adds a type and two Bezier handles to each stroke point, plus a
// closed flag and a tolerance per item. From here the scene payload is decoded
// AGAINST the minor rather than assuming the current layout, so the next field
// a node gains needs no packing trick: minor 0 and 1 documents read their
// points as hard corners, which is what they already meant.
//
// Minor 3 adds a loft's profile list to a node. It sits in the middle of the
// node record rather than at the end, which is fine precisely because the
// reader is told the version — that is the whole point of decoding against the
// minor instead of guessing from the bytes.
//
// Minor 6 lets a voxel layer carry a stack of resolution levels. The 'VOXL'
// chunk still opens with the COARSEST level in the layout it always had, and
// any finer level follows inside the same chunk as a tail of per-cell offsets.
// A build that predates the tail therefore stops after the coarsest level and
// opens the document there rather than failing, and a layer with one level
// serialises to the bytes it always did. The scene payload is untouched; the
// minor moves because the container's content did, and kSceneMinor moves with
// it because a reader is told one number for both.
//
// The cost of opening rather than failing: such a build holds only the coarsest
// level, so SAVING the document back drops the finer ones. That is lossy in the
// one direction a chunked format cannot protect against — the levels are gone
// from the reader's model, not merely unwritten — and it is the trade the
// backward-open rule buys. A build that understands the tail round-trips it.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/document.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace io {

inline constexpr std::uint16_t kClaySpaceMajor = 1;
inline constexpr std::uint16_t kClaySpaceMinor = 6;

// The document bundle a .clayspace file holds. Voxel layer content is keyed
// by layer id (the scene module stays voxel-agnostic by layering rule).
struct ClaySpaceDoc {
    scene::Document document;
    std::map<scene::LayerId, voxel::VoxelGrid> voxel_layers;
    // Masks sit beside voxel content rather than inside scene::Document, so a
    // mask's presence cannot change what the document evaluates to — the
    // structural version of "masking gates authoring, not evaluation".
    std::map<scene::LayerId, voxel::MaskField> masks;
    // A mesh layer's triangles, for the same reason and one more: the layering
    // table withholds clay/mesh from clay::scene, so imported geometry
    // physically cannot enter the evaluated document. Stored exactly as the
    // importer returned it — no welding, reordering, renormalizing or
    // reindexing — so the round trip is an identity.
    //
    // An entry is never erased when its layer is removed, because the inverse
    // of a layer removal restores a Layer by value and cannot carry a payload.
    // save_clayspace writes a chunk only for an id that is still a mesh layer
    // and load_clayspace drops a chunk that names none, which is what keeps an
    // orphan harmless without breaking undo within a session.
    std::map<scene::LayerId, mesh::Mesh> mesh_layers;
    std::vector<std::uint8_t> thumbnail_png;      // optional passthrough
    std::vector<std::uint8_t> camera_bookmarks;   // optional passthrough
};

std::vector<std::uint8_t> save_clayspace(const ClaySpaceDoc& doc);
IoStatus load_clayspace(const std::uint8_t* data, std::size_t size, ClaySpaceDoc* out);

IoStatus save_clayspace_file(const ClaySpaceDoc& doc, const std::string& path);
// The budget's max_file_bytes bounds what will be read into memory before the
// buffer is sized. It is a parameter rather than a fixed ceiling because a
// document carrying sampled volumes is large by nature, and nothing here caps
// what save_clayspace_file will WRITE — a reader that could not raise the
// ceiling would be unable to reopen a document this library had just written.
IoStatus load_clayspace_file(const std::string& path, ClaySpaceDoc* out,
                             const ImportBudget& budget = {});

// Scene payload codec shared with the command vocabulary (scene chunk =
// serialize_document; exposed for tests and the C ABI).
IoStatus decode_document(const std::uint8_t* data, std::size_t size, scene::Document* out,
                         std::uint16_t minor = kClaySpaceMinor);

}  // namespace io
}  // namespace clay
