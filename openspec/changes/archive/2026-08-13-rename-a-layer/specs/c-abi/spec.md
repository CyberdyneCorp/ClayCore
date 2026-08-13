# c-abi — a layer can be renamed

Delta for `rename-a-layer` (#92).

## ADDED Requirements

### Requirement: A host can rename a layer
The C API SHALL let a host change a layer's name after creation, so that the name a document persists is the name the artist chose. Until this existed a layer was named only by the call that created it, the rename lived in the host alone and was lost on the next save, and the layer-discovery surface reported the stale creation name back with no way to tell it from a current one.

The rename SHALL go through the same command vocabulary as every other layer mutation: one rename is ONE undo step whose inverse restores the previous name exactly, and a ghosted or locked layer SHALL refuse it with the typed invalid-argument refusal that every other edit of a protected layer gives, rather than applying it silently.

A NULL name and an empty name SHALL be refused, and a refused rename SHALL leave the name unchanged. The name SHALL have no length limit imposed by this call: the saved layer record length-prefixes it and the name query sizes before it writes, so a long name costs a reader a larger buffer rather than a truncation.

Names SHALL NOT be required to be unique, because the calls that create layers have never required it and a uniqueness enforced on renames alone would be a guarantee the document does not keep. Instead the meaning of a duplicate SHALL be documented at the call: the by-name layer lookups answer with the FIRST layer in stack order carrying the name, so a rename onto a name already in use shadows the other layer rather than rebinding it, and a host whose lookup must survive a rename holds the layer id, which is stable across a save and reload.

The addition SHALL be purely additive — no existing signature changes, no enumerator's value changes, and no document format version moves, since the layer record has always carried the name.

#### Scenario: A rename survives a save and reload
- **WHEN** a layer is created, renamed, and the document is saved and loaded into a fresh document
- **THEN** the layer's name reads back as the NEW name, for an SDF, a voxel and a mesh layer alike

#### Scenario: A rename is one undo step
- **WHEN** a layer is renamed twice with undo enabled and undone twice
- **THEN** each undo restores exactly the previous name, ending at the creation name, and redo re-applies each rename in order

#### Scenario: The by-name lookup follows the rename
- **WHEN** a voxel layer is renamed and its grid is looked up by the new name
- **THEN** the lookup answers with that layer's grid, and the old name is refused as not found

#### Scenario: A duplicate name shadows rather than rebinds
- **WHEN** a layer is renamed to a name another layer already carries
- **THEN** the rename is accepted, both layers keep the name, and the by-name lookup answers with whichever of them is first in stack order

#### Scenario: A protected layer refuses the rename
- **WHEN** a ghosted or locked layer is renamed
- **THEN** the call is refused as an invalid argument, the name is unchanged, and releasing the protection makes the rename possible again

#### Scenario: An unusable name is refused before it replaces a good one
- **WHEN** a rename is called with a null document, a null name, an empty name, or an id no layer has
- **THEN** it is refused — invalid argument for the first three, not found for the last — and no layer's name changes
