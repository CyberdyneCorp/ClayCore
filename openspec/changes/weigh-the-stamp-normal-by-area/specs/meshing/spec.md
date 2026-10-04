## ADDED Requirements

### Requirement: The stamp's averaged normal does not depend on the triangulation
The region's averaged normal SHALL be the sum of the pre-stamp vertex normals, each weighted by its composed weight TIMES the surface area its vertex stands for (one third of every triangle it is a corner of, including any face a multiresolution level derives across a depth boundary), normalised. It SHALL be resolved once, in the shared composition, for the fixed mesh, the adaptive surface and a multiresolution level alike. The weighted centroid SHALL keep one equal vote per vertex.

#### Scenario: A symmetric ridge resolves its own normal
- **WHEN** a fin 0.1 thick and 1.0 tall, meshed at voxel 0.01 on a lattice that passes through its faces and on one that falls between them, takes one Draw stamp of radius 0.45 centred on its ridge
- **THEN** the averaged normal lies within 0.5 deg of the fin's axis of symmetry on both lattices, on the fixed mesh and on the adaptive surface built from it

#### Scenario: Smooth surfaces stay as close or closer
- **WHEN** the same stamp lands on the pole of a unit sphere and on the inner equator of a torus `R = 1, r = 0.4`, each meshed at voxel 0.01
- **THEN** the averaged normal lies within 0.25 deg of the surface normal there
