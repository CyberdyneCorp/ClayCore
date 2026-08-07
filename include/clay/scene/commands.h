#pragma once

// The one command vocabulary shared by the in-memory undo stack and the
// document file format (scene-model spec): every mutation is a serializable
// command with a computable inverse; stroke commands coalesce into single
// undo steps.

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "clay/scene/document.h"

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

using Command =
    std::variant<AddNodeCmd, RemoveNodeCmd, MoveNodeCmd, SetTransformCmd, SetPrimCmd,
                 SetColorCmd, SetOpBlendCmd, AppendStrokeCmd, TrimStrokeCmd, AddLayerCmd,
                 RemoveLayerCmd, SetLayerVisibleCmd, SetLayerTransformCmd,
                 SetLayerProtectionCmd, SetStrokePointsCmd>;

// The layer a command would edit, or 0 for one that edits no existing layer
// (adding a layer creates its target; changing protection is how a protected
// layer is released). Exposed because apply() returns nullopt for both "no
// such layer" and "that layer is protected", and a binding has to tell a
// caller which of the two it hit.
LayerId edited_layer(const Command& cmd);

// The scene payload layout this build writes. It tracks the .clayspace
// container's minor version, which is what a reader is told; io asserts they
// agree so the two cannot drift.
inline constexpr std::uint16_t kSceneMinor = 4;

// Apply a command; returns its inverse, or nullopt if the target does not
// exist or is protected (ghosted or locked). The document is unchanged in
// either case.
std::optional<Command> apply(Document& doc, const Command& cmd);

// Binary serialization (the same encoding the document format's command
// chunks use).
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
    bool undo(Document& doc);
    bool redo(Document& doc);
    void begin_group();
    void end_group();
    std::size_t undo_depth() const { return undo_.size(); }
    std::size_t redo_depth() const { return redo_.size(); }

  private:
    struct Entry {
        std::vector<Command> inverses;  // applied in reverse order on undo
    };
    static bool try_coalesce(Entry& top, const Command& cmd, const Command& inverse);
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    bool grouping_ = false;
};

}  // namespace scene
}  // namespace clay
