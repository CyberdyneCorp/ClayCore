## ADDED Requirements

### Requirement: Relief displaces along each point's own normal
Relief SHALL move each point of the accumulated surface along that point's own normal — the gradient of the accumulated field — and not along one direction shared by the stamp. That makes it the field counterpart of the mesh Inflate brush (`CLAY_BRUSH_FRAME_VERTEX_NORMAL`), and only an approximation of the mesh Draw brush and the Standard preset built on it (`CLAY_BRUSH_FRAME_REGION_NORMAL`).

The library SHALL NOT describe Relief as a faithful Standard. Where the normals under a stamp agree, the two frames differ by a small fraction of the amplitude; on a feature narrower than the stamp they differ by the whole amplitude, and the documentation beside the op SHALL say so with measured numbers.

#### Scenario: A thin ridge thickens on both faces
- **WHEN** one relief stamp of amplitude k covers the top of a fin narrower than the stamp
- **THEN** where the stamp's weight is full, each face of the fin moves outward by k, as well as the top rising by k

#### Scenario: The per-point-normal displacement lies on the relief surface
- **WHEN** each surface point in a relief stamp's support is moved along its own normal by the relief's amplitude times its weight
- **THEN** the moved point lies on the displaced surface

#### Scenario: A shared-direction displacement does not
- **WHEN** the same points on the faces of a fin narrower than the stamp are moved instead along the stamp's single averaged normal
- **THEN** they lie at least half the amplitude away from the relief surface
