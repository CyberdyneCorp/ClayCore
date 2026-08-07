# sdf-kernels — mask extrude

Delta for `add-mask-extrude`.

## ADDED Requirements

### Requirement: A mask can be measured as a distance
The library SHALL provide a conversion from a mask to a signed distance field: the distance to the boundary of the region where the mask meets a threshold, negative inside it. The result SHALL be an ordinary sampled volume, so that it is 1-Lipschitz, tape-expressible, blob-carried and identical on every backend.

The conversion exists because a mask is a [0,1] scalar on a lattice. Composing one into a field expression directly would put a step in the result, and the Lipschitz bound the evaluator depends on would no longer hold.

The conversion SHALL pad the sampled region by a caller-given amount, because a band clipped at the mask's own border is no use to an operation that reaches outside it.

An empty mask SHALL yield nothing rather than a volume that reads as empty space everywhere, which is harder to notice than a failure.

#### Scenario: Inside is negative and outside is positive
- **WHEN** a mask is painted as a blob and converted to a field
- **THEN** the field is negative well inside the blob, positive well outside it, and near zero at its border

#### Scenario: It is a distance
- **WHEN** the converted field is sampled along a line crossing the border
- **THEN** the value changes at roughly unit rate with distance, rather than stepping

#### Scenario: An empty mask converts to nothing
- **WHEN** a mask with nothing painted is converted
- **THEN** no field is produced

### Requirement: Mask extrude on a field
The library SHALL resolve a source field, a mask and a thickness into a new field holding only the masked patch of the source's surface, thickened. The result SHALL be an ordinary sampled volume, so meshing, evaluation, picking, serialization and every backend apply to it unchanged.

The thickness SHALL be placeable on either side of the source surface or centred on it. The intersection with the masked region SHALL admit a rounding radius, so the rim can be soft rather than a hard edge at the mask's border.

The result SHALL declare the Lipschitz its samples actually have rather than assuming one: a rounded intersection of two fields is a bound, not an exact distance, and an evaluator told otherwise oversteps.

The extrude SHALL be refused — yielding nothing — for an empty mask, a mask that does not reach the source surface, a non-positive thickness, or a sampling resolution finer than the mask itself can describe. Producing something in those cases would produce something that is not what the caller asked for.

The mask SHALL NOT be modified.

#### Scenario: A plate comes off a sphere
- **WHEN** a cap of a sphere is masked and extruded outward at a thickness
- **THEN** the result is a shell sitting on that cap, of that thickness measured along the surface normal, and empty away from the mask

#### Scenario: Each side means what it says
- **WHEN** the same mask and thickness are extruded outward, inward and centred
- **THEN** each result lies on the corresponding side of the source surface

#### Scenario: The rim rounds
- **WHEN** an extrude is taken with a rounding radius
- **THEN** the field is continuous across the mask's border rather than stepping at it

#### Scenario: Refusals produce nothing
- **WHEN** an extrude is asked for with an empty mask, a mask away from the surface, or a non-positive thickness
- **THEN** nothing is produced, and nothing crashes

#### Scenario: A ray still lands
- **WHEN** the extruded volume is sphere-traced from outside
- **THEN** the march converges on its surface, which is what a declared Lipschitz that held means in practice
