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

#include <cstdint>
#include <map>
#include <optional>
#include <type_traits>
#include <string>
#include <vector>

#include "clay/io/result.h"
#include "clay/mesh/mesh_data.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/topology_cache.h"
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
// Minor 17 writes a SHARED PAYLOAD once, and like 14 and 16 it is a SCENE
// PAYLOAD change — see scene::kSceneMinor for the id, why it is document-wide
// rather than a NodeId, and the measurement behind it. Unlike 15 and 16 it
// rewrites a field rather than appending one, so a build that predates 17 reads
// a length where an id sits and FAILS on the first node carrying a volume or a
// gate rather than misreading it. Writing AT minor 16 hands such a build a
// document it can open, with every payload written once per node exactly as
// before; what is lost is the deduplication, which costs bytes rather than
// anything an artist authored, and is therefore the recoverable direction.
// Minor 18 adds a LAYER's COMPOSITION — the op it folds into the layers
// beneath it with — and like 12, 15 and 16 it is an APPENDED scene field; see
// scene::kSceneMinor for the block's layout. A build that predates 18
// desynchronises on the first layer record and FAILS rather than misreading,
// which is this format's usual direction. The DOWNGRADE is where 18 differs
// from every minor before it: writing at 17 is allowed only for a document
// whose layers all union, and REFUSED for one that carries a composition,
// because a subtractive layer written as a union opens cleanly as a different
// sculpture. scene::layer_blocking_minor is the query a caller asks first.
// Minor 19 adds an 'MRES' chunk: one mesh layer's multiresolution hierarchy,
// the layer id and the bytes MultiresSurface::encode() already produces. A NEW
// CHUNK, so this is minor 13's mild kind -- length prefixed, skipped by a build
// that predates 19 exactly as it already skips a mesh layer it does not know,
// and the scene payload is untouched, so a document carrying no hierarchy
// serialises to the bytes it always did. The container does not version the
// surface: the chunk gates whether a hierarchy EXISTS and the surface's own
// encoding gates what is in it, because two negotiations of one question
// eventually disagree.
//
// THE ONE-DIRECTIONAL LOSS IS THE UNSAFE DIRECTION HERE, which is new. Minor 13
// named the same shape for groups and could call it safe: a build that predates
// it opens a document, saves it back, and drops the groups -- "geometry
// reappearing is recoverable and obvious". A build predating 19 doing the same
// drops a SCULPT. The cage returns, the levels do not, and the file opens
// cleanly and looks deliberate, which is the one failure this format works
// hardest to avoid. Nothing here can stop an older build; what this can do is
// say so, and `multires_carries_detail` is what a host asks to know whether a
// given document has anything to lose.
inline constexpr std::uint16_t kClaySpaceMinor = 19;

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
    // PER MESH LAYER, THE GENERATION OF ITS TRIANGLES: 1 when they are first
    // installed and one more every time they are REPLACED WHOLESALE. What it
    // exists for is the change a cache does NOT survive -- a rebuild swaps every
    // vertex and every index, and an adjacency, a BVH or a live sculptor built
    // over the old ones is wrong in a way nothing else detects. A sculpt does
    // not move it: a brush displaces vertices and leaves the topology alone,
    // which is precisely the change those caches are built to survive.
    //
    // HERE, BESIDE THE TRIANGLES, rather than on the binding handle that used
    // to hold it (#472). Kept there, the counter had exactly two writers and
    // undo, redo and journal replay were none of them: they restore a mesh
    // through `session::History`'s `mesh::Mesh*` resolver, which has no way to
    // tell the binding's separate map which layers it replaced. So a rebuild
    // moved the token and undoing the rebuild did not, and a host sculpting
    // through an adjacency built over the restored triangles got a refused
    // stroke naming nothing. The mutation now owns its invalidation signal:
    // `install_mesh_geometry` is the only way triangles enter a layer, and it
    // cannot install without advancing.
    //
    // RUNTIME-ONLY -- deliberately not serialized, and no format minor. The
    // number is an invalidation token for caches that are LIVE in this session;
    // nothing an adjacency or a sculptor was built over can survive a reopen,
    // so a loaded document establishes a fresh generation domain starting at 1.
    // Writing it would also make a save's bytes depend on how the session got
    // here, which is exactly what the round-trip identity forbids.
    std::map<scene::LayerId, std::uint64_t> mesh_geometry_revision;

    // The generation of `layer`'s triangles. 1 for a layer that holds none, so
    // a reader never has to distinguish "never installed" from "installed once"
    // -- both mean "nothing has been replaced under you".
    std::uint64_t mesh_revision(scene::LayerId layer) const {
        auto it = mesh_geometry_revision.find(layer);
        return it == mesh_geometry_revision.end() ? 1u : it->second;
    }

    // THE ONE PLACE A MESH LAYER'S TRIANGLES ARE INSTALLED. An attach, a
    // rebuild, a load, an undo, a redo, a replayed journal event -- all of them
    // land here, and every one of them advances the generation. A caller that
    // reaches `mesh_layers` directly to assign is the bug this exists to make
    // unreachable.
    void install_mesh_geometry(scene::LayerId layer, mesh::Mesh triangles) {
        mesh_layers.insert_or_assign(layer, std::move(triangles));
        ++mesh_geometry_revision[layer];
        // AND THE ADJACENCY BUILT OVER THE TRIANGLES THAT JUST LEFT. It is the
        // same statement as the line above it and lands here for the same
        // reason: this is where triangles ENTER a layer, so it is where what
        // was derived from them can be invalidated without every caller having
        // to remember. `note_mesh_geometry_replaced` below is the other half --
        // a weld rewrites them in place and never comes through here.
        topology_cache.forget(layer);
    }

    // THE ADJACENCY EVERY MESH SCULPTOR OVER A LAYER SHARES, keyed by layer id.
    //
    // HERE, BESIDE THE TRIANGLES, for the reason `mesh_geometry_revision` is
    // here: it describes them, and a cache that lived on a binding handle would
    // be invalidated only by the paths that binding knows about. Both bindings
    // reach this one, and `install_mesh_geometry` above is what keeps it
    // honest.
    //
    // Building the weld classes and the neighbourhood CSR a brush walks is the
    // WHOLE of what a sculptor costs to construct -- 120.8 ms on a
    // 296k-triangle mesh, against a sculptor whose other members are empty
    // vectors -- and a second session on one layer used to pay it again. It is
    // 0.25 ms now.
    //
    // AN ENTRY IS VERIFIED, NOT TRUSTED. Every lookup fingerprints the entry
    // against the mesh it is about to be served for, so a replacement path that
    // reached `mesh_layers` some other way is a slow miss rather than a wrong
    // adjacency. That is belt and braces with the line above, deliberately:
    // this counter has been wrong before (#472).
    //
    // RUNTIME-ONLY, like the generation beside it: nothing here is serialized
    // and a reopen starts empty.
    mesh::TopologyCache topology_cache;

    // The same signal for a rewrite made IN PLACE through a borrowed mesh --
    // a weld, which rewrites the triangles without ever holding a second copy
    // of them. Separate from the installer rather than folded into it because
    // the alternative is copying a whole mesh out and back to say one thing.
    //
    // IT FORGETS TOO, AND THAT IS THE POINT OF IT BEING A SECOND CHOKEPOINT. A
    // weld changes the triangles an adjacency was built over without replacing
    // the container they live in, so `install_mesh_geometry` never runs and
    // cannot speak for it. Two paths change a layer's geometry; both invalidate
    // here, and neither caller has to remember.
    //
    // The fingerprint would have caught a miss -- a weld moves the vertex and
    // triangle counts, so the entry fails verification and is rebuilt. That is
    // exactly why this was missed when the cache was written against the
    // installer alone: the belt held while the braces were absent, and nothing
    // failed. Forgetting here makes the miss impossible rather than survivable.
    void note_mesh_geometry_replaced(scene::LayerId layer) {
        ++mesh_geometry_revision[layer];
        topology_cache.forget(layer);
    }

    // A mesh layer's multiresolution hierarchy, keyed the same way and for the
    // same layering reason. Before this, a hierarchy was a STANDALONE handle
    // that no document held, so saving a sculpt saved the base cage and dropped
    // every level above it -- and a host's own side-car file was the only record
    // that a row had ever been a hierarchy.
    //
    // THE CAGE EXISTS TWICE and the two are NOT reconciled. mesh_layers holds
    // the triangles; the hierarchy holds its own copy of the base level, because
    // mesh::multires_from_mesh builds from a mesh VALUE and keeps no link back.
    // The two could already diverge before they were both stored, so carrying
    // them together does not create that hazard -- it makes it observable, which
    // `multires_matches_cage` below is for. Nothing here edits either to agree
    // with the other: a save that mutated authored content would be a worse
    // surprise than a disagreement a caller can ask about.
    //
    // Orphan behaviour is mesh_layers', for its reason: an entry outlives its
    // layer so undo within a session works, the writer emits a chunk only for an
    // id that is still a mesh layer, and the reader drops one naming none.
    std::map<scene::LayerId, mesh::MultiresSurface> multires_layers;
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

// THE ONE THING THIS STRUCT HAS TO STAY, said here rather than discovered at a
// call site three files away.
//
// `load_clayspace` builds a fresh document and MOVE-ASSIGNS it over the
// caller's (`*out = std::move(result)`). A member that is not move-assignable
// — a std::mutex, an atomic, a reference, a const field — deletes the implicit
// move, and the compiler then reports that the COPY assignment is deleted,
// naming an operation nobody wrote at a line that is not the cause. This says
// what actually broke, at the definition that broke it.
//
// `mesh::TopologyCache` is the member that made this reachable: it holds a
// mutex and declares its moves explicitly for exactly this reason.
static_assert(std::is_move_assignable_v<ClaySpaceDoc>,
              "load_clayspace move-assigns a fresh document over the caller's. A member that "
              "is not move-assignable -- a std::mutex, an atomic, a reference -- deletes the "
              "implicit move and the error names COPY assignment instead. Declare the "
              "member's moves explicitly, as mesh::TopologyCache does.");

// WHICH SNAPSHOT IS THIS (survive-a-crash 2.1).
//
// A 64-bit hash of the serialized bytes, and nothing more: what a crash
// journal needs is "this is not the snapshot I was taken against", and the
// cost of a false MATCH is a wrong recovery that a full compare would have to
// hold the whole snapshot in memory to avoid. Every serializing entry point
// stamps it into `scene::Document::snapshot_id`, so a host gets the pairing
// without asking for it — see `History::journal_since`.
//
// NOT a checksum: it is not written into the file, it does not detect
// corruption on disk, and it is stable neither across builds that change the
// document encoding nor across byte orders. It answers one question, about two
// things already in memory. Costs 0.24 ms on a 1.13 MB snapshot, against the
// 1.52 ms the save producing those bytes costs.
std::uint64_t snapshot_identity(const std::uint8_t* data, std::size_t size);

// -- hierarchies, and the two questions carrying one raises -------------------

// Does a hierarchy hold anything an artist authored above its base cage?
// Levels above 0, or any sculpt layer -- a base deformation layer writes at
// level 0 and is still authored, so a level count alone would miss it.
//
// This is the question a DOWNGRADE turns on, not a display concern: a cage with
// nothing on it loses nothing by being written as a plain mesh layer.
bool multires_carries_detail(const mesh::MultiresSurface& surface);

// Does this layer's cage agree with its hierarchy's base level?
//
// NOT a promise that they ever will. A hierarchy is built from a mesh VALUE
// (`mesh::multires_from_mesh`) and keeps no link to the layer, so the two have
// always been able to drift; before this change nothing in the document could
// see it, because the hierarchy was not in the document. This answers the
// question rather than preventing the drift, and nothing here edits either side
// to agree with the other.
//
// COMPUTED, NEVER STORED. `snapshot_identity` is the right shape and the wrong
// storage: it is stable "neither across builds that change the document encoding
// nor across byte orders", so a hash written into a file would report a
// divergence that had not happened the first time an encoding moved. Counts are
// compared before positions, because a retopology, a decimation or a re-import
// -- the ways this actually happens -- all change a count.
bool multires_matches_cage(const mesh::Mesh& cage, const mesh::MultiresSurface& surface);

// NO `multires_blocking_minor`, deliberately, and the reason is worth keeping
// because the plan for this change had one. It was modelled on
// `scene::layer_blocking_minor`, which exists because `serialize_document` takes
// a minor to WRITE at and can therefore be asked to write one it cannot express.
// `save_clayspace` takes no such parameter: the container is always written at
// `kClaySpaceMinor`, and the older-minor discipline in this format lives in the
// SCENE PAYLOAD rather than here. A query answering which layer blocks a write
// nobody can request would be an entry point with no caller.
//
// What replaces it is `multires_carries_detail` above, which answers the
// question a host actually has -- "would anything an artist made be lost" --
// without pretending the container has a downgrade path it does not.

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
