# scene-model — a sign per armature node

Delta for `add-armature-node-signs` (#99).

## MODIFIED Requirements

### Requirement: An armature is edited as a tree
The module SHALL provide edits that add a child to a node, move a node, set a node's radius, set a node's sign, and delete a node together with its subtree. Each SHALL go through the command vocabulary, so it is undoable, refused on a protected layer, and serialised with the document.

Moving a node SHALL move its subtree with it. This is the property the feature exists for: an arm hangs from a shoulder, so moving the shoulder carries the arm rather than leaving it behind.

A node added as a child SHALL be positive; the sign edit flips it. Deleting a node SHALL take its signs with its subtree, so no node is left with another node's sign, and a negative node SHALL NOT be required to be a leaf.

#### Scenario: Moving a parent carries its subtree
- **WHEN** a node with descendants is moved
- **THEN** every descendant moves by the same displacement, and their positions relative to it are unchanged

#### Scenario: Deleting a node takes its subtree
- **WHEN** a node with descendants is deleted
- **THEN** the descendants go with it, and no node is left naming a parent that no longer exists

#### Scenario: Every armature edit undoes exactly
- **WHEN** any armature edit is applied and undone
- **THEN** the document matches what it was before, including the tree's shape, every radius and every sign

#### Scenario: Deleting a subtree keeps the survivors' signs
- **WHEN** a subtree containing a negative node is deleted from a rig whose other nodes mix signs
- **THEN** every surviving node keeps its own sign under the renumbering

### Requirement: An armature persists with the document
A document containing an armature SHALL save and reload evaluating bit-identically, carrying the tree's shape and signs as well as its positions and radii.

The format SHALL stay backward-open: a reader that does not know armatures SHALL skip one rather than refusing the document.

#### Scenario: An armature round trips
- **WHEN** a document containing a branching armature is saved and reloaded
- **THEN** it evaluates bit-identically and reserialises to identical bytes

#### Scenario: A negative node survives the round trip
- **WHEN** a document containing an armature with a negative node is saved and reloaded
- **THEN** the sign is still there, the hollow still evaluates, and the node can be flipped positive again
