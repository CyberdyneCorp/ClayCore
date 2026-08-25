# scene-model

## ADDED Requirements

### Requirement: A surface region can be named
The library SHALL provide a SURFACE GROUP: an identifier attachable to a region of a layer's surface, independent of how that layer stores its surface.

A surface point SHALL be resolvable to the group it belongs to, and a group SHALL be resolvable to the region it covers, on every representation the library holds. Where a representation cannot store a per-element id — an SDF layer has no elements — the mechanism SHALL be stated in the specification rather than left to the binding, so a host learns one concept and not three.

Group membership SHALL survive the operations that preserve a layer's identity: saving and loading, hiding and showing, transforming the layer, and reordering the stack. It SHALL NOT be claimed to survive a representation bridge, which resamples the surface; what happens across a bridge SHALL be stated explicitly and MAY be "the ids are gone".

A group SHALL support the set operations an artist expects of a selection: grow, shrink, and the border between a group and its complement. These SHALL be defined on the region rather than on the storage, so growing a group on a mesh and on a voxel grid mean the same thing.

#### Scenario: A point resolves to its group
- **WHEN** a region of a layer's surface is assigned a group and a point inside that region is queried
- **THEN** the query returns that group, and a point outside it does not

#### Scenario: Groups survive a save
- **WHEN** a document carrying surface groups is saved and reloaded
- **THEN** every group covers the same region it covered before

#### Scenario: Growing a group is defined on the region
- **WHEN** a group is grown by one step on two layers holding the same shape in different representations
- **THEN** both cover the geometrically corresponding region, within the coarser representation's resolution

### Requirement: Visibility applies to a region, not only to a layer
A host SHALL be able to hide part of a layer's surface, show it again, and invert what is hidden, addressing the part by surface group or by mask.

Hiding SHALL NOT delete. Hidden geometry SHALL persist through save and load and SHALL be restored exactly when shown, matching the guarantee a hidden LAYER already carries.

Hidden geometry SHALL be excluded from the operations that act on visible surface — evaluation for display, meshing, and picking — and a host SHALL be able to determine whether an operation respected the hidden set. **An operation that ignores hidden geometry SHALL say so**, because a brush that silently reaches hidden surface is worse than one that refuses.

#### Scenario: A hidden region contributes nothing and is not lost
- **WHEN** part of a layer is hidden, the document is saved and reloaded, and the region is shown again
- **THEN** the meshed surface omits the region while hidden, still omits it after the reload, and matches the original once shown

#### Scenario: Isolating is hiding the complement
- **WHEN** a group is isolated
- **THEN** the result is identical to hiding everything not in that group

#### Scenario: Hiding is undoable
- **WHEN** a region is hidden and the edit is undone
- **THEN** the visible surface is restored exactly
