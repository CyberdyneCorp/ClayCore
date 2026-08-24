#pragma once

// WHAT THE EDIT LIST COSTS.
//
// `command_bytes` already answers this for the undo stack, and it had to,
// because a history without a budget is how a long session gets killed by iOS.
// It answered it only for the history: the DOCUMENT the history records against
// went unmeasured, which is the larger of the two on any document holding real
// work.
//
// WHAT sizeof DOES NOT SEE, and it is most of a node. A `Node` is a fixed
// struct with four heap members hanging off it — its stroke points, its
// deformer chain, a bend deformer's own guide curve, and a group's child list.
// A spline stroke of a thousand points and an empty sphere report the same
// `sizeof`. So does an armature-deformed node against a bare one. That is why
// this walks rather than multiplies.

#include <cstddef>
#include <unordered_set>

#include "clay/scene/document.h"

namespace clay {
namespace scene {

// Payloads that more than one owner can point at: an instance layer's
// `SdfContent`, and the `FieldVolume` a node samples. Counting one of these
// once per owner reports memory that was never allocated — which is worse than
// useless to a host trying to decide what to release, because it invites
// freeing something to recover bytes that do not exist.
//
// Pass ONE of these through a whole report and each payload is counted the
// first time it is seen. Pass none and every payload is counted in full, which
// is what a single node's cost means on its own.
using SharedSeen = std::unordered_set<const void*>;

// One node INCLUDING what hangs off it. Shared with `command_bytes`, which
// needs exactly the same walk for the nodes an inverse carries — two copies of
// this would drift the moment `Node` grew another heap member, and that is not
// hypothetical: the private copy this replaced had fallen SIX members behind.
//
// It counted stroke points, the deformer chain, a bend guide and the child
// list, and missed the armature binding, the three profile arrays (one of them
// a vector of vectors), a lattice deformer's cage, and BOTH sampled volumes —
// which are typically the largest thing a node owns by two orders of magnitude.
// So `add-history-budget`'s byte figure was low, and lowest exactly on the
// documents where a budget matters.
std::size_t node_bytes(const Node& n, SharedSeen* seen = nullptr);

// A whole edit list: the node table, the root and child lists, layer records
// and their names.
//
// A layer's `sdf` is a `shared_ptr` and an INSTANCE layer shares its source's
// content. Shared content is therefore counted ONCE, not once per instance —
// counting it per layer would report ten instances of one blockout as ten
// copies of memory that was never allocated, which is the opposite of what a
// host under memory pressure needs to hear. The same holds for a sampled
// volume two nodes point at.
std::size_t document_bytes(const Document& doc);

}  // namespace scene
}  // namespace clay
