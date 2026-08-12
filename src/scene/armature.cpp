#include "clay/scene/armature.h"

#include <algorithm>

namespace clay {
namespace scene {

namespace {

// Children of each node, built once so a subtree walk is linear rather than
// quadratic. A node never lists itself, so a root does not contain itself twice.
std::vector<std::vector<std::uint32_t>> children_of(const std::vector<std::uint32_t>& parents) {
    std::vector<std::vector<std::uint32_t>> kids(parents.size());
    for (std::uint32_t i = 0; i < parents.size(); ++i) {
        std::uint32_t p = parents[i];
        if (p < parents.size() && p != i) kids[p].push_back(i);
    }
    return kids;
}

}  // namespace

std::vector<std::uint32_t> armature_subtree(const std::vector<std::uint32_t>& parents,
                                            std::uint32_t node) {
    std::vector<std::uint32_t> out;
    if (node >= parents.size()) return out;
    const std::vector<std::vector<std::uint32_t>> kids = children_of(parents);
    std::vector<std::uint32_t> stack{node};
    while (!stack.empty()) {
        std::uint32_t n = stack.back();
        stack.pop_back();
        out.push_back(n);
        for (std::uint32_t c : kids[n]) stack.push_back(c);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool armature_is_valid(const std::vector<StrokePoint>& nodes,
                       const std::vector<std::uint32_t>& parents) {
    if (nodes.size() != parents.size()) return false;
    for (std::uint32_t p : parents)
        if (p >= parents.size()) return false;
    // Walking to a root must terminate. A step count past the node count means
    // the walk is going round rather than up.
    for (std::size_t i = 0; i < parents.size(); ++i) {
        std::size_t walk = i, steps = 0;
        while (parents[walk] != walk) {
            walk = parents[walk];
            if (++steps > parents.size()) return false;
        }
    }
    return true;
}

bool armature_add_child(std::vector<StrokePoint>& nodes, std::vector<std::uint32_t>& parents,
                        std::uint32_t parent, kernel::cfloat3 pos, float radius) {
    if (nodes.empty()) {
        // The first node is a root whatever it was asked to hang from: there is
        // nothing yet for it to hang from.
        StrokePoint n;
        n.pos = pos;
        n.radius = radius;
        nodes.push_back(n);
        parents.push_back(0);
        return true;
    }
    if (parent >= nodes.size()) return false;
    StrokePoint n;
    n.pos = pos;
    n.radius = radius;
    nodes.push_back(n);
    parents.push_back(parent);
    return true;
}

bool armature_move(std::vector<StrokePoint>& nodes, const std::vector<std::uint32_t>& parents,
                   std::uint32_t node, kernel::cfloat3 delta) {
    if (node >= nodes.size() || nodes.size() != parents.size()) return false;
    for (std::uint32_t i : armature_subtree(parents, node)) nodes[i].pos = nodes[i].pos + delta;
    return true;
}

bool armature_set_radius(std::vector<StrokePoint>& nodes, std::uint32_t node, float radius) {
    if (node >= nodes.size() || radius < 0.0f) return false;
    nodes[node].radius = radius;
    return true;
}

bool armature_delete_subtree(std::vector<StrokePoint>& nodes,
                             std::vector<std::uint32_t>& parents,
                             std::vector<std::int8_t>& signs, std::uint32_t node) {
    if (node >= nodes.size() || nodes.size() != parents.size()) return false;
    armature_normalize_signs(signs, nodes.size());
    const std::vector<std::uint32_t> doomed = armature_subtree(parents, node);
    std::vector<bool> drop(nodes.size(), false);
    for (std::uint32_t i : doomed) drop[i] = true;

    // Old index -> new index, so the parents that survive can be renumbered.
    // Everything about this is order-preserving, which keeps the fold order the
    // kernel depends on stable for the nodes that remain.
    std::vector<std::uint32_t> remap(nodes.size(), 0);
    std::vector<StrokePoint> kept_nodes;
    std::vector<std::uint32_t> kept_parents;
    std::vector<std::int8_t> kept_signs;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        if (drop[i]) continue;
        remap[i] = static_cast<std::uint32_t>(kept_nodes.size());
        kept_nodes.push_back(nodes[i]);
        kept_parents.push_back(parents[i]);
        kept_signs.push_back(signs[i]);
    }
    for (std::uint32_t& p : kept_parents) p = remap[p];
    nodes = std::move(kept_nodes);
    parents = std::move(kept_parents);
    signs = std::move(kept_signs);
    return true;
}

void armature_normalize_signs(std::vector<std::int8_t>& signs, std::size_t node_count) {
    signs.resize(node_count, std::int8_t{1});
}

bool armature_set_sign(std::vector<std::int8_t>& signs, std::size_t node_count,
                       std::uint32_t node, std::int8_t sign) {
    if (node >= node_count || (sign != 1 && sign != -1)) return false;
    armature_normalize_signs(signs, node_count);
    signs[node] = sign;
    return true;
}

int armature_add_child_mirrored(std::vector<StrokePoint>& nodes,
                                std::vector<std::uint32_t>& parents, std::uint32_t parent,
                                kernel::cfloat3 pos, float radius, float plane_epsilon) {
    if (!nodes.empty() && parent >= nodes.size()) return 0;
    const std::size_t before = nodes.size();
    if (!armature_add_child(nodes, parents, parent, pos, radius)) return 0;
    if (kernel::cabs(pos.x) <= plane_epsilon) return 1;  // its own reflection

    // The reflection hangs from the parent's reflection where there is one, and
    // from the same parent otherwise — a limb mirrored off a spine node has no
    // mirrored parent to find, and hanging it from the spine is what a sculptor
    // means by it.
    std::uint32_t mirror_parent = parent;
    if (before > 0) {
        const kernel::cfloat3 pp = nodes[parent].pos;
        if (kernel::cabs(pp.x) > plane_epsilon) {
            for (std::uint32_t i = 0; i < before; ++i) {
                const kernel::cfloat3 q = nodes[i].pos;
                if (kernel::cabs(q.x + pp.x) <= plane_epsilon &&
                    kernel::cabs(q.y - pp.y) <= plane_epsilon &&
                    kernel::cabs(q.z - pp.z) <= plane_epsilon) {
                    mirror_parent = i;
                    break;
                }
            }
        }
    }
    armature_add_child(nodes, parents, mirror_parent,
                       kernel::cf3(-pos.x, pos.y, pos.z), radius);
    return 2;
}

}  // namespace scene
}  // namespace clay
