# meshing

## ADDED Requirements

### Requirement: Colour brushes on a mesh layer
The library SHALL provide two vertex-colour brushes over a mesh's own triangles, `paint` and `smear`, so that a mesh layer's surface colour can be EDITED rather than only carried. Without them a mesh layer is the only representation whose colour is read-only, while the SDF side paints through `Op::Paint` strokes and the voxel side through its palette.

`paint` SHALL blend each vertex's colour toward a caller-supplied target by the brush's own per-vertex weight, so the falloff, the strength, the geodesic walk, the mask gate and the alpha stamp compose with it without per-verb code.

`smear` SHALL push existing colour along the drag direction, taking each vertex's new colour from the one-ring neighbour lying most nearly OPPOSITE the drag and weighting it by that alignment. A zero drag direction SHALL do nothing, rather than degenerating into a smooth — a verb that silently becomes a different verb is worse than one that refuses.

**A colour brush SHALL NOT move a vertex.** `positions` and `normals` SHALL be byte-identical before and after, which is the mirror of the guarantee the displacement verbs make about `colors`, and is what lets a host run a colour pass over a finished sculpt without a diff on the geometry.

A colour brush SHALL REFUSE a mesh with no vertex colour attribute rather than creating one, and the library SHALL provide an explicit way to create it. Allocating a colour per vertex on the first dab hides a real cost behind a brush stroke, and makes "nothing happened" indistinguishable from "this mesh had no colour attribute".

Colour SHALL be blended componentwise and never converted between colour spaces, so a linear buffer stays linear and an sRGB one stays sRGB. The ends of the blend SHALL be EXACT: a fully-weighted dab SHALL leave the target colour bit-identical, not one ULP away from it.

A colour edit SHALL be recorded and reverted by the same per-gesture record that already carries positions and normals, so a colour stroke undoes bit-identically like every other verb.

Applying a colour verb SHALL be DETERMINISTIC: the same mesh, settings and stamps SHALL produce bit-identical colours on every run and every platform.

#### Scenario: A colour brush moves nothing
- **WHEN** `paint` or `smear` is stamped on a mesh carrying vertex colours
- **THEN** `colors` differs and `positions` and `normals` are byte-identical

#### Scenario: A mesh with no colours is refused
- **WHEN** a colour verb is stamped on a mesh whose colour attribute is absent
- **THEN** nothing is painted, no colour attribute is created, and the caller can create one explicitly and then paint

#### Scenario: A full-weight dab lands exactly on the target
- **WHEN** `paint` is stamped with a constant falloff and full strength
- **THEN** the vertices it fully covers hold the target colour bit-identically

#### Scenario: Smear has a direction
- **WHEN** `smear` is dragged across a boundary between two colours, and then dragged the opposite way
- **THEN** the colour boundary moves with the drag in each case, and a zero drag direction changes nothing

#### Scenario: A colour stroke reverts exactly
- **WHEN** a stroke of a colour verb is reverted through its record
- **THEN** `colors` is bit-identical to before the stroke

## MODIFIED Requirements

### Requirement: A mesh brush does not disturb vertex colours
The fixed-topology mesh verbs that MOVE VERTICES SHALL leave `colors` untouched, so an imported model's colours survive sculpting on it.

Stated as a requirement rather than left implicit because it was originally true by omission — no verb wrote colour — and the point of writing it down was that a colour brush would have to add colour writing deliberately rather than by accident. `paint` and `smear` are that deliberate act, and are exempt from this requirement by being its counterpart: they write colour and move no vertex.

#### Scenario: Sculpting a coloured mesh keeps its colours
- **WHEN** any displacement mesh verb is stamped on a mesh carrying vertex colours
- **THEN** `colors` is byte-identical before and after
