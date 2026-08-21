# Proposal: say what undo actually covers, and what it does not

## Why

`scene-model` requires that "every document mutation SHALL be expressed as a
serializable command with a computable inverse", and lists the vocabulary:

> add/remove item, set parameter, **voxel-span edit**, layer
> add/remove/reorder/retransform, group/ungroup

**There is no voxel command.** `Command` is a `std::variant` of nineteen
alternatives and not one of them edits a voxel grid; `include/clay/scene/commands.h`
mentions the word "voxel" exactly once, in a comment about a serialization
minor. The requirement has been false for as long as voxel layers have existed,
and nothing catches it, because a spec sentence has no test.

That matters more than a wrong word. A host reading this requirement will build
one undo button and expect it to cover the document. What it actually gets is
**three unrelated history mechanisms, one per representation**:

| Representation | Mechanism | Reaches undo? |
|---|---|---|
| SDF edit list, layer properties | `UndoStack` over `Command` | yes |
| Voxel grid | sculpt layers — record a pass, dial its strength, merge it down | no |
| Mesh layer | `MeshDeltas::revert` | no |

Each is defensible on its own. A voxel edit has no compact inverse — the
inverse of "carve here" is the cells that were there — which is exactly why the
voxel side records passes instead. But **a single user-visible undo does not
span them**, and no document says so. A host discovers it by shipping.

The rest of this change writes down the workflow guarantees the ROADMAP has
been carrying under "Requirements taken from their bugs" since they were
collected, with the note "worth writing into the specs they touch". They never
were. Three of the seven turn out to be **already guaranteed** — stating them
costs nothing and makes them testable; the others are named as gaps in the
ROADMAP rather than invented here.

## What changes

- `scene-model`'s undo vocabulary requirement is corrected to describe the
  vocabulary that exists, and to state the boundary rather than leave it to be
  found.
- The workflow guarantees that already hold are added as requirements with
  scenarios: protection beats every operation including reordering, hidden is
  not deleted, and the symmetry plane is explicit, persistent and moves with
  the layer.

## What this does NOT change

No code. Every added requirement is a property the engine already has, pinned
by a scenario that passes today; the corrected one replaces a false sentence
with a true one. Making undo span representations is a real change and is
scoped separately — see `add-history-budget` and the ROADMAP.
