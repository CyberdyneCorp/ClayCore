# meshing Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Default mesher with watertight guarantee
`clay::mesh` SHALL provide a default cell-marching mesher whose output is watertight and 2-manifold by construction, running only over surface-crossing bricks. v1 implements this with marching tetrahedra (Freudenthal 6-tet decomposition with globally consistent face diagonals — no ambiguous configurations exist, so the guarantee is structural); a table-based marching cubes with asymptotic-decider ambiguity resolution MAY replace it later as a triangle-count optimization provided the same guarantees hold. The CPU implementation is the golden reference; GPU implementations (Metal/CUDA) SHALL match its topology invariants (watertight, manifold, Euler characteristic on golden scenes) though not bit-identical vertex positions.

#### Scenario: Watertight across the op matrix
- **WHEN** golden scenes covering every op × blend combination are meshed at standard resolutions
- **THEN** every output mesh passes watertight and 2-manifold validation

#### Scenario: GPU meshing topology parity
- **WHEN** a golden scene is meshed on CPU and on a GPU backend
- **THEN** both meshes are watertight/manifold with identical Euler characteristic

### Requirement: Surface nets preview mesher
The module SHALL provide a surface-nets mesher for cheap smooth preview meshes, sharing the brick traversal and attribute sampling of marching cubes.

#### Scenario: Preview mesh from bricks
- **WHEN** surface nets runs over a filled brick cache
- **THEN** it produces a valid mesh in less time than marching cubes at equal resolution (benchmarked, regression-gated)

### Requirement: Dual contouring (flagged)
The module SHALL provide dual contouring with QEF minimization over Hermite data (position + normal per edge crossing) for sharp-edge export, in its manifold variant, shipped behind an explicit opt-in flag until hardened post-v1.

#### Scenario: Sharp edge preserved
- **WHEN** a chamfered box union is meshed with dual contouring
- **THEN** the chamfer's edge lines appear as sharp polylines in the mesh (vertices placed by QEF on the edges), unlike the rounded MC result

### Requirement: Decimation
The module SHALL provide quadric edge-collapse decimation via meshoptimizer, driven by target triangle ratio or error bound, aware of vertex color attributes (collapses SHALL NOT merge across strong color boundaries beyond the configured attribute weight).

#### Scenario: Ratio-targeted decimation
- **WHEN** a mesh is decimated to ratio 0.5
- **THEN** the output has ≤ 50% of input triangles, remains watertight if the input was, and preserves color regions within the attribute error bound

### Requirement: Mesh validation
The module SHALL provide validation primitives: watertightness, 2-manifoldness, degenerate-triangle detection, and sampled self-intersection checks. These back both CI export gates and any consumer's "clean geometry" claims.

#### Scenario: Validator catches a hole
- **WHEN** a mesh with one deleted triangle is validated
- **THEN** the watertight check fails and reports the open edge loop

### Requirement: Mesh attributes
Meshers SHALL emit vertex colors sampled from the scene color field (faithful to blend gradients via the material-mix factor), normals from field gradient or face normals (caller choice), and SHALL offer an optional box-projection UV utility.

#### Scenario: Blend gradient in vertex colors
- **WHEN** two differently colored shapes joined by a smooth blend are meshed
- **THEN** vertex colors across the joint interpolate following the blend's material-mix falloff, not a hard color seam

