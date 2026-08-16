# meshing — closing the mesh brush vocabulary

Delta for `close-mesh-brush-vocabulary`.

## ADDED Requirements

### Requirement: A mesh brush can be modulated by an alpha
A mesh brush SHALL accept a caller-supplied scalar stamp that scales its per-vertex weight, so detail work on a mesh layer is alpha-driven as it already is on voxels and on SDF items.

The engine SHALL NOT decode images. The stamp SHALL be sampled by the same kernel function the SDF alpha uses, so one stamp reads identically on both representations.

An absent alpha SHALL leave every verb exactly as it is today.

#### Scenario: No alpha changes nothing
- **WHEN** a brush is stamped without an alpha
- **THEN** the result is identical to the same stamp before alphas existed

#### Scenario: An all-zero alpha moves nothing
- **WHEN** a brush is stamped with an alpha whose samples are all zero
- **THEN** no vertex moves

#### Scenario: The same stamp reads the same on a mesh and a field
- **WHEN** the same samples are applied to a mesh brush and to an SDF item at corresponding points
- **THEN** the sampled values agree

### Requirement: Relax evens vertex distribution without reshaping
A verb SHALL slide vertices along the surface to even their spacing, moving them in the tangent plane rather than toward the Laplacian average. This is what recovers a region stretched by a large grab, which matters here because no brush adds polygons.

Its residual normal motion SHALL be stated rather than implied to be zero: sliding along a tangent plane leaves a curved surface by a second-order amount.

#### Scenario: Relax evens spacing
- **WHEN** relax is stamped on a region whose triangles vary in size
- **THEN** the variation in edge length within the region falls

#### Scenario: Relax moves the surface far less than smooth
- **WHEN** relax and smooth are stamped with the same strength on the same region
- **THEN** relax's movement along the surface normal is a small fraction of smooth's

### Requirement: Layer deposits to a ceiling rather than accumulating
A verb SHALL deposit material up to a fixed height above the surface as it was when the STROKE began, and no further, so a slow stroke and a fast one over the same path give the same result.

The height SHALL be in world units rather than scaled by the brush radius, because a ceiling that moved with the brush size would not be a ceiling.

#### Scenario: Repeated stamps do not dig deeper
- **WHEN** layer is stamped many times at one place within one stroke
- **THEN** the surface reaches the stated height and stops

#### Scenario: Draw at the same settings does accumulate
- **WHEN** draw is stamped the same number of times at the same place
- **THEN** it moves the surface further than layer did

### Requirement: Nudge moves material along the surface
A verb SHALL push material tangentially along the stroke direction, as distinct from grab, which carries the region rigidly.

#### Scenario: Nudge stays on the surface
- **WHEN** nudge is stamped with a drag direction
- **THEN** the vertices move within their tangent planes rather than along the drag itself

### Requirement: Every new verb honours the standing mesh contract
The added verbs SHALL change no topology, SHALL respect masks and falloffs as the existing verbs do, and SHALL record into `VertexDeltas` so a stroke is one undo step.

#### Scenario: Topology is untouched
- **WHEN** any new verb is stamped
- **THEN** `indices` and `quads` are byte-identical before and after

#### Scenario: A stroke reverts exactly
- **WHEN** a stroke of any new verb is reverted through its record
- **THEN** the mesh is bit-identical to before the stroke
