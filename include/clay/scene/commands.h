#pragma once

// The one command vocabulary shared by the in-memory undo stack and the
// document file format (scene-model spec): every mutation is a serializable
// command with a computable inverse; stroke commands coalesce into single
// undo steps.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "clay/math/geom.h"
#include "clay/scene/document.h"
#include "clay/scene/memory.h"

namespace clay {
namespace scene {

struct AddNodeCmd {  // add item or group subtree (preorder, ids preserved)
    LayerId layer = 0;
    NodeId parent = kNoNode;
    int index = -1;
    std::vector<Node> subtree;
};
struct RemoveNodeCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
};
struct MoveNodeCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    NodeId new_parent = kNoNode;
    int new_index = -1;
};
struct SetTransformCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    math::Transform xform;
    // The per-axis scale rides the transform command rather than getting one of
    // its own because this command is the WHOLE transform: a caller that sets a
    // uniform transform means a uniform scale, and one undo step should put
    // back everything one edit changed. Default (1, 1, 1), so a command built
    // by anything that predates the field is a uniform transform and says so.
    kernel::cfloat3 scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);
};
struct SetPrimCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    Prim prim;
};
struct SetColorCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    kernel::cfloat3 color = kernel::cf3(0, 0, 0);
};
struct SetOpBlendCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    Op op = Op::Add;
    Blend blend;
    float rounding = 0.0f;
};
struct AppendStrokeCmd {  // inverse: TrimStrokeCmd — the coalescing pair
    LayerId layer = 0;
    NodeId node = kNoNode;
    std::vector<StrokePoint> points;
};
struct TrimStrokeCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    std::uint32_t count = 0;  // remove last N points
};
struct AddLayerCmd {
    Layer layer;
    int index = -1;
    // An INSTANCE names the layer whose edit list it shares instead of
    // carrying one. 0 — the default, and what every other layer-add means —
    // is "the Layer above carries its own content".
    //
    // Needed because this command travels: in memory `layer.sdf` is already
    // the source's `shared_ptr` and this field changes nothing, but the
    // journal and the document file serialize the command, and a Layer
    // serialized with its content inline comes back as a COPY. That is the
    // multiplication `io::layer_memory` promises does not happen, so the
    // reference has to survive the encoding rather than only the pointer.
    //
    // Resolved on apply against the document being applied to, which for a
    // replay is the snapshot the journal was taken against — so the source
    // layer is present and its id resolves. An id that does not resolve
    // REFUSES the command; falling back to a copy would reintroduce the
    // defect one recovery later, silently.
    LayerId content_source = 0;
};
struct RemoveLayerCmd {
    LayerId id = 0;
};
struct SetLayerVisibleCmd {
    LayerId id = 0;
    bool visible = true;
};
// Replace an item's whole point list, plus the two properties that only mean
// anything alongside it. A curve is tens of points, so a whole-list replace
// costs less than the bookkeeping six granular commands would need, and its
// inverse is the previous list — exact by construction rather than by careful
// arithmetic. Append/Trim stay for the drag case, where the list only grows.
struct SetStrokePointsCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    std::vector<StrokePoint> points;
    bool closed = false;
    float tolerance = 0.01f;
};

// An armature's whole tree, replaced. Whole-tree for SetStrokePointsCmd's
// reason: an armature is tens of nodes, so replacing the list costs less than
// the bookkeeping granular commands would need, and its inverse is exactly the
// tree that was there. The tree EDITS — add a child, move a node carrying its
// subtree, delete a subtree — are pure functions in scene/armature.h that
// compute the new tree; this is what installs one.
struct SetArmatureCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    std::vector<StrokePoint> nodes;
    std::vector<std::uint32_t> parents;
    // +1 or -1 per node; shorter than `nodes` reads as positive-padded, the
    // reading the kernel and the node record both make.
    std::vector<std::int8_t> signs;
    float blend_k = 0.0f;
};

// A node's whole deformer chain, replaced. Whole-list for SetStrokePointsCmd's
// reason: a chain is a handful of records, so replacing it costs less than the
// bookkeeping granular commands would need, and its inverse is the previous list
// — exact by construction.
//
// Without this a deformer could only be set when its node was created, so no
// verb built on one could act on an existing sculpt, and any that tried would
// escape undo.
//
// ORDER IS THE CONTRACT: deformers apply in authoring order, so `deformers[0]`
// warps the point first and is therefore the OUTERMOST warp on the resulting
// geometry. A verb that means to move the assembled shape puts its deformer at
// the FRONT; one appended at the back has its region weight evaluated at a point
// the earlier deformers already moved.
struct SetDeformersCmd {
    LayerId layer = 0;
    NodeId node = kNoNode;
    std::vector<Deformer> deformers;
};

// Both flags in one command: they are the same concept at two strengths, and
// a UI toggling one usually shows the other beside it.
struct SetLayerProtectionCmd {
    LayerId id = 0;
    bool ghost = false;
    bool locked = false;
};
struct SetLayerTransformCmd {
    LayerId id = 0;
    math::Transform xform;
    // The whole layer's per-axis scale, set with the placement it belongs to
    // (#373). One command rather than two, because they are one placement: two
    // would let an undo restore a frame that never existed, with the rotation
    // from one step and the squash from another.
    kernel::cfloat3 scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);
};
// Mirroring is an edit like any other: it changes what the layer evaluates to.
// It was written straight into the layer, which meant it neither respected the
// lock nor landed on the undo stack.
struct SetLayerMirrorCmd {
    LayerId id = 0;
    std::uint8_t axes = 0;  // kMirrorX|Y|Z
    float k = 0.0f;
};
// The layer's radial symmetry, set as a whole. A command for the same reason
// the mirror is one: it is a property of the layer that evaluation reads, so
// writing it directly would neither respect the lock nor reach the undo stack.
struct SetLayerRadialCmd {
    LayerId id = 0;
    std::uint16_t count = 0;  // 0/1 = off
    std::uint8_t axis = 1;    // 0/1/2
    float k = 0.0f;
};
// A layer's name, replaced. A command rather than a field write for the reason
// the mirror became one: the name was set once at creation and never after, so
// a host kept its own display name beside the document and the rename was lost
// on the next save. Its inverse is the previous name, exact by construction.
struct SetLayerNameCmd {
    LayerId id = 0;
    std::string name;
};
// How a layer folds into the layers beneath it. A command for the reason the
// mirror and the radial mode became ones: it is a property of the layer that
// EVALUATION reads, so writing it straight into the record would neither
// respect the lock nor reach the undo stack.
//
// REFUSED ON A NON-SDF LAYER, which no other layer command does. A voxel or a
// mesh layer never enters the tape, so a composition on one is state nothing
// reads — and the refusal is here, in the vocabulary, rather than only at the
// binding, so a replayed journal cannot install what the setter rejects.
struct SetLayerCompositionCmd {
    LayerId id = 0;
    LayerComposition composition;
};

using Command =
    std::variant<AddNodeCmd, RemoveNodeCmd, MoveNodeCmd, SetTransformCmd, SetPrimCmd,
                 SetColorCmd, SetOpBlendCmd, AppendStrokeCmd, TrimStrokeCmd, AddLayerCmd,
                 RemoveLayerCmd, SetLayerVisibleCmd, SetLayerTransformCmd,
                 SetLayerProtectionCmd, SetStrokePointsCmd, SetDeformersCmd,
                 SetLayerMirrorCmd, SetLayerRadialCmd, SetArmatureCmd, SetLayerNameCmd,
                 SetLayerCompositionCmd>;

// The layer a command would edit, or 0 for one that edits no existing layer
// (adding a layer creates its target; changing protection is how a protected
// layer is released). Exposed because apply() returns nullopt for both "no
// such layer" and "that layer is protected", and a binding has to tell a
// caller which of the two it hit.
LayerId edited_layer(const Command& cmd);

// The world-space INFLUENCE bound of what `cmd` targets in the document AS IT
// IS NOW — the box outside which applying (or having applied) it cannot change
// the field. Empty means nothing to dirty; math::Aabb::infinite() means the
// target's influence has no finite extent.
//
// Called on ONE side of an apply it is not an answer: an add's node is not
// there before, a removal's is not there after, and a move has two ends. The
// undo stack calls it before and after and unions the two, which is what makes
// all three come out right without a case per command.
//
// It is deliberately loose in two places, because being tight there would cost
// correctness rather than buy it:
//   - a node inside a GROUP reports its root ancestor's bound. A group's blend
//     spreads a child's influence past the child's own box, and the amount is
//     the ancestors' business, not the child's.
//   - a node command reports the union over every layer sharing the content.
//     Layer instancing shares SdfContent by reference, so one edit lands once
//     per instance, each through that layer's own transform.
// A command that cannot change what the document evaluates to — a rename, a
// protection flag — reports an empty box rather than the layer's.
class LayerExtent;  // bounds.h; forward so this header stays light

// The ONE existing item a command edits in place, if it is that kind of
// command: the node stays where it is, in the layer it is in, and only its own
// geometry can have moved.
//
// It exists so a cache of a layer's extent can be told what changed instead of
// being dropped (#451). Everything else -- adding, removing or reparenting a
// node, and every layer-level edit -- returns none, and a caller must then
// assume the whole layer moved.
//
// CONSERVATIVE BY CONSTRUCTION: a command kind added later is none until
// somebody names it here, so the failure of forgetting is a cache that is
// dropped too often rather than one that is kept when it should not be.
struct EditedItem {
    LayerId layer = 0;
    NodeId node = kNoNode;
    bool known = false;
};
EditedItem command_edited_item(const Command& cmd);

// `extent` (optional) memoizes the LAYER EXTENT an intersect's bound needs --
// see bounds.h. It matters here because this is called TWICE per edit, on
// either side of the apply, and #319 made that walk O(items) where it used to
// be constant: an intersect drag paid two full layer walks a frame (#451).
//
// THE MEMO KEYS ON POINTER IDENTITY, NOT ON CONTENT, so one handed to both
// calls would answer the second with the first's geometry -- and the two calls
// straddle the apply precisely because the document differs between them. A
// caller reusing one across an edit must key it on `Document::content_serial`,
// which advances inside `apply` for exactly this reason.
math::Aabb command_influence_bound(const Document& doc, const Command& cmd,
                                   LayerExtent* extent = nullptr);

// WHERE A SUPPORTED EDIT CAN CHANGE THE SURFACE, which is a different and
// narrower question from where it changes the FIELD.
//
// `command_influence_bound` above answers the field question, and for an
// INTERSECT the answer is the whole layer's extent -- correct, measured, and
// catastrophic as a refill region: an intersect operand dragged across a form
// re-meshed the layer every frame, 41.5-44.0 ms against 3.8-4.2 for the same
// drag with a subtracting operand, and 6.8-10.1 SECONDS on a fixture with ten
// times the extent (issue #471).
//
// This answers the other question for the ONE edit kind that measured: a
// SetTransformCmd moving an existing, visible INTERSECT item with finite
// support. Taken on both sides of the apply and unioned -- exactly as the
// influence bound is, and by the same caller -- the union is the swept support
// of the operand's old and new geometry, outside which the item's own field is
// beyond the band on both sides and `max(acc, item)` cannot have moved the
// band-clamped result. `item_geometry_reach_in_document` is where that
// argument is written and where its terms are.
//
// nullopt for EVERYTHING ELSE, deliberately and by construction:
//   - any other command kind;
//   - an op that is not Intersect -- a local op's influence bound already IS
//     this box, so there is nothing to narrow and nothing to change;
//   - a node that is missing, hidden or a group on either side;
//   - a deformer chain (its Lipschitz factor is not in the proof), a volume
//     primitive, an unbounded primitive, an infinite grid repeat, a gate;
//   - a morph or a gate anywhere in the layer's chain, a morph in a fold above
//     it, an infinite support, a box that fails a numerical sanity check --
//     all of which `item_geometry_reach_in_document` refuses.
// A caller that gets nullopt on EITHER side must keep the conservative union.
// Falling back is the feature: too wide costs a refill, too narrow is stale
// geometry with nothing on the host's side to point at.
std::optional<math::Aabb> command_surface_delta_bound(const Document& doc, const Command& cmd);

// What an AddLayerCmd that REINSERTS an existing layer must name as its
// content source: the first OTHER layer in stack order holding the same edit
// list, or 0 when this layer holds it alone.
//
// A reorder is a remove and an add, and so is the sever a bake performs. In
// memory the add carries the layer's own shared_ptr and the field changes
// nothing — but the journal serializes the command, and an add naming no
// source writes the edit list INLINE. Replayed, that record deserializes as
// its own content and the layers come back unlinked, with the shapes right
// and nothing to see. So a reinsertion of a shared layer names a sharer, and
// at replay time that sharer is present because it was present when the
// command ran. A deep copy the caller wants (the bake's sever) passes 0 and
// gets the old, inline behaviour.
LayerId content_sharer_of(const Document& doc, LayerId layer);

// The scene payload layout this build writes. It tracks the .clayspace
// container's minor version, which is what a reader is told; io asserts they
// agree so the two cannot drift.
// Minor 10 changed no scene field — it moved in step with the container, whose
// voxel payload gained sculpt layers. Writing a document AT minor 9 therefore
// still produces exactly the bytes minor 9 always did.
//
// Minor 11 adds an item's GATE — the mask that protects a surface from any
// operation.
//
// Minor 14 adds an item's PER-AXIS SCALE (issue #320): three floats appended
// last in the node record, and three more in SetTransformCmd, which carries the
// whole transform and so had to carry this too. Writing AT minor 13 or below
// drops them, and the item degrades to its UNIFORM scale — a squashed cylinder
// comes back round rather than missing, which is the recoverable direction and
// the one an older build can evaluate.
//
// Minor 15 adds a layer record's CONTENT SOURCE (issue #364): one layer id
// before the content, 0 meaning "the content follows" and any other id meaning
// "share that layer's edit list, and nothing follows". It is what makes an
// INSTANCE layer survive a save as a reference instead of as a copy, and it is
// the same field on AddLayerCmd. Which layer owns the content is derived at
// write time from the identity of the content — first holder in stack order
// owns it — so nothing is stored that the document could contradict, and a
// document whose original source layer was removed writes with no special case.
// Writing AT minor 14 or below writes every layer's content inline exactly as
// it always did, and the instances come back as independent copies: the shapes
// are right, the sharing is gone.
//
// Minor 16 adds a LAYER's per-axis scale (issue #373): one cfloat3 in the layer
// record, appended and gated exactly as the radial fields before it are. It is
// the same field minor 14 gave an ITEM, one level up, and it is what gives a
// subtool gizmo three handles instead of one.
//
// Same shape as minors 7, 8, 11, 14 and 15, so the same two directions. A build
// that predates 16 reading a 16 document is one cfloat3 long on the first layer
// record and desynchronised for every record after it, and FAILS — the reader's
// own bounds and element-count checks reject the stream rather than misread it.
// Writing AT minor 15 or below drops the field and the layer comes back at the
// identity triple (1, 1, 1), which is what every file written before this field
// meant: the squash is gone, and visibly so.
// Minor 17 writes a SHARED PAYLOAD once. A Node's `volume` and its `gate` are
// both `shared_ptr<const field::FieldVolume>`, and types.h says why: "several
// items gated by one painted mask should not each carry a copy of it". That was
// true of memory and false of the file — every node serialized its own payload,
// so N items sharing one wrote N copies and came back as N unrelated volumes,
// which meant the next save wrote N again. Measured through the C ABI: one
// captured volume placed eight times saved 1,499,457 bytes against 187,531 for
// one placement, and now saves 189,117.
//
// It is the rule minor 15 gave a shared EDIT LIST, one level down: the first
// holder writes the bytes and every later one names them. The name is a
// DOCUMENT-WIDE payload id and not a NodeId, because node ids are per-layer —
// every layer numbers from 1 — so an id would name a different node in another
// layer. Ids ascend from 1 in the order the deterministic node walk first meets
// each payload, and a reader refuses an id that does not, since no writer
// produces one.
//
// A different shape from 7, 8, 11, 14, 15 and 16: this changes a field that was
// already there rather than appending one, so a build that predates 17 reads a
// length where an id now sits and desynchronises on the first node carrying a
// payload — and FAILS, on the same bounds and element-count checks. Writing AT
// minor 16 restores the per-node shape exactly, so such a build opens the
// document and gets what it always got. What is lost by that downgrade is the
// deduplication and nothing else: the same volumes, once per node.
//
// Minor 18 adds a LAYER's COMPOSITION: one op byte, one blend profile byte, one
// blend radius and one rounding, appended to the layer record and gated exactly
// as the radial fields and the per-axis scale before them are. It is how a
// visible SDF layer folds into the layers beneath it.
//
// Same shape as minors 7, 8, 11, 14, 15 and 16, so the same two directions. A
// build that predates 18 reading an 18 document is ten bytes long on the first
// layer record and desynchronised for every record after it, and FAILS — the
// reader's own bounds and element-count checks reject the stream rather than
// misread it.
//
// A DEPARTURE ON THE WAY DOWN, and the first one this format has made. Every
// earlier minor is writable at the previous one, degrading to what that minor
// meant: 14 comes back unsquashed, 15's instances come back as copies, 17
// writes each payload once per node. None of those is a different sculpture.
// 18 written at 17 WOULD be one — a subtractive layer comes back as a union, so
// the cutter that was carving a hole is a lump welded onto the form, in a file
// that opens cleanly and looks deliberate. So serialize_document REFUSES a
// document carrying any non-default composition below minor 18 (see
// `layer_blocking_minor`), and where every layer unions it writes exactly the
// bytes 17 always did. "Writable at the previous minor" is read as "when the
// previous minor can SAY it", not "by discarding what it cannot".
//
// The default composition IS the hard union, which is what makes "a document
// saved before this feature loads unioning and renders as it did" true by
// construction rather than by a migration.
// Minor 19 changes NO scene field — it moves in step with the container, whose
// document gained an 'MRES' chunk carrying a mesh layer's multiresolution
// hierarchy. Writing a document AT minor 18 therefore still produces exactly the
// bytes minor 18 always did, and `layer_blocking_minor` answers for 19 exactly
// as it answers for 18, because the composition is still the only field whose
// absence changes the model. This is minor 10's case, for minor 10's reason.
inline constexpr std::uint16_t kSceneMinor = 19;

// Apply a command; returns its inverse, or nullopt if the target does not
// exist or is protected (ghosted or locked). The document is unchanged in
// either case.
std::optional<Command> apply(Document& doc, const Command& cmd);

// Binary serialization (the same encoding the document format's command
// chunks use).
// What a command OWNS beyond its variant, for a memory budget. The variant is
// 128 bytes inline whatever it holds, and the entries that matter are the ones
// carrying heap payloads: the inverse of REMOVING an item is an AddNodeCmd
// carrying a whole subtree, while the inverse of adding one is an id.
//
// `seen` charges a shared payload — a sampled volume — once across a whole
// walk. The undo and redo stacks pass one, so a volume held by ten inverses is
// one allocation and is reported as one.
std::size_t command_bytes(const Command& cmd, SharedSeen* seen = nullptr);

std::vector<std::uint8_t> serialize(const Command& cmd);
std::optional<Command> deserialize(const std::uint8_t* data, std::size_t size);

// The first SDF layer whose COMPOSITION `minor` cannot express, or 0 when the
// whole document can be written at that layout with nothing an artist authored
// dropped. A host asks this BEFORE it saves, so it can put an honest sentence
// in front of a person instead of guessing on their behalf.
//
// It is a query and not a bool because a refusal a caller cannot NAME is a
// refusal a caller has to explain by guessing.
//
// Every minor below 18 answers the same way, because the composition is the
// only field so far whose absence changes the MODEL rather than the file: 14's
// per-axis scale comes back unsquashed, 15's instances come back as copies, 16
// the same one level up, and 17 writes each payload once per node. Every one of
// those is smaller or plainer and none of them is a different sculpture.
LayerId layer_blocking_minor(const Document& doc, std::uint16_t minor);

// Whole-document snapshot (used by tests for bit-identity checks and by the
// io module as the scene chunk payload).
// `minor` is the layout to WRITE at, defaulting to the current one. Writing at
// an older layout is what lets a build produce a file an older one can open —
// and it is what makes "a minor-1 document reads as hard corners" testable
// without manufacturing a stream by hand, which only stays correct until the
// next field is added.
//
// REFUSES — returns an EMPTY vector, which is never a valid stream since even
// an empty document writes its layer count — when `minor` cannot express what
// this document says. Today that is exactly `layer_blocking_minor(doc, minor)`
// being non-zero: a layer carrying a composition, written below minor 18.
//
// Refusing rather than degrading is a departure from every earlier minor, and
// it is deliberate. The rule this format has followed is "writable at the
// previous minor, degrading to whatever that minor meant", and it was cheap
// while the loss was never something an artist made — 17 written at 16 costs
// the payload deduplication and nothing else. 18 written at 17 would turn a
// SUBTRACTIVE layer into a union: the cutter that was carving a hole comes
// back as a lump welded onto the form, in a file that opens cleanly and looks
// deliberate. So "writable at the previous minor" is read as "when the
// previous minor can SAY it" rather than "by discarding what it cannot", and
// where every layer unions this still writes exactly the bytes 17 always did.
std::vector<std::uint8_t> serialize_document(const Document& doc,
                                             std::uint16_t minor = kSceneMinor);
// `minor` is the container's minor version, so a node can gain a field without
// inventing a packing trick to stay readable. Defaults to the current layout,
// which is what a standalone round trip wants.
std::optional<Document> deserialize_document(const std::uint8_t* data, std::size_t size,
                                             std::uint16_t minor = kSceneMinor);

// Undo stack over the command vocabulary. perform() applies and records;
// consecutive AppendStrokeCmds on the same node coalesce into one step, and
// begin_group/end_group bundle arbitrary commands into one step.
class UndoStack {
  public:
    bool perform(Document& doc, const Command& cmd);
    // `out_bound` (optional) receives the union of command_influence_bound
    // taken before and after every command in the step — the region a consumer
    // holding a cache has to invalidate, which is otherwise unknowable from
    // outside: an in-place edit keeps its node id, so no diff of the document
    // across the call can see it. Null costs nothing; no bound is computed.
    bool undo(Document& doc, math::Aabb* out_bound = nullptr);
    bool redo(Document& doc, math::Aabb* out_bound = nullptr);
    void begin_group();
    void end_group();
    std::size_t undo_depth() const { return undo_.size(); }
    // What the stacks OWN, for a memory budget. See command_bytes.
    std::size_t undo_bytes() const;
    std::size_t redo_bytes() const;
    std::size_t redo_depth() const { return redo_.size(); }

  private:
    struct Entry {
        std::vector<Command> inverses;  // applied in reverse order on undo
    };
    static bool try_coalesce(Entry& top, const Command& cmd, const Command& inverse);
    // The half undo() and redo() share: apply an entry's commands in reverse,
    // return the entry the opposite stack keeps, and widen `bound` (optional)
    // by what each command targeted on both sides of its apply.
    static Entry replay(Document& doc, const Entry& entry, math::Aabb* bound);
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    // How many brackets are open, not whether one is. Nested brackets collapse
    // into the outermost step — see begin_group.
    int group_depth_ = 0;
};

}  // namespace scene
}  // namespace clay
