## ADDED Requirements

### Requirement: A captured region of the field is a reusable asset

A finite region of a document's field SHALL be capturable as a self-contained
signed-field asset that can be placed into an SDF layer many times. A capture
SHALL be a sampled field rather than a copy of the edit items that produced it:
a captured subtree carries identity dependencies on nodes that may be edited or
deleted, an evaluation cost that grows with what it captured, and no bounded
serialized form, and none of those is true of samples.

A placement SHALL be an ORDINARY EDIT ITEM — editable, transformable, undoable,
and combinable with the same ops and blends every other item has. The feature is
non-destructive because of that and not in addition to it.

**Placing an asset SHALL NOT copy its samples.** A document with a thousand
placements of one asset SHALL hold one copy of that asset's payload, and the
per-placement cost SHALL be a transform and a reference. Anything else makes a
detail brush unusable at the scale a detail brush is for.

A capture SHALL be taken in a frame the caller supplies, and the asset SHALL
record it, so the same asset placed at a new orientation is the shape that was
captured. The frame SHALL NOT be inferred from the captured content: an
orientation derived from the samples changes when the region moves, so
re-capturing the same detail would produce an asset that no longer agrees with
the placements already made from it.

Scale SHALL be uniform where the field's exactness contract requires it, and a
scale the contract cannot honour SHALL be refused rather than accepted with a
field that quietly reports the wrong distance — a marcher stepping on a wrong
bound misses surface, which is visible as holes rather than as an error.

Intensity SHALL NOT be expressed by multiplying the captured distance. That
scales the metric rather than the sculpt, so the result is a field whose zero set
has moved and whose gradient no longer has unit length; scale, the combine op and
the blend are the controls that mean what an artist expects.

#### Scenario: An asset reloaded on its own is the asset that was saved
- **WHEN** a captured asset is written to its standalone form and read back
- **THEN** the two place identically, and the field they contribute agrees at every point

#### Scenario: A placed asset reproduces what was captured
- **WHEN** a region is captured and the asset is placed back at the transform it was captured from
- **THEN** the field it contributes agrees with the source over that region within the sampling tolerance the capture declares

#### Scenario: A thousand placements hold one payload
- **WHEN** one asset is placed many times in a document
- **THEN** the document's authoritative bytes grow by a per-placement reference and not by the asset's samples, and saving and reloading preserves that

#### Scenario: A placement is an ordinary item
- **WHEN** an asset has been placed
- **THEN** it can be moved, re-combined, hidden and undone exactly as any other item, and one gesture that places several is one undo step

#### Scenario: A scaled placement is still safe to march
- **WHEN** a placed asset carries a non-uniform scale and the field is sampled outside its surface
- **THEN** stepping from any such point by the distance the field reports does not cross the surface

#### Scenario: An asset outlives what produced it
- **WHEN** the items a region was captured from are edited or deleted
- **THEN** every placement of the asset is unaffected, because the asset holds samples rather than a reference to those items
