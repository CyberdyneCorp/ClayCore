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
// Minor 5 lets a document CARRY a mesh rather than only produce one. It adds a
// 'MESH' chunk per mesh layer and a third value to the layer record's kind
// byte. A reader written against 4 opens a 5 document, skips the unknown chunk,
// and ignores a layer whose kind it does not recognise exactly as it already
// ignores a voxel layer — and loses those layers if it saves the document
// again, the same one-directional loss the minors below carry. The scene
// payload is untouched: a document with no mesh layer serialises to the bytes
// it always did.
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
//
// Minor 7 adds an armature's topology: one parent index per stroke point, at
// the end of the node record, written only from 7.
//
// This is a SCENE PAYLOAD change, and those are a different kind from minor 5
// above. A new chunk is length-prefixed, so a build that does not know it skips
// it; a field inside the node record is not, and node records are written back
// to back. The count word is written for every node at minor 7, armature or
// not, so a document with no armature is four bytes per node LONGER than the
// same document at 6 rather than byte-identical to it, and a build that
// predates 7 reading a 7 document is short by that word on the first node and
// desynchronised for every node after it. It does not misread: the reader's
// bounds and element-count checks reject the stream and the load fails.
//
// That is the same shape as minors 2, 3 and 4, and it is worth being plain
// about what "backward-open" does and does not buy. It means a CURRENT build
// opens every older document, which the payload's decode-against-the-minor rule
// gives. It does not mean an older build opens a newer one — for a payload
// change it cannot, and the protection is that it fails rather than silently
// reading a different scene. Writing at an older minor is how a document is
// made readable by an older build, which is what the `minor` parameter on
// save_clayspace and serialize_document is for.
//
// Minor 8 adds an armature's signs: one byte per node, +1 or -1, after the
// parents at the end of the node record, written only from 8. The same scene
// payload trade as minor 7: the count word goes out for every node, a build
// that predates 8 desynchronises and fails rather than misreads, and writing
// AT minor 7 drops only the signs — an all-positive document loses nothing
// to it.
//
// Minor 9 adds a sampled VOLUME's colour: one packed 0x00RRGGBB word per
// stored sample, appended after the samples, present or absent as a whole
// section. It differs in kind from 7 and 8 — the payload change is inside the
// volume's own blob rather than in the node record — and it is why the volume
// header grew a slot for the section's offset, 0 when there is none.
//
// So the older-reader story is the volume's rather than the record's: every
// section of a volume is addressed by offsets the header carries, so a build
// that predates 9 reads the same index, far and data arrays it always did and
// simply never looks for colour. Writing AT minor 8 drops only the colours —
// an uncoloured volume, which is every volume any build before this produced,
// loses nothing to it.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/document.h"
#include "clay/voxel/grid.h"
#include "clay/voxel/groups.h"
#include "clay/voxel/mask.h"

namespace clay {
namespace io {

inline constexpr std::uint16_t kClaySpaceMajor = 1;
// Minor 10 adds sculpt layers to the voxel payload. That payload is opaque to
// this container, so the bump is a READER SIGNAL rather than a layout change
// here: a minor-9 build opening a minor-10 file meets an unknown tag in the
// voxel stream and falls back to the flattened grid, which is the honest
// degradation — the sculpt is exactly what the layers composed to, it just
// stops being dialable. Nothing is lost that the older build could have shown.
//
// Minor 11 adds an item's gate to the scene payload, which IS a layout change
// there — see scene::kSceneMinor.
//
// Minor 13 adds a 'GRUP' chunk: the document's surface groups, ids and hidden
// set together in one blob. A NEW CHUNK, so this is the mild kind — length
// prefixed, and a build that predates 13 skips it and opens the document with
// no groups, exactly as it already skips a mesh layer it does not know. The
// scene payload is untouched: a document that names no region serialises to the
// bytes it always did.
//
// The one-directional loss is the same one minor 5 carries and is worth naming
// because it is louder here: such a build SAVING the document back drops every
// group AND every hidden flag, so a region an artist had put away comes back
// visible. That is the safe direction — geometry reappearing is recoverable and
// obvious, geometry silently staying hidden is neither.
//
// Minor 14 adds an item's per-axis scale, which is a SCENE PAYLOAD change —
// see scene::kSceneMinor.
//
// Minor 15 adds a layer record's CONTENT SOURCE, and it is a scene payload
// change too: one layer id in every layer record, before the content, 0 for
// "the content follows" and any other id for "share that layer's edit list,
// and nothing follows here". It is what lets an INSTANCE layer — several
// layers over one shared edit list, each with its own transform — be written
// once instead of once per instance. Ten instances of a blockout were ten
// copies in the file and are now one, which is also the accounting
// io::layer_memory promises and could not previously keep across a save.
//
// So it is the shape of minors 7, 8, 11 and 14 rather than of 13: a field
// inside a back-to-back record, not a new chunk. A build that predates 15
// reading a 15 document is one layer id long on the first record and
// desynchronised for every record after it, and FAILS — the reader's bounds
// and element-count checks reject the stream rather than misread it.
//
// Writing AT minor 14 or below is the way to hand such a document to an older
// build, and what it drops is exactly the sharing: every layer goes out with
// its own copy of the content, so the ten instances open as ten independent
// layers. The shapes are all there and every one of them evaluates as it did;
// what is lost is that they were the same subtool — an edit through one no
// longer reaches the others, and the document is ten times the edit list it
// was. An older build SAVING such a document back keeps them independent, so
// reopening it here does not restore the link. That is the recoverable
// direction: duplicated geometry is visible and re-instanceable, whereas a
// silently dropped layer would not be.
//
// Minor 16 adds a LAYER's per-axis scale, and like 14 it is a SCENE PAYLOAD
// change — see scene::kSceneMinor for the field and its gating. Same shape as
// 15 and the same two directions: a build that predates 16 desynchronises on
// the first layer record and FAILS rather than misreading, and writing AT minor
// 15 hands such a build a document it can open, whose layers are unsquashed to
// the identity triple rather than dropped. What is lost is a squash an artist
// applied to a whole subtool; it is visible, and re-applying it is one gizmo
// drag, which is why this is the recoverable direction rather than a reason to
// refuse the downgrade.
inline constexpr std::uint16_t kClaySpaceMinor = 16;

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
    // Surface groups: named regions of the MODEL, on one world-space lattice
    // (add-surface-groups). PER DOCUMENT rather than per layer, and that is the
    // decision rather than an accident — a mask is per layer because it gates
    // edits to that layer, while a group names a region an artist recognises,
    // and "isolate the head" when the head spans two layers is precisely the
    // case per-layer storage makes impossible.
    //
    // Optional, and absent on every document that has never named a region, so
    // the cost of the feature to a document that does not use it is one empty
    // optional and no chunk in the file.
    std::optional<voxel::GroupField> groups;

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
