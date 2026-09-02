# meshing — the preview mesher is cheaper because the mesher is cheaper

Delta for `make-the-dual-walk-one-pass`.

## MODIFIED Requirements

### Requirement: Surface nets preview mesher
The module SHALL provide a surface-nets mesher for cheap smooth preview meshes, sharing the brick traversal and attribute sampling of marching cubes.

"Cheap" SHALL mean cheap to BUILD as well as cheap to carry, and the two SHALL be gated separately, because a mesher can look cheap on either one while being expensive on the other. Surface nets emits about a third of marching cubes' vertices, so any comparison that charges per vertex — an attribute pass, an upload, a draw — reports it as cheaper whatever its geometry step costs. A gate that means to say the MESHER is cheaper SHALL therefore compare the two over the SAME precomputed lattice, with no field evaluation and no attribute pass on either side, or it is measuring something else and will pass while the claim is false.

#### Scenario: Preview mesh from a lattice
- **WHEN** surface nets and marching cubes are run over the same precomputed lattice at equal resolution
- **THEN** surface nets produces a valid mesh in less time, and the comparison includes no field evaluation and no vertex attributes on either side

#### Scenario: Preview mesh from bricks
- **WHEN** surface nets runs over a filled brick cache
- **THEN** it produces a valid mesh with fewer vertices and triangles than marching cubes at equal resolution

## ADDED Requirements

### Requirement: The dual walk visits each cell once
The dual meshers — surface nets, dual contouring and the quad mesher — SHALL place a cell's vertex and emit the quads on the lattice edges it owns in ONE walk of the cell range.

This is sound rather than merely convenient: a quad sits on an edge leaving a cell's minimum corner, and the four cells around that edge are reached by stepping BACK along the two axes that are not the edge's, never forward. They are the owning cell and three already placed, so a single walk never needs a vertex it has not yet made. The vertices and the indices SHALL come out in the order two separate walks produced them.

A second walk SHALL NOT re-read the corners the first already had. The minimum corner and its three axis neighbours are four of the eight the vertex pass reads, and reading them again cost a third of the mesher — for every cell in the range, including the ones that own no vertex and therefore can contribute no quad, since an edge leaving the minimum corner changes sign only when the cell has corners of both signs.

The lattice sampler SHALL be reached without an indirect call in the meshers that supply one directly. Eight corners for every cell of a range is tens of millions of calls for a preview-resolution mesh, and through a function object none of them inlines: measured at 52.6 ms against 6.7 ms for the same reads on one lattice.

#### Scenario: A cell's vertex exists before any quad that references it
- **WHEN** the quads of a dual mesh are read in the order they were emitted
- **THEN** the largest vertex index in successive quads never goes backwards, so no quad references a vertex placed later

#### Scenario: One walk is what two walks were
- **WHEN** a dual mesh is built after the walk is changed
- **THEN** its vertices, indices, quads and normals are unchanged bit for bit, for every mesher that shares the walk
