# voxel-engine — smooth voxel display

Delta for `smooth-voxel-display` (#108).

## ADDED Requirements

### Requirement: A grid meshes smoothly as well as blockily
A grid SHALL offer a smooth surface mesh beside `mesh_greedy`, so a sculpt can be displayed as a form rather than as boxes. Greedy meshing emits axis-aligned quads by construction, which is correct for hard-surface work and for export and is the wrong picture of an organic sculpt.

The smooth mesher SHALL place one vertex per surface cell at the centroid of that cell's edge crossings, over occupancy sampled at voxel centres. The centroid is what does the smoothing: a corner cell's vertex is pulled toward the average of its crossings, so the corner rounds without any filtering step. No occupied cell SHALL disappear as a consequence of smoothing at the default setting — a lone voxel has a sign change on each of its six edges and SHALL still produce a surface.

An optional occupancy blur MAY be offered for a smoother result and SHALL NOT be the default, because a blur is what erases thin features: an isolated voxel under a 3x3x3 tent sits near 0.3 occupancy, below the isolevel, and vanishes. A caller asking for it is asking for that trade.

The documentation SHALL state which setting an organic sculpt wants, because the default is not it: the unblurred surface rounds corners but still terraces, since every crossing over binary occupancy interpolates to the same midpoint. One blur pass is what reads as clay. A default that silently deletes thin detail is the wrong default however good it looks, so the recommendation belongs in prose rather than in the default.

Vertex colour SHALL be the average of the palette colours of the occupied cells adjacent to that vertex. A smooth surface has no per-quad facet to carry one palette entry, so two colours meeting gradate across a cell rather than meeting on a line.

`mesh_greedy` SHALL be unchanged in behaviour and output. It remains what export uses and what a hard-surface voxel sculpt wants, and the smooth mesher SHALL NOT be described as replacing it.

The smooth mesher SHALL be documented as a PREVIEW path and SHALL NOT claim manifold or watertight output, for the reason `mesh/surface_nets.h` already gives: a cell crossed twice by the surface receives one vertex and the sheets pinch.

#### Scenario: A cube sculpt rounds rather than staying a cube
- **WHEN** a solid box of voxels is meshed smoothly
- **THEN** its corners are rounded, its faces are not planar quads spanning the box, and the same grid meshed with `mesh_greedy` still returns the flat-faced box unchanged

#### Scenario: A lone voxel survives
- **WHEN** a grid holding one occupied cell is meshed smoothly at the default setting
- **THEN** it produces a closed surface around that cell rather than an empty mesh

#### Scenario: Colour blends across a boundary rather than facetting
- **WHEN** two adjacent regions of different palette indices are meshed smoothly
- **THEN** vertices between them carry a blend of the two palette colours, and no vertex carries a colour absent from the palette

#### Scenario: The blocky mesh is untouched
- **WHEN** any grid is meshed with `mesh_greedy` before and after this change
- **THEN** the vertex and index buffers are byte-identical
