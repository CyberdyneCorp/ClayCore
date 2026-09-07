## MODIFIED Requirements

### Requirement: Automasking gates a brush without a per-verb branch
The library SHALL provide automask factors — normal angle, topology connectivity, boundary proximity, cavity, and surface-group membership — evaluated over the brush's WORKSET and composed into the per-vertex weight by multiplication.

An automask SHALL be computed for the vertices a stamp reaches and SHALL NOT be computed, allocated or scanned for the whole surface.

Cavity and curvature automasks SHALL be derived from the SAME estimator the procedural mask verbs use, so that a painted cavity mask and a cavity automask cannot disagree about one surface.

The cavity and surface-group automasks read WORLD-ADDRESSED lattices, and a sculptor's vertices are not in world space. Each vertex SHALL be placed by the stroke's mesh-to-world transform before either lattice is sampled — the same placement the painted mask already receives. One estimator does not prevent two answers about one surface if its two callers ask it about different points.

The surface-group automask SHALL read the document's group field rather than a per-face group identifier. Groups are addressed on a world lattice so that they survive a representation bridge; a per-face copy would be a second answer to the same question and would not survive one.

The cavity strength SHALL be honoured at zero. Zero is off even when the factor's bit is set, because it is what a host's slider at zero means, and a binding that reads it as "unset, take the default" inverts it.

A fully automasked vertex SHALL be bit-identical to its input position.

#### Scenario: A cavity automask and a painted cavity mask agree
- **WHEN** a cavity automask and a procedural cavity mask are evaluated over the same surface with the same parameters
- **THEN** they report the same values within the estimator's own tolerance

#### Scenario: An automask costs the workset, not the model
- **WHEN** a stamp with every automask enabled runs on a mesh of a million vertices with a footprint of a few thousand
- **THEN** the automask evaluation touches the workset and its read halo only

#### Scenario: A transformed layer's automask reads where the layer is
- **WHEN** a stroke on a layer whose transform is not the identity enables the cavity or surface-group automask
- **THEN** the lattice is sampled at the vertex's world position, and the factor gates the same surface a painted mask on that layer gates

#### Scenario: A cavity strength of zero is off
- **WHEN** a brush sets the cavity factor's bit and a cavity strength of zero
- **THEN** the factor scales no weight, and the stamp is the stamp it would have been with the factor off
