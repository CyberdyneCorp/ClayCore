## ADDED Requirements

### Requirement: A sculpt layer's strength is defined on a quantity whose half is defined

Where the library offers a per-layer strength, that strength SHALL scale a
quantity for which "half" has a meaning in what the layer STORES — a
displacement, an amplitude, or a reproducible fraction of discrete writes.

A strength SHALL NOT be implemented as an interpolation of the composed signed
distance field between the document without the layer and the document with it.
Such an interpolation moves the surface linearly only where the two fields
already agree in sign, and admits new material abruptly where they do not, so
one control performs two different operations and neither is the one the label
promises.

Where a layer's contribution has no such quantity — a pass of authored CSG nodes
being the case that matters — the library SHALL offer enable/disable rather than
a strength, and SHALL NOT offer a strength that is approximate.

A layer MAY acquire a strength by being converted to a representation that has
one, and that conversion SHALL be the artist's explicit choice rather than a
silent consequence of moving a slider, because it costs the layer's declarative
re-editability.

#### Scenario: A slider that would not deliver what it reads
- **WHEN** a per-layer strength is proposed for a representation whose stored contribution has no defined scaling
- **THEN** it is refused rather than implemented as a field interpolation

#### Scenario: A pass that can be dialled
- **WHEN** a layer's contribution is a displacement, a deformation amplitude, or a set of discrete writes
- **THEN** a strength is offered, and at any value it delivers that fraction of the pass

#### Scenario: The endpoints are exact
- **WHEN** a strength is at 0 or at 1
- **THEN** the result is exactly the document without the layer, or exactly the document with it, rather than an approximation of either
