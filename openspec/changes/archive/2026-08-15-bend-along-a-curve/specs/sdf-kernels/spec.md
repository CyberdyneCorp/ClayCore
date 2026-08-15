# sdf-kernels — bending along a drawn curve

Delta for `bend-along-a-curve`.

## ADDED Requirements

### Requirement: Bend along a guide curve
The kernel dialect SHALL provide a deformer that maps an item's local span along an axis onto a guide polyline's ARC LENGTH, carrying the material on the guide's parallel-transported frames, so an artist can draw the shape the item's axis takes rather than being limited to the circular arc a constant rate produces.

The map SHALL be the INVERSE of the swept primitive and SHALL be implemented by reusing the sweep's own nearest-point query, frame construction and guide blob layout, not by a second implementation of them. A sweep and a bend along a curve are the same geometry read from either end, and sharing the query is what makes them agree by construction.

A guide running straight along the axis SHALL be the IDENTITY map, asserted rather than assumed — it is what makes this deformer a generalization of the undeformed item rather than a second thing to keep in step.

Agreement with the constant-rate `bend` on a circular guide SHALL NOT be used as evidence: that kernel is the CHEAP bend, a rotation by an angle proportional to the axis coordinate rather than an arc-length-preserving one, so the two are not meant to agree and an assertion that they do would be checking against a known approximation. The arc-length claim SHALL be asserted directly instead — a point ON the guide has zero perpendicular offset by construction, so the deformed field there SHALL equal the undeformed field at the axis point its arc length maps to.

The guide SHALL live in the tape's blob with the deformer record holding an offset and a count, reusing the swept vertex layout. Widening every deformer record to fit the largest payload, or reusing the node's primitive stroke, SHALL NOT be used: the first charges every item for the widest deformer in the innermost evaluation loop, and the second would silently forbid bending the stroke and swept items most likely to want it.

The declared Lipschitz factor SHALL charge BOTH the curvature compression on the inside of a bend and the axial rescale from laying the span onto the guide's length. Where the item's cross-section reaches the tightest bend radius the map folds through itself; this SHALL degrade to a very small step rather than being refused, because a guide is editable after the fact.

The tightest bend radius SHALL be measured as the CIRCUMRADIUS of consecutive guide triples rather than as accumulated turn angle, which reads a finely-tessellated gentle curve as a tight one.

The influence bound SHALL be the guide's own bounds grown by the item's cross-section extent. Unlike a rotation about a fixed axis, a curve can carry material anywhere the guide goes, so the undeformed item's neighbourhood does not contain the warp.

A guide of fewer than two points, or one of zero length, SHALL be refused at the bindings rather than divided by.

It SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene whose guide TURNS, so a backend that read the arc length but ignored the transported frames fails rather than passes.

#### Scenario: A straight guide changes nothing
- **WHEN** an item is bent along a guide that runs straight down its own axis
- **THEN** every point evaluates as it did undeformed

#### Scenario: A point on the guide reads the item's axis
- **WHEN** the field is evaluated at a guide vertex whose arc length is known
- **THEN** it equals the undeformed field at the axis point that arc length maps to

#### Scenario: The material follows where the guide goes
- **WHEN** a guide turns away from the item's axis
- **THEN** the item's far end is found near the guide's far end, and the influence bound contains it

#### Scenario: A fold degrades rather than lying
- **WHEN** the item's cross-section is wider than the guide's tightest bend radius
- **THEN** the compiled tape reports a very small safe step scale rather than claiming the field is a distance

#### Scenario: A guide that cannot mean anything is refused
- **WHEN** a caller passes fewer than two guide points, or a guide of zero length
- **THEN** the binding refuses it rather than dividing by zero

### Requirement: Deformers may read the tape blob
The deformer point map SHALL receive the tape's blob pointer, so a deformer whose payload is not a fixed size can hold an offset into the blob exactly as a primitive does.

The blob is already in hand at the point map's only call site, so this SHALL be a threading change rather than new plumbing; deformers that carry no payload SHALL ignore the pointer and behave identically.

#### Scenario: Payload-free deformers are unaffected
- **WHEN** the blob pointer is threaded through the deformer point map
- **THEN** every existing deformer produces the values it produced before, on every backend
