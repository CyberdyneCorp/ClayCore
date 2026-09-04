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
    // The whole layer's PER-AXIS scale, innermost in the layer's own frame —
    // before `xform` places it, exactly as a node's sits before its own
    // placement (#373). The full map is
    // `layer.xform * diag(scale_axes) * node.xform * diag(node.scale_axes)`,
    // and the two scales at each level multiply: `xform.scale` stays the
    // uniform similarity factor and this modulates it.
    //
    // A separate field rather than a widening of math::Transform: a Transform
    // is a similarity, and the algebra it is closed under is only the
    // similarities. A per-axis scale composed into it would make every
    // `a * b` in the tree quietly wrong instead of failing to compile.
    kernel::cfloat3 scale_axes = kernel::cf3(1.0f, 1.0f, 1.0f);
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
    // Radial symmetry about the layer-local axis. 0 or 1 is off and costs
    // nothing. Participation reuses the item-level mirror flag rather than
    // adding a second one: an asymmetric detail is excluded from a layer's
    // symmetry once, not once per mode.
    std::uint16_t radial_count = 0;  // 0/1 = off, else copies INCLUDING the original
    std::uint8_t radial_axis = 1;    // 0/1/2 — Y by default, as Repeat::radial arrays
    float radial_k = 0.0f;           // seam smoothing between neighbouring copies
    std::shared_ptr<SdfContent> sdf;  // shared between instances
    // Voxel and mesh content live beside the document, keyed by layer id (see
    // io::ClaySpaceDoc): the layering table withholds both modules from
    // clay::scene, which is what makes "this content does not change what the
    // document evaluates to" structural rather than a rule to maintain.
};

// -- an item's PLACED frame, both per-axis scales composed --------------------
//
// One place each, so tape_build, bounds, pick and the two brushes cannot drift
// on the order the scales compose in or on which component a distance is
// corrected by. The node-level halves live in scene/types.h; these are the
// whole composition:
//
//   world_from_local = layer.xform · diag(layer.scale_axes)
//                    · node.xform  · diag(node.scale_axes)
//
// The uniform `xform.scale` at each level stays the similarity factor and the
// axes modulate it, so a triple of ones is the identity at either level and an
// unsquashed document takes the fast paths below unchanged.

inline bool layer_is_squashed(const Layer& l) {
    return !scale_axes_uniform(l.scale_axes) || l.scale_axes.x != 1.0f;
}

// The layer's own map, per-axis scale innermost.
inline math::cfloat4x4 layer_matrix(const Layer& l) {
    if (!layer_is_squashed(l)) return l.xform.matrix();
    return math::mul(l.xform.matrix(), math::scale_matrix(l.scale_axes));
}

inline math::cfloat4x4 layer_inverse_matrix(const Layer& l) {
    if (!layer_is_squashed(l)) return l.xform.inverse_matrix();
    return math::mul(math::inverse_scale_matrix(l.scale_axes), l.xform.inverse_matrix());
}

// The layer's own distance factor, for a length authored in the LAYER's local
// units — a group's rounding radius, which no single item owns.
inline float layer_distance_scale(const Layer& l) {
    return l.xform.scale * scale_axes_factor(l.scale_axes);
}

// The whole thing: the layer's map, then the item's own.
inline math::cfloat4x4 placed_matrix(const Layer& l, const Node& n) {
    return math::mul(layer_matrix(l), item_matrix(n));
}

inline math::cfloat4x4 placed_inverse_matrix(const Layer& l, const Node& n) {
    return math::mul(item_inverse_matrix(n), layer_inverse_matrix(l));
}

// The factor a LOCAL distance is multiplied back by. The product of the two
// similarity scales and of the smallest component of each per-axis scale.
//
// CONSERVATIVE RATHER THAN EXACT, and provably so: the composed linear part is
// `L · D_l · R · D_n` with L and R rotations, and the smallest singular value
// of a product is at least the product of the smallest singular values, which
// for a rotation is 1. So this never exceeds the true minimum stretch, and a
// distance multiplied by it never overestimates. That is `cscale_nu_dist`'s
// instinct applied twice.
inline float placed_distance_scale(const Layer& l, const Node& n) {
    return l.xform.scale * scale_axes_factor(l.scale_axes) * n.xform.scale *
           scale_axes_factor(n.scale_axes);
}

// The dual, for a WORLD radius being mapped inward: the largest component, so
// every world reach is at most the region the caller circled. Under-reach is
// recoverable by asking again; over-reach is not.
inline float placed_reach_scale(const Layer& l, const Node& n) {
    return l.xform.scale * scale_axes_reach(l.scale_axes) * n.xform.scale *
           scale_axes_reach(n.scale_axes);
}

// Whether the composition is still a similarity — the case that keeps the field
// EXACT and compiles to bit-identical tape.
inline bool placed_is_similarity(const Layer& l, const Node& n) {
    return scale_axes_uniform(l.scale_axes) && scale_axes_uniform(n.scale_axes);
}

class Document {
  public:
    std::vector<Layer> layers;
    std::vector<NodeId> selection;

    // Advances whenever a COMMAND changes this document (issue #451).
    //
    // Distinct from the ABI's `revision`, which advances at the END of an edit
    // and therefore cannot separate the document before an apply from the
    // document after it. `apply_edit` takes `command_influence_bound` on BOTH
    // sides, so a cache keyed on that revision answers the second call with the
    // first's geometry -- a bound that is too small, which is
    // under-invalidation and shows up as stale bricks rather than as an error.
    // That was measured: a memo keyed on `revision` reported one walk and zero
    // reuses per drag frame, which is only possible if both calls saw the same
    // key.
    //
    // Bumped in `scene::apply`, which is the ONE funnel every command-based
    // mutation passes through -- ordinary edits, undo, redo and a replayed
    // journal all reach it -- so nothing has to be enumerated and nothing can
    // be missed by adding a command later. A mutation made WITHOUT a command
    // (a consolidation installing a volume, say) does not reach it, and the
    // binding that owns such a path bumps this where it already invalidates.
    //
    // Runtime only: never serialized, and meaningless across documents.
    std::uint64_t content_serial = 1;

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
