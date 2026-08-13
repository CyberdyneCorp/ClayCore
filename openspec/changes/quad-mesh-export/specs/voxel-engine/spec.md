# voxel-engine — a sculpt exports as quads

Delta for `quad-mesh-export`.

## ADDED Requirements

### Requirement: A voxel grid meshes to quads in two modes
`VoxelGrid` SHALL provide a quad mesher offering two modes, because a voxel sculpt is two different subjects depending on what the user made.

**Dual mode** SHALL be the lattice dual over the same occupancy field `mesh_smooth` builds — the rounded form, quads meeting four to a vertex — generalised to a lattice cell size other than the grid's voxel size by sampling that occupancy TRILINEARLY.

At the grid's own voxel size with no blur the sampler reads exactly the values `mesh_smooth` reads, so dual mode SHALL return `mesh_smooth`'s mesh vertex for vertex and index for index, differing only by the quad array. This identity is what keeps the two on one code path rather than two that drift.

A cell size COARSER than a voxel low-passes the occupancy and CAN drop a one-voxel-thick feature entirely — the same failure `blur` already carries, for the same reason, and stated in the same place. A cell size FINER than a voxel resamples the same step field: it adds quads and no detail, so the count search SHALL clamp there and report that it clamped.

**Faces mode** SHALL emit one planar, axis-aligned quad per exposed voxel face — the greedy sweep with merging switched off. It SHALL NOT change `mesh_greedy`, whose merged output and its existing per-chunk guarantees are untouched.

Faces mode SHALL WELD its corners, keyed by lattice corner AND palette index. Today's `emit_quad` pushes four fresh vertices per face, so a greedy mesh arrives in a DCC as disconnected rectangles; welding makes it a connected quad grid within a colour region, and splits it at a colour boundary so per-face palette colour survives — which is why those vertices were duplicated in the first place.

Faces mode SHALL NOT emit vertex normals. A welded corner is shared by faces pointing three ways and has no single normal; averaging would round a cube and duplicating would undo the weld. The quads are planar, so a consumer derives the face normal from the face. A caller who needs per-face normals uses `mesh_greedy` and gets triangles, as today.

Faces mode has no cell size — its lattice is the grid. Its count lever SHALL be the multi-resolution LEVEL the grid already carries, so its granularity is roughly a factor of four per step, and a requested target SHALL pick the nearest level. This SHALL be stated, because a caller who asks for fifty thousand quads and receives twelve thousand needs to know a level was chosen rather than a bug hit.

#### Scenario: Dual mode at the grid's own resolution is the smooth mesh
- **WHEN** a sculpt is quad-meshed in dual mode at the grid's voxel size with no blur
- **THEN** the positions and triangle indices are identical to `mesh_smooth`'s, and the mesh additionally carries one quad per two triangles

#### Scenario: Faces mode is one quad per exposed face
- **WHEN** a solid block of voxels is quad-meshed in faces mode
- **THEN** the output carries exactly one quad per exposed voxel face, every quad is planar and axis-aligned, and the quads cover exactly the surface `mesh_greedy` covers over the same cells

#### Scenario: Faces mode welds within a colour and splits across one
- **WHEN** a two-colour slab is quad-meshed in faces mode
- **THEN** faces of the same palette index sharing a lattice corner share one vertex, and the corner on the boundary between two palette indices appears once per colour

#### Scenario: Greedy meshing is untouched
- **WHEN** `mesh_greedy` and `mesh_greedy_chunks` run after faces mode exists
- **THEN** their output is identical, vertex for vertex and index for index, to what they produced before it

#### Scenario: A cell finer than a voxel is clamped
- **WHEN** dual mode is asked for a cell size below the grid's voxel size
- **THEN** it meshes at the voxel size and reports that it clamped, rather than spending the quads on detail the grid does not hold
