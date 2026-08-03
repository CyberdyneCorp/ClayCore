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
        if (index < 0 || index > static_cast<int>(siblings->size()))
            siblings->push_back(id);
        else
            siblings->insert(siblings->begin() + index, id);
        return id;
    }

    // Remove a node and its subtree; returns the flattened subtree
    // (preorder, ids preserved) so a command inverse can reinsert it.
    std::vector<Node> remove(NodeId id) {
        std::vector<Node> out;
        NodeId parent = kNoNode;
        int index = -1;
        if (!locate(id, &parent, &index)) return out;
        std::vector<NodeId>* siblings = parent == kNoNode ? &roots : &find_mut(parent)->children;
        siblings->erase(siblings->begin() + index);
        collect(id, out);
        for (const Node& n : out) nodes_.erase(n.id);
        return out;
    }

    // Reinsert a subtree produced by remove().
    bool reinsert(const std::vector<Node>& subtree, NodeId parent, int index) {
        if (subtree.empty()) return false;
        for (const Node& n : subtree) {
            nodes_.emplace(n.id, n);
            next_id_ = n.id >= next_id_ ? n.id + 1 : next_id_;
        }
        std::vector<NodeId>* siblings = &roots;
        if (parent != kNoNode) {
            Node* g = find_mut(parent);
            if (!g || !g->is_group) return false;
            siblings = &g->children;
        }
        if (index < 0 || index > static_cast<int>(siblings->size()))
            siblings->push_back(subtree.front().id);
        else
            siblings->insert(siblings->begin() + index, subtree.front().id);
        return true;
    }

    // Parent (kNoNode = root list) and sibling index of a node.
    bool locate(NodeId id, NodeId* parent, int* index) const {
        for (std::size_t i = 0; i < roots.size(); ++i)
            if (roots[i] == id) {
                *parent = kNoNode;
                *index = static_cast<int>(i);
                return true;
            }
        for (const auto& [nid, n] : nodes_) {
            for (std::size_t i = 0; i < n.children.size(); ++i)
                if (n.children[i] == id) {
                    *parent = nid;
                    *index = static_cast<int>(i);
                    return true;
                }
        }
        return false;
    }

    bool move(NodeId id, NodeId new_parent, int new_index) {
        NodeId parent;
        int index;
        if (!locate(id, &parent, &index)) return false;
        // detach without destroying
        std::vector<NodeId>* siblings = parent == kNoNode ? &roots : &find_mut(parent)->children;
        siblings->erase(siblings->begin() + index);
        std::vector<NodeId>* dest = &roots;
        if (new_parent != kNoNode) {
            Node* g = find_mut(new_parent);
            if (!g || !g->is_group) {  // restore and fail
                siblings->insert(siblings->begin() + index, id);
                return false;
            }
            dest = &g->children;
        }
        if (new_index < 0 || new_index > static_cast<int>(dest->size()))
            dest->push_back(id);
        else
            dest->insert(dest->begin() + new_index, id);
        return true;
    }

    const std::unordered_map<NodeId, Node>& nodes() const { return nodes_; }

  private:
    void collect(NodeId id, std::vector<Node>& out) const {
        const Node* n = find(id);
        if (!n) return;
        out.push_back(*n);
        for (NodeId c : n->children) collect(c, out);
    }

    std::unordered_map<NodeId, Node> nodes_;
    NodeId next_id_ = 1;
};

enum class LayerKind : std::uint8_t { Sdf = 0, Voxel = 1 };

inline constexpr std::uint8_t kMirrorX = 1;
inline constexpr std::uint8_t kMirrorY = 2;
inline constexpr std::uint8_t kMirrorZ = 4;

struct Layer {
    LayerId id = 0;
    std::string name;
    LayerKind kind = LayerKind::Sdf;
    math::Transform xform;
    bool visible = true;
    int resolution = 256;
    std::uint8_t mirror_axes = 0;  // kMirrorX|Y|Z — item-level mirror flag folds here
    float mirror_k = 0.0f;         // Mirror Blend seam smoothing
    std::shared_ptr<SdfContent> sdf;  // shared between instances
    // voxel content lands with the voxel-engine group
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
