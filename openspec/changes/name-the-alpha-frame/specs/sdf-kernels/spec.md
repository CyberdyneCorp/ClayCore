# sdf-kernels — the frame an alpha is authored in

Delta for `name-the-alpha-frame`.

## ADDED Requirements

### Requirement: An alpha stamp is authored in the item's own space
An alpha deformer's centre, direction, tangent, extent and radius SHALL be interpreted in the item's own local space, as a bend curve's guide and a lattice's box already are, and the API SHALL say so at every door that authors one.

The consequence is the point: they RIDE the item's transform. Moving the item moves the stamp, scaling it scales the stamp, and a stroke's template alpha therefore arrives in every stamp's own frame — turned by that stamp's rotation and scaled by its radius — without the stroke resolver transforming anything. A resolver that pre-transformed a template's alpha would apply the stamp transform twice.

The library SHALL provide the conversion from a world-space surface placement into an item's frame, because the helper that derives a placement from a surface hit necessarily produces world coordinates and the deformer necessarily consumes local ones.

#### Scenario: A stamp lands where the surface was hit
- **WHEN** a world-space placement derived from a surface hit on an item with a non-identity transform is converted into that item's frame and applied
- **THEN** the field moves by the authored amplitude at the hit point, and falls off symmetrically away from it

#### Scenario: An alpha rides its item's transform
- **WHEN** the same local alpha is applied to an item at the identity and to one translated, rotated and scaled
- **THEN** the second's field at each transformed point equals the first's at the original point, scaled by the item's scale

#### Scenario: A stroke carries the template's alpha unchanged
- **WHEN** a template item carrying an alpha is stroked into several stamps of differing radius and rotation
- **THEN** every resolved stamp carries the alpha with its samples, and its centre, direction and radius are unchanged in local coordinates

### Requirement: A degenerate alpha is refused rather than appended
An alpha whose direction has no length, or whose radius is not positive, SHALL be refused, leaving the item unchanged.

Both were previously accepted: the kernel substitutes a fixed axis for a zero direction and floors a non-positive radius, so each appended a deformer that returned success and did nothing. That is the case the width-below-two refusal already exists for.

The all-zeroes direction that a mesh brush descriptor documents as "the surface normal under the centre" SHALL NOT be given that meaning here, and the API SHALL say why: a mesh brush resolves it by querying the surface, and an item being authored has no surface to query.

#### Scenario: A zero direction is refused
- **WHEN** an alpha is added with an all-zeroes direction, or with a zero or negative radius
- **THEN** the call is refused and the item is byte-identical to one the call was never made on
