# mesh-multires — a hierarchy is a document payload, not only a handle

Delta for `persist-a-multires-hierarchy`.

## ADDED Requirements

### Requirement: A hierarchy has a document identity
A multiresolution hierarchy SHALL be identified within a document by the layer id of the mesh layer holding its base cage, and a layer SHALL carry at most one hierarchy. The identity is the layer id rather than a handle address so that it survives a save, a load and a process boundary — a handle does not, which is the whole reason a host needed a side-car file to remember that a row was ever a hierarchy.

Associating a hierarchy with a layer that is not a mesh layer SHALL be refused, and so SHALL associating a second hierarchy with a layer that already carries one. Replacing an existing hierarchy SHALL be an explicit act rather than a silent overwrite, because the detail it drops is not recoverable from the cage.

#### Scenario: The identity survives a round trip
- **WHEN** a document holding two hierarchies on two mesh layers is saved and reloaded
- **THEN** each hierarchy is associated with the same layer id it had before, and neither is reachable from the other's layer

#### Scenario: A non-mesh layer is refused
- **WHEN** a hierarchy is associated with an SDF or voxel layer
- **THEN** the association is refused and the document is unchanged

### Requirement: A cage and its hierarchy's base are not reconciled
A hierarchy holds its own copy of the base cage and the mesh layer holds triangles, and nothing SHALL force the two equal. `clay_multires_from_mesh` builds from a mesh value rather than from a layer, so the two can already diverge; carrying both in a document SHALL NOT change that, and a save SHALL NOT mutate either to agree with the other.

Instead the engine SHALL answer, on demand, whether a layer's cage agrees with its hierarchy's base level. The answer SHALL be computed from the two objects in memory and SHALL NOT be stored in the document, because a stored identity is stable neither across builds that change an encoding nor across byte orders, and would report a divergence that had not happened.

#### Scenario: A round trip does not reconcile
- **WHEN** a document whose cage and hierarchy base already disagree is saved and reloaded
- **THEN** both come back exactly as they were, and neither has been changed to match the other

#### Scenario: Divergence is reportable
- **WHEN** a layer's cage is edited while it carries a hierarchy, and the two are compared
- **THEN** the comparison reports that they disagree, and reports agreement when the cage is untouched

### Requirement: A hierarchy is counted by document memory
`io::document_memory` SHALL count the hierarchies a document carries, using the accounting `mesh::MultiresSurface::memory()` already reports, so that a host sizing a document sees the term that dominates it. A hierarchy is routinely the largest payload in a document, and reporting a document's memory while omitting it answers a question nobody asked.

#### Scenario: A document's memory includes its hierarchies
- **WHEN** the memory of a document holding a hierarchy is reported
- **THEN** the total includes that hierarchy's own accounting, and grows when a level is added

### Requirement: A hierarchy still does not reach the evaluated field
Carrying a hierarchy in a document SHALL NOT change what that document evaluates to. A hierarchy is authored geometry in the same sense an imported mesh is: it is stored beside the document, it is withheld from `clay::scene` by the layering table, and the field a document compiles is the same with it and without it.

#### Scenario: The field is unchanged by the payload
- **WHEN** the same document is evaluated with a hierarchy attached and with it absent
- **THEN** the evaluated field is bit-identical at every sampled point
