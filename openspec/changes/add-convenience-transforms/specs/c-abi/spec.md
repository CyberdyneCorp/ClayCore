# c-abi

## ADDED Requirements

### Requirement: A layer can be placed by a convenience rule

The API SHALL offer three whole-layer placements computed from the layer's own
content rather than passed in: snapping the low face of its bounds to a named
ground height, centring its bounds on the world origin, and returning its
placement's translation to the origin. Each SHALL take the document and the
layer and nothing else beyond the ground height the first one names.

Each SHALL change the placement's TRANSLATION alone. The rotation, the uniform
scale factor and the per-axis scale factors SHALL be carried through unchanged,
so a layer carrying three different factors is placed without being unsquashed.
This is the reason the calls exist: the single-factor reader refuses a
non-uniformly scaled layer and the single-factor setter clears the per-axis
scale, so the read-modify-write a host would otherwise write is either
impossible or silently destructive on exactly those layers.

Each SHALL state a TOTAL placement rather than an increment, so applying one
twice is the same gesture as applying it once. Idempotence SHALL be stated to
within the rounding of one addition at the box's magnitude rather than bit for
bit, because the second application recomputes the bound from an already-moved
layer.

The centring call SHALL be named for the box it reads and SHALL NOT be named
for a centre of mass. The engine holds no density, and the call's answer is
identical for a hollow shell and a solid of the same extent.

The call that returns the translation to the origin SHALL read no bounds and
SHALL NOT reset the rotation or either scale.

The resulting placement change SHALL classify as RIGID, so the guarantee a
rigid re-placement already carries applies: the layer's surface afterwards is
its surface beforehand moved by one matrix, and every other layer's field is
unchanged.

No delta SHALL be returned. A caller wanting the translation reads the per-axis
placement before and after and subtracts the positions, which is exact because
the change is a pure translation.

#### Scenario: A squashed layer keeps its three factors
- **GIVEN** a layer carrying three different per-axis scale factors and a rotation
- **WHEN** any of the three convenience placements is applied
- **THEN** the per-axis placement reads back the same three factors and the same rotation
- **AND** the layer's bounds have the same size they had, moved by the translation

#### Scenario: The low face lands on the plane
- **WHEN** a layer is snapped to a ground height
- **THEN** the minimum corner of its bounds has that height on the up axis, and its other two axes are unchanged

#### Scenario: The box centre lands on the origin
- **WHEN** a layer is centred
- **THEN** the midpoint of its bounds is the world origin in all three axes

#### Scenario: Returning to the origin keeps the rotation
- **GIVEN** a layer with a rotation, a uniform scale and a non-zero position
- **WHEN** its placement is returned to the origin
- **THEN** the position reads back as zero and the rotation and both scales are unchanged

#### Scenario: Applying it twice is applying it once
- **WHEN** a snap is applied twice in succession
- **THEN** the second application moves the layer by no more than one rounding at the coordinate's magnitude

### Requirement: A convenience placement refuses what its bound cannot describe

A convenience placement SHALL refuse rather than silently do nothing, and SHALL
leave the document unchanged when it refuses.

An unknown layer id SHALL be `CLAY_ERROR_NOT_FOUND`. Every other refusal SHALL
be `CLAY_ERROR_INVALID_ARGUMENT`, so that a not-found code continues to mean
"no layer carries this id" alone and a host can tell a stale id from a layer
that cannot take the operation.

A ghosted or locked layer SHALL be refused on the same terms as every other
edit, and SHALL be refused BEFORE the bounds are walked.

A layer holding no material SHALL be refused by the two calls that read bounds.
An empty layer has no low face and no centre, and the two states a silent
no-op would conflate — already in place, and nothing here — are the two a host
most needs told apart.

A layer carrying a RADIAL symmetry mode SHALL be refused by the two calls that
read bounds, naming the radial mode as the reason. The tight layer bound covers
the mirror copies and does not cover the radial ones, so an answer computed
from it would place the layer plausibly and wrongly; refusing is the same
choice already made where a cage cannot be placed through a record that cannot
hold its map.

The call that reads no bounds SHALL accept both an empty layer and a radial
layer, since neither condition can affect a translation it computes without
looking.

A ground height that is not finite SHALL be refused.

A degenerate bound — equal minimum and maximum in one or more axes — SHALL be
ACCEPTED. These placements only translate and nothing divides by an extent, so
a planar layer, a single-cell voxel grid and a single-vertex mesh each have a
well-defined low face and centre.

A HIDDEN layer SHALL be accepted. The bound answers from content rather than
from visibility, and a caller naming a layer says more than the flag does.

A convenience placement SHALL be refused while a layer placement gesture is
open, on the same terms as every other edit.

#### Scenario: An empty layer is refused, and told apart from a missing one
- **WHEN** a layer holding no material is snapped, and separately a layer id no layer carries is snapped
- **THEN** the first returns an invalid-argument error and the second a not-found error, and neither changes the document

#### Scenario: A radial layer is refused with its reason
- **GIVEN** a layer with a radial symmetry count above one
- **WHEN** it is snapped or centred
- **THEN** the call is refused and the diagnostic names the radial mode
- **AND** returning its placement to the origin succeeds, because that call reads no bounds

#### Scenario: A protected layer refuses before it costs anything
- **WHEN** a locked or ghosted layer takes any of the three
- **THEN** the call is refused and the layer's placement is unchanged

#### Scenario: A flat layer is placed, not refused
- **GIVEN** a layer whose content is degenerate in one axis
- **WHEN** it is snapped to a ground height
- **THEN** the call succeeds and its single plane sits at that height

#### Scenario: A hidden layer is placed
- **WHEN** a hidden layer holding material is centred
- **THEN** the call succeeds and its bounds are centred on the origin

### Requirement: A convenience placement never severs a shared layer

A convenience placement applied to a layer that shares its edit list with
another SHALL move that layer's placement alone. It SHALL NOT give the layer a
private copy of its content, SHALL NOT change what any other layer over the
same content evaluates to, and SHALL leave the sharing reported as it was.

A layer's placement is not shared by instancing — only the edit list is — so
each instance's bound is the shared content under its OWN placement and two
instances snap independently. Severing is what a BAKE does, because a bake
replaces an edit list; a placement replaces nothing, and unlinking a subtool
because an artist pressed a transform button would be a silent loss of the link
instancing exists for.

#### Scenario: Snapping one instance leaves the other alone
- **GIVEN** two layers sharing one edit list at different placements
- **WHEN** one of them is snapped to a ground height
- **THEN** the other layer's placement and bounds are unchanged
- **AND** both layers still report sharing their content

#### Scenario: Both instances can be placed independently
- **WHEN** each of two layers sharing an edit list is snapped to the same ground height
- **THEN** both have their low face at that height, and neither has a private copy of the content
