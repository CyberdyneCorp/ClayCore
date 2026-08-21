# scene-model

## MODIFIED Requirements

### Requirement: Undo command vocabulary
Every mutation of the SDF EDIT LIST AND OF LAYER STATE SHALL be expressed as a serializable command with a computable inverse: add/remove/reorder item, set parameter, set transform, set deformers, append/trim stroke, layer add/remove/reorder/retransform/rename, visibility, protection, mirror, armature. The in-memory undo stack and the document file format SHALL share this single command vocabulary. Consecutive commands from one stroke SHALL be coalescable into a single undo step. Item state carried by commands SHALL include any deformer chain, so deformed documents round-trip.

**The vocabulary SHALL NOT be described as covering every document mutation, because it does not.** Voxel-grid edits and mesh-layer vertex edits are not commands and have never been: a voxel edit has no compact inverse — the inverse of a carve is the cells it removed — and a mesh edit's is a sparse vertex delta. Each representation therefore carries its own history mechanism:

| Representation | Mechanism | Reachable from `undo()` |
|---|---|---|
| SDF edit list, layer state | the command vocabulary above | yes |
| Voxel grid | sculpt layers — record a pass, dial its strength, reorder, merge down | no |
| Mesh layer | sparse vertex deltas, reverted against the sculptor | no |

A host SHALL be able to discover this from the specification rather than from behaviour. **A single user-visible undo step does not span two representations**, and a host that presents one undo button over all three is presenting something the engine does not implement. Whether it should is a separate question, scoped in the ROADMAP rather than assumed here.

#### Scenario: Command inverse restores state
- **WHEN** any command from the vocabulary is applied to a document and then its inverse is applied
- **THEN** the document state is bit-identical to the original (verified by serialization comparison)

#### Scenario: Stroke coalescing
- **WHEN** a sculpt stroke generates N incremental point-append commands followed by stroke end
- **THEN** undo removes the entire stroke as one step

#### Scenario: Deformed item round trip
- **WHEN** a document containing an item with a deformer chain is serialized and reloaded
- **THEN** the reloaded document evaluates bit-identically and re-serializes to identical bytes

#### Scenario: A host application undoes through the engine
- **WHEN** a binding performs an edit on a document with undo enabled and then undoes it
- **THEN** the document serializes bit-identically to its state before the edit

#### Scenario: A voxel edit is not on the undo stack
- **WHEN** undo is enabled, a voxel layer is edited, and the undo depth is read before and after
- **THEN** the depth is unchanged, and undo does not restore the edited cells

## ADDED Requirements

### Requirement: Protection refuses reordering, not only editing
A ghosted or locked layer SHALL refuse every operation that mutates it, and REORDERING THE LAYER SHALL COUNT AS ONE. Protection is checked before the operation rather than by the operation, so a host rearranging a stack SHALL clear protection first.

This is stated because the opposite is the natural assumption: a reorder changes no geometry, so a host reasonably expects it to pass. It does not, and discovering that from a failed call in the field is worse than reading it here. Reading is never editing — a ghosted, locked or hidden layer answers every query normally.

#### Scenario: A ghosted layer refuses to move
- **WHEN** a layer is ghosted and the host asks to move it to another index
- **THEN** the call is refused, the stack order is unchanged, and clearing the ghost makes the same call succeed

#### Scenario: A locked layer refuses to move
- **WHEN** a layer is locked and the host asks to move it to another index
- **THEN** the call is refused and the stack order is unchanged

### Requirement: Hidden is not deleted
Hiding a layer SHALL remove its contribution from evaluation WITHOUT discarding what it holds. The hidden state SHALL persist through save and load, and showing the layer again SHALL restore its contribution exactly.

Taken from a competitor's known failure, where hidden geometry was lost across a resampling or a save. Here it costs nothing to guarantee, because visibility is a flag evaluation reads rather than an edit applied to content.

#### Scenario: A hidden layer contributes nothing and keeps everything
- **WHEN** a layer holding a sphere is hidden, the document is saved and reloaded, and the layer is shown again
- **THEN** the field reads unaffected while hidden, still unaffected after the reload, and identical to the original once shown

### Requirement: The symmetry plane is explicit and moves with its layer
A layer's mirror SHALL reflect through the plane where the layer-LOCAL coordinate of the enabled axis is zero, so that retransforming the layer MOVES THE PLANE with it. The plane SHALL persist in the saved document.

The symmetry centre is therefore explicit, persistent and reachable by the same gizmo that moves a layer, rather than implicitly pinned to the world origin — the requirement taken from a competitor whose symmetry centre could not be moved once sculpting began.

#### Scenario: Moving the layer moves the plane
- **WHEN** a layer mirrored about x carries an off-centre lump, and the layer is then translated along x
- **THEN** the reflected lump moves with it, and the world position that held the reflection before the move no longer does
