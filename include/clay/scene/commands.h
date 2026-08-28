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

using Command =
    std::variant<AddNodeCmd, RemoveNodeCmd, MoveNodeCmd, SetTransformCmd, SetPrimCmd,
                 SetColorCmd, SetOpBlendCmd, AppendStrokeCmd, TrimStrokeCmd, AddLayerCmd,
                 RemoveLayerCmd, SetLayerVisibleCmd, SetLayerTransformCmd,
                 SetLayerProtectionCmd, SetStrokePointsCmd, SetDeformersCmd,
                 SetLayerMirrorCmd, SetLayerRadialCmd, SetArmatureCmd, SetLayerNameCmd>;

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
math::Aabb command_influence_bound(const Document& doc, const Command& cmd);

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
inline constexpr std::uint16_t kSceneMinor = 15;

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

// Whole-document snapshot (used by tests for bit-identity checks and by the
// io module as the scene chunk payload).
// `minor` is the layout to WRITE at, defaulting to the current one. Writing at
// an older layout is what lets a build produce a file an older one can open —
// and it is what makes "a minor-1 document reads as hard corners" testable
// without manufacturing a stream by hand, which only stays correct until the
// next field is added.
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
    bool grouping_ = false;
};

}  // namespace scene
}  // namespace clay
