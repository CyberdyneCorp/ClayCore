# scene-model

## MODIFIED Requirements

### Requirement: One undo order spans every representation

The session history SHALL order steps across the SDF edit list, voxel grids,
masks and mesh layers, so that undo reverses what happened most recently
whichever representation produced it.

An explicit group SHALL bundle arbitrary STEPS, of any representation, into one
step — not merely the scene commands inside it. The wrapped command stack
already collapses the commands of a bracket into one entry; a bracket SHALL do
the same for the kinds that stack cannot see, so that one gesture a host
bracketed is one undo however many representations it touched.

A step folded from a bracket SHALL apply its parts backwards on undo and
forwards on redo, and SHALL be all-or-nothing: if any part refuses, the parts
already applied SHALL be restored and the step SHALL remain on the stack.

An operation nothing records SHALL NOT be folded into a group. It stays its own
step, so that the horizon a host draws from it is not crossed by an undo.

#### Scenario: A group holds a command and a voxel pass
- **GIVEN** a history with undo enabled
- **WHEN** a bracket contains one scene command and one voxel step
- **THEN** the undo depth is one
- **AND** one undo reverses both

#### Scenario: A group of commands alone is unchanged
- **GIVEN** a history with undo enabled
- **WHEN** a bracket contains only scene commands
- **THEN** it records exactly one step, as it did before groups spanned kinds

#### Scenario: A barrier in a group is not swallowed
- **GIVEN** a history with undo enabled
- **WHEN** a bracket contains a voxel step and an operation nothing records
- **THEN** the unrecordable operation is still its own step
- **AND** the undo depth stops at it

#### Scenario: A part that refuses leaves the step unapplied
- **GIVEN** a folded group whose voxel part names a layer that cannot be resolved
- **WHEN** the step is undone
- **THEN** the undo is refused
- **AND** the scene part it had already reversed is restored
