# python-bindings — quad meshing from Python

Delta for `quad-mesh-export`.

## ADDED Requirements

### Requirement: Quad meshing from Python
`pyclay` SHALL expose quad meshing on a document and on a voxel grid, taking a lattice cell size OR a target quad count, a tolerance, an iteration cap and a mode, and returning a `Mesh`.

The mode SHALL be spelled as a string, as the mesher choice already is, and an unrecognised one SHALL raise rather than fall back. Asking a document for the voxel-only faces mode SHALL raise, for the same reason the C ABI refuses it.

Every entry point SHALL carry a name in the C ABI under the prefix rules the binding-parity gate applies, so the surface stays reachable from C without an exemption.

The docstrings SHALL state that this is a lattice-derived quad grid and NOT field-aligned retopology — no edge loops, no feature-placed poles, not animation-ready — and SHALL state that a target is approached rather than hit.

#### Scenario: A document quad-meshes from Python
- **WHEN** a document is quad-meshed at a given cell size
- **THEN** the returned mesh reports a non-zero quad count and its triangles are that quad list's triangulation

#### Scenario: An unknown mode raises
- **WHEN** a mode outside the documented list is passed
- **THEN** a `ValueError` names the modes that exist

#### Scenario: Faces mode on a document raises
- **WHEN** a document is asked for the voxel-only faces mode
- **THEN** it raises, naming the voxel grid as the source that mode belongs to

### Requirement: A Python mesh exposes its quads as numpy
`Mesh` SHALL expose its quads as a numpy view shaped `(Q, 4)` of `uint32`, zero-copy and lifetime-bound to the mesh exactly as the position, normal, colour, uv and index views already are, and SHALL expose the quad count.

A mesh with no quads SHALL present an empty `(0, 4)` array, matching how the index view already presents an empty mesh, so a caller can shape code against it without a null check.

The existing `indices` view SHALL keep returning the triangulation shaped `(T, 3)`, and `triangle_count` SHALL keep counting triangles. Nothing an existing script reads changes value.

`Mesh` SHALL also report how the mesh was produced — the cell size, the target asked for, the count reached, the iterations spent, whether it converged and whether it clamped — so a script that asks for a count can print what it got. A mesh that was not quad-meshed SHALL raise rather than report zeroes.

#### Scenario: Quads come back as numpy
- **WHEN** a script reads the quad view of a quad mesh
- **THEN** it is an `(Q, 4)` uint32 array whose every entry indexes a vertex, and modifying the mesh's owner does not invalidate it while the view lives

#### Scenario: A triangle mesh has an empty quad view
- **WHEN** a script reads the quad view of a mesh from any existing mesher or from a file
- **THEN** it is an empty `(0, 4)` array and the quad count is zero

#### Scenario: The report is printable
- **WHEN** a script quad-meshes with a target and reads the report
- **THEN** it names the cell size, the target, the count actually produced, and whether the search converged
