# c-abi

## ADDED Requirements

### Requirement: Undo reports the region it changed
The C API SHALL offer undo and redo entry points that additionally report the world-space INFLUENCE bound of what they applied, in the three-state shape the influence-bound queries already use: nothing changed, a finite box, or unbounded. The bound SHALL be usable directly as the region argument of the brick cache's dirty marking, with the unbounded state spelled the way that call already spells it.

Without this the narrowest region a host can honestly dirty after an undo is the whole layer, because nothing in the ABI says which nodes the step touched. Every alternative available to the host is worse: diffing the layer's nodes across the call misses an in-place change — an undone move, resize or colour edit keeps its node id — and under-dirtying leaves stale bricks at a blend seam, which is silent and on screen.

The bound SHALL be the union, over every command in the step, of what that command targets BEFORE it is applied and AFTER it is applied. One side alone cannot see a move (which has two ends), a removal (whose node is gone afterwards) or an add (whose node was not there before).

The bound MAY be larger than the region that actually changed and SHALL NOT be smaller. Where being tight would cost correctness it SHALL be conservative: a node inside a group SHALL report its root ancestor's bound, because the group's blend spreads a child's influence past the child's own box; and a command on content shared by instanced layers SHALL report the union over every layer that shares that content.

A command that cannot change what the document evaluates to SHALL contribute nothing to the bound, so a step made only of such commands reports that there is nothing to dirty rather than reporting the layer.

The existing undo and redo entry points SHALL keep their signatures and their behaviour, and the reporting variants SHALL agree with them on what was undone and on the resulting document.

#### Scenario: Undoing a dab dirties the dab
- **WHEN** an item is added to a populated layer and the add is undone through the reporting variant
- **THEN** the reported bound contains the added item's influence bound and is strictly smaller than the layer's

#### Scenario: A move is bounded at both ends
- **WHEN** an item is transformed far from where it was and the transform is undone
- **THEN** the reported bound contains both the position it was moved to and the position it was moved back to

#### Scenario: An undone removal is bounded by what came back
- **WHEN** a node is removed and the removal is undone, restoring it
- **THEN** the reported bound contains the restored node's influence bound

#### Scenario: A group's blend is not cut off
- **WHEN** a child of a smooth-blended group is edited and the edit is undone
- **THEN** the reported bound is the group's influence bound, which reaches past the child's own box by the group's blend support

#### Scenario: An instanced layer is not missed
- **WHEN** a layer is instanced, an edit is made through one instance, and that edit is undone
- **THEN** the reported bound covers where the change lands in every layer sharing the content, not only the layer named by the command

#### Scenario: Unbounded influence is expressible
- **WHEN** the undone step touched a node whose influence has no finite extent
- **THEN** the call reports the unbounded state rather than a finite box, and the host's honest response is to dirty everything

#### Scenario: An edit that changes no field dirties nothing
- **WHEN** a layer rename is undone
- **THEN** the call reports that there is nothing to dirty

#### Scenario: Nothing to undo is still not an error
- **WHEN** the reporting variant is called on a document with nothing to undo
- **THEN** it reports that nothing was undone, reports nothing to dirty, and returns success
