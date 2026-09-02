## ADDED Requirements

### Requirement: A world magnify resolves into a field-level radial scale
A magnify stated in WORLD space — a centre, a radius and a SIGNED strength — SHALL resolve into one `magnify` deformer per item the region reaches, each already in that item's own frame, so that the layer's ASSEMBLED surface swells or gathers rather than one item's share of it.

`magnify` is per item and applied to that item's local point, exactly as `grab` is, so a magnify put on one item of a smooth-unioned form scales that item's field and leaves the others where they were. This is the hazard `move_brush` exists for, and it applies to the radial scale verbatim.

A POSITIVE strength SHALL swell the surface away from the centre and a NEGATIVE one gather it toward. One signed parameter covers Magnify and Pinch, which are one deformation.

The strength SHALL cross the layer's symmetry images unchanged: a reflection or a rotation of a radial scale is a radial scale of equal strength, unlike a drag's displacement, which has to be mapped per image.

This SHALL follow the resolver pattern the Move brush established: the layer is READ and never written so a host can preview the gesture, the warps are RETURNED rather than applied so one command per node inside an undo group makes the gesture one undo step, and each warp SHALL belong at the FRONT of its node's chain.

Items the region cannot reach SHALL take no deformer, a strength of zero SHALL produce nothing, and a non-positive radius SHALL produce nothing.

#### Scenario: A blended form swells as one surface
- **WHEN** a magnify is resolved over a form smooth-unioned from two items and the warps are applied
- **THEN** both items take a share and the surface swells symmetrically about the gesture's centre

#### Scenario: Magnifying one item is not the same thing
- **WHEN** the same deformation is expressed as a magnify on a single item instead
- **THEN** that item's side moves and the other is left behind

#### Scenario: The sign chooses Magnify or Pinch
- **WHEN** the same region is resolved at a positive and then a negative strength
- **THEN** the surface swells away from the centre in the first case and gathers toward it in the second

#### Scenario: A transformed layer maps correctly
- **WHEN** the layer carries a transform and a magnify is resolved in world space
- **THEN** the surface changes where the gesture was aimed, and the radius each item sees is the world radius through that layer's scale

#### Scenario: Nothing is written
- **WHEN** a magnify is resolved
- **THEN** the document is unchanged until the caller applies the result

### Requirement: A magnify gesture coalesces rather than accumulating
A magnify applied repeatedly as one gesture SHALL replace that gesture's deformers rather than adding another beside them, for the reason a drag does: otherwise the chain grows by one entry per frame and each entry multiplies into the declared Lipschitz.

A gesture SHALL be recognised by its KIND as well as by its centre and radius. A drag and a magnify over the same ball are two gestures, and neither SHALL replace the other's leading deformer.

#### Scenario: A live gesture replaces its own last frame
- **WHEN** a magnify is applied over several frames at a growing strength
- **THEN** each item carries one magnify, and the document is the one a single frame at the final strength produces

#### Scenario: A pinch does not swallow a drag over the same ball
- **WHEN** a magnify is applied over a ball that already carries a drag's grab
- **THEN** both remain, the magnify in front

### Requirement: A magnify can be previewed without applying it
Resolving SHALL be pure, and a caller SHALL be able to learn which nodes a magnify would warp without touching the document. The preview SHALL refuse exactly what the apply refuses, so a host does not discover a malformed gesture only on commit.

#### Scenario: The preview names what the apply touches
- **WHEN** a magnify is previewed and then applied over the same region
- **THEN** the preview names the same nodes, and the document is unchanged until the apply
