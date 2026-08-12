#pragma once

// Document model: layers holding shared content, node arenas, selection.
// Layer instancing shares SdfContent by reference — editing the source
// updates every instance (scene-model spec).

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "clay/scene/types.h"

namespace clay {
namespace scene {

// Node arena for one SDF edit tree. Ids are stable and never reused within
// a content's lifetime.
//
// A location index (id -> parent + sibling index) rides along with every
// structural edit, so locate() — which undo pays once per removed node —
// costs a hash lookup instead of a walk over the whole arena. Undoing a
// 100-stamp stroke on a 10k-node document paid 100 such walks (~0.6 ms,
// linear in the document); with the index it is flat in document size.
class SdfContent {
  public:
    std::vector<NodeId> roots;

    const Node* find(NodeId id) const {
        auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : &it->second;
    }
    Node* find_mut(NodeId id) {
        auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : &it->second;
    }

    // Insert a node (fresh id assigned unless the node carries one that is
    // unused — command replay preserves ids). parent = kNoNode -> root list.
    // index = -1 appends. Returns the id, or kNoNode on bad parent.
    NodeId insert(Node node, NodeId parent = kNoNode, int index = -1) {
        if (node.id == kNoNode || nodes_.count(node.id)) node.id = next_id_;
        next_id_ = node.id >= next_id_ ? node.id + 1 : next_id_;
        std::vector<NodeId>* siblings = &roots;
        if (parent != kNoNode) {
            Node* g = find_mut(parent);
            if (!g || !g->is_group) return kNoNode;
            siblings = &g->children;
        }
        NodeId id = node.id;
        nodes_.emplace(id, std::move(node));
        place(*siblings, parent, id, index);
        return id;
    }

    // Reserve a fresh id without inserting, so a brand-new insertion can be
    // expressed as an AddNodeCmd (whose replay preserves ids) and thereby be
    // recorded by an enabled undo stack like every other edit.
    NodeId reserve_id() { return next_id_++; }

    // Remove a node and its subtree; returns the flattened subtree
    // (preorder, ids preserved) so a command inverse can reinsert it.
    std::vector<Node> remove(NodeId id) {
        std::vector<Node> out;
        NodeId parent = kNoNode;
        int index = -1;
        if (!locate(id, &parent, &index)) return out;
        std::vector<NodeId>* siblings = parent == kNoNode ? &roots : &find_mut(parent)->children;
        siblings->erase(siblings->begin() + index);
        reindex(*siblings, parent, static_cast<std::size_t>(index));
        collect(id, out);
        for (const Node& n : out) {
            nodes_.erase(n.id);
            where_.erase(n.id);
        }
        return out;
    }

    // Reinsert a subtree produced by remove().
    bool reinsert(const std::vector<Node>& subtree, NodeId parent, int index) {
        if (subtree.empty()) return false;
        for (const Node& n : subtree) {
            auto stored = nodes_.emplace(n.id, n).first;
            next_id_ = n.id >= next_id_ ? n.id + 1 : next_id_;
            reindex(stored->second.children, n.id, 0);
        }
        std::vector<NodeId>* siblings = &roots;
        if (parent != kNoNode) {
            Node* g = find_mut(parent);
            if (!g || !g->is_group) return false;
            siblings = &g->children;
        }
        place(*siblings, parent, subtree.front().id, index);
        return true;
    }

    // Parent (kNoNode = root list) and sibling index of a node. The indexed
    // answer is verified against the tree before it is trusted, because
    // `roots` is a public member a caller can rewrite directly; a stale
    // entry falls through to the full walk and repairs itself there.
    bool locate(NodeId id, NodeId* parent, int* index) const {
        auto it = where_.find(id);
        if (it != where_.end() && verified(id, it->second)) {
            *parent = it->second.parent;
            *index = static_cast<int>(it->second.index);
            return true;
        }
        return locate_by_walk(id, parent, index);
    }

    // Rebuild the location entries for the root list, for code that rewrites
    // `roots` wholesale (the document reader does) rather than editing
    // through insert/remove/move.
    void reindex_roots() { reindex(roots, kNoNode, 0); }

    // Is `id` at or below `root`? The cycle test move() needs.
    bool contains(NodeId root, NodeId id) const {
        if (root == id) return true;
        const Node* n = find(root);
        if (!n) return false;
        for (NodeId c : n->children)
            if (contains(c, id)) return true;
        return false;
    }

    bool move(NodeId id, NodeId new_parent, int new_index) {
        NodeId parent = kNoNode;
        int index = -1;
        if (!locate(id, &parent, &index)) return false;
        // A node cannot move into its own subtree. Detaching it and then
        // reattaching it below itself closes a cycle: the subtree leaves the
        // root list, so it stops evaluating, is dropped on save (write_content
        // walks from roots) and can no longer be reached by remove().
        if (new_parent != kNoNode && contains(id, new_parent)) return false;
        std::vector<NodeId>* dest = &roots;
        if (new_parent != kNoNode) {
            Node* g = find_mut(new_parent);
            if (!g || !g->is_group) return false;
            dest = &g->children;
        }
        // detach without destroying
        std::vector<NodeId>* siblings = parent == kNoNode ? &roots : &find_mut(parent)->children;
        siblings->erase(siblings->begin() + index);
        reindex(*siblings, parent, static_cast<std::size_t>(index));
        place(*dest, new_parent, id, new_index);
        return true;
    }

    const std::unordered_map<NodeId, Node>& nodes() const { return nodes_; }

  private:
    struct Location {
        NodeId parent = kNoNode;
        std::uint32_t index = 0;
    };

    void collect(NodeId id, std::vector<Node>& out) const {
        const Node* n = find(id);
        if (!n) return;
        out.push_back(*n);
        for (NodeId c : n->children) collect(c, out);
    }

    // Insert `id` into `siblings` at `index` (append when out of range) and
    // refresh the location of it and of every sibling the insert shifted.
    void place(std::vector<NodeId>& siblings, NodeId parent, NodeId id, int index) {
        std::size_t at = (index < 0 || index > static_cast<int>(siblings.size()))
                             ? siblings.size()
                             : static_cast<std::size_t>(index);
        siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(at), id);
        reindex(siblings, parent, at);
    }

    void reindex(const std::vector<NodeId>& siblings, NodeId parent, std::size_t from) const {
        for (std::size_t i = from; i < siblings.size(); ++i)
            where_[siblings[i]] = Location{parent, static_cast<std::uint32_t>(i)};
    }

    const std::vector<NodeId>* sibling_list(NodeId parent) const {
        if (parent == kNoNode) return &roots;
        const Node* n = find(parent);
        return n ? &n->children : nullptr;
    }

    bool verified(NodeId id, const Location& loc) const {
        const std::vector<NodeId>* s = sibling_list(loc.parent);
        return s && loc.index < s->size() && (*s)[loc.index] == id;
    }

    // The pre-index locate: scan the root list, then every node's children.
    // Kept as the fallback that makes a stale index self-repairing.
    bool locate_by_walk(NodeId id, NodeId* parent, int* index) const {
        for (std::size_t i = 0; i < roots.size(); ++i)
            if (roots[i] == id) {
                where_[id] = Location{kNoNode, static_cast<std::uint32_t>(i)};
                *parent = kNoNode;
                *index = static_cast<int>(i);
                return true;
            }
        for (const auto& [nid, n] : nodes_) {
            for (std::size_t i = 0; i < n.children.size(); ++i)
                if (n.children[i] == id) {
                    where_[id] = Location{nid, static_cast<std::uint32_t>(i)};
                    *parent = nid;
                    *index = static_cast<int>(i);
                    return true;
                }
        }
        where_.erase(id);
        return false;
    }

    std::unordered_map<NodeId, Node> nodes_;
    // id -> position, mutable because locate() is const and repairs it.
    mutable std::unordered_map<NodeId, Location> where_;
    NodeId next_id_ = 1;
};

// Appended, so the existing enumerators keep their values and the layer
// record's kind byte keeps its layout. A build that predates a kind reads it
// into the fixed uint8_t underlying type and skips the layer at every
// `kind == Sdf` gate, which is how a non-SDF kind costs no field semantics.
enum class LayerKind : std::uint8_t { Sdf = 0, Voxel = 1, Mesh = 2 };

inline constexpr std::uint8_t kMirrorX = 1;
inline constexpr std::uint8_t kMirrorY = 2;
inline constexpr std::uint8_t kMirrorZ = 4;

struct Layer {
    LayerId id = 0;
    std::string name;
    LayerKind kind = LayerKind::Sdf;
    math::Transform xform;
    bool visible = true;
    // Protection, both off by default so a document that never sets them
    // behaves exactly as it did before they existed. Ghost is "show me this
    // but stay out of my way": still evaluated, never picked, never edited.
    // Locked is "this is finished": still picked, never edited. Neither
    // changes what the layer evaluates to — how a host DRAWS a ghost is its
    // own business, and this engine is headless.
    bool ghost = false;
    bool locked = false;
    bool protected_from_edits() const { return ghost || locked; }
    int resolution = 256;
    std::uint8_t mirror_axes = 0;  // kMirrorX|Y|Z — item-level mirror flag folds here
    float mirror_k = 0.0f;         // Mirror Blend seam smoothing
    std::shared_ptr<SdfContent> sdf;  // shared between instances
    // Voxel and mesh content live beside the document, keyed by layer id (see
    // io::ClaySpaceDoc): the layering table withholds both modules from
    // clay::scene, which is what makes "this content does not change what the
    // document evaluates to" structural rather than a rule to maintain.
};

class Document {
  public:
    std::vector<Layer> layers;
    std::vector<NodeId> selection;

    Layer& add_sdf_layer(std::string name) {
        Layer l;
        l.id = next_layer_id_++;
        l.name = std::move(name);
        l.sdf = std::make_shared<SdfContent>();
        layers.push_back(std::move(l));
        return layers.back();
    }

    // Reserve a fresh layer id without inserting — the AddLayerCmd analogue
    // of SdfContent::reserve_id, for bindings that add layers through the
    // command vocabulary so the add is undoable.
    LayerId reserve_layer_id() { return next_layer_id_++; }

    // Instance: shares the source's content by reference.
    Layer* instance_layer(LayerId src_id, std::string name) {
        Layer* src = find_layer(src_id);
        if (!src || src->kind != LayerKind::Sdf) return nullptr;
        Layer l = *src;
        l.id = next_layer_id_++;
        l.name = std::move(name);
        layers.push_back(std::move(l));
        return &layers.back();
    }

    Layer* find_layer(LayerId id) {
        for (Layer& l : layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    const Layer* find_layer(LayerId id) const {
        for (const Layer& l : layers)
            if (l.id == id) return &l;
        return nullptr;
    }

    bool remove_layer(LayerId id, Layer* removed = nullptr, int* index = nullptr) {
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (layers[i].id == id) {
                if (removed) *removed = layers[i];
                if (index) *index = static_cast<int>(i);
                layers.erase(layers.begin() + i);
                return true;
            }
        return false;
    }

    void insert_layer(Layer layer, int index) {
        next_layer_id_ = layer.id >= next_layer_id_ ? layer.id + 1 : next_layer_id_;
        if (index < 0 || index > static_cast<int>(layers.size()))
            layers.push_back(std::move(layer));
        else
            layers.insert(layers.begin() + index, std::move(layer));
    }

  private:
    LayerId next_layer_id_ = 1;
};

}  // namespace scene
}  // namespace clay
