# scene-model — armatures

Delta for `add-armature`.

## ADDED Requirements

### Requirement: An armature is edited as a tree
The module SHALL provide edits that add a child to a node, move a node, set a node's radius, and delete a node together with its subtree. Each SHALL go through the command vocabulary, so it is undoable, refused on a protected layer, and serialised with the document.

Moving a node SHALL move its subtree with it. This is the property the feature exists for: an arm hangs from a shoulder, so moving the shoulder carries the arm rather than leaving it behind.

#### Scenario: Moving a parent carries its subtree
- **WHEN** a node with descendants is moved
- **THEN** every descendant moves by the same displacement, and their positions relative to it are unchanged

#### Scenario: Deleting a node takes its subtree
- **WHEN** a node with descendants is deleted
- **THEN** the descendants go with it, and no node is left naming a parent that no longer exists

#### Scenario: Every armature edit undoes exactly
- **WHEN** any armature edit is applied and undone
- **THEN** the document matches what it was before, including the tree's shape and every radius

### Requirement: An armature can be authored symmetrically
Adding a node with mirroring SHALL add the node and its reflection through the layer's mirror as ONE undo step, following the precedent the voxel mirrored write already sets.

This is an authoring rule rather than a field one: the layer mirror already reflects what is evaluated, and what is missing is that building one arm builds the other.

#### Scenario: A mirrored insert is one step
- **WHEN** a child is added with mirroring on, and the edit is undone
- **THEN** both the node and its reflection are gone, in one undo

### Requirement: An armature persists with the document
A document containing an armature SHALL save and reload evaluating bit-identically, carrying the tree's shape as well as its positions and radii.

The format SHALL stay backward-open: a reader that does not know armatures SHALL skip one rather than refusing the document.

#### Scenario: An armature round trips
- **WHEN** a document containing a branching armature is saved and reloaded
- **THEN** it evaluates bit-identically and reserialises to identical bytes
