#pragma once

// Tree edits for an armature (scene-model spec, add-armature).
//
// These are PURE functions over (nodes, parents): each returns the tree the
// edit produces and leaves the document alone. The command that installs one is
// SetArmatureCmd, a whole-tree replace, for the reason SetStrokePointsCmd
// already gives for curves — an armature is tens of nodes, so replacing the
// list costs less than the bookkeeping granular commands would need, and its
// inverse is exactly the tree that was there before.
//
// Keeping the semantics here rather than in the command is what makes them
// testable without a document, and what stops "move carries the subtree" being
// re-derived by every caller that needs it.

#include <cstdint>
#include <vector>

#include "clay/scene/types.h"

namespace clay {
namespace scene {

// The tree a node's descendants form, including the node itself. Order is
// ascending index, so a caller can delete from the back without invalidating
// what it has not visited yet.
std::vector<std::uint32_t> armature_subtree(const std::vector<std::uint32_t>& parents,
                                            std::uint32_t node);

// Whether `parents` is a forest: every index in range, and no cycles. A cycle
// would make the field depend on the order links are walked rather than on the
// tree, so callers refuse one rather than rendering something arbitrary.
bool armature_is_valid(const std::vector<StrokePoint>& nodes,
                       const std::vector<std::uint32_t>& parents);

// Append a node under `parent`. Returns false and leaves both alone if the
// parent does not exist.
bool armature_add_child(std::vector<StrokePoint>& nodes, std::vector<std::uint32_t>& parents,
                        std::uint32_t parent, kernel::cfloat3 pos, float radius);

// Move a node BY `delta`, carrying every descendant with it. This is the
// property the whole feature exists for: an arm hangs from a shoulder, so
// moving the shoulder moves the arm rather than stretching the link to it.
bool armature_move(std::vector<StrokePoint>& nodes, const std::vector<std::uint32_t>& parents,
                   std::uint32_t node, kernel::cfloat3 delta);

bool armature_set_radius(std::vector<StrokePoint>& nodes, std::uint32_t node, float radius);

// Delete a node and everything under it, renumbering the parents that survive.
// A root's subtree is the whole tree, which empties the armature — that is the
// caller's decision to make, not this function's to refuse.
bool armature_delete_subtree(std::vector<StrokePoint>& nodes,
                             std::vector<std::uint32_t>& parents, std::uint32_t node);

// Append a node under `parent` AND its reflection through the plane x = 0,
// under the mirror of that parent where one is known. Returns the number of
// nodes added: 2 normally, 1 when the node sits on the mirror plane and its
// reflection would be itself.
//
// Authoring symmetry, not evaluation symmetry — the layer mirror already
// reflects what is evaluated. What was missing is that building one arm builds
// the other, which is what a sculptor means by symmetry being "on".
int armature_add_child_mirrored(std::vector<StrokePoint>& nodes,
                                std::vector<std::uint32_t>& parents, std::uint32_t parent,
                                kernel::cfloat3 pos, float radius, float plane_epsilon = 1e-5f);

}  // namespace scene
}  // namespace clay
