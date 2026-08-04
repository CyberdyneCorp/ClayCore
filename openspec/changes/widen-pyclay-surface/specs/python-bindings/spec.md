# python-bindings — full vocabulary through pyclay

Delta for `widen-pyclay-surface`.

## MODIFIED Requirements

### Requirement: pyclay module
The library SHALL ship a nanobind extension module `pyclay` exposing: document/layer construction (`Document`, `add_sdf_layer`, `add_voxel_layer`), the full edit vocabulary (primitives, ops, blends, transforms, deformers, mirrors, strokes) with Pythonic parameter names, field evaluation (`eval`, `gradients`), meshing with resolution/decimation/backend selection, mesh predicates (`is_watertight()` etc.), and save/load of `.clayspace` plus mesh export (OBJ/FBX/PLY/glTF).

Deformers are the one documented exception until the tape gains deformer opcodes; every other construct the C++ library implements SHALL be reachable from Python, specifically:

- **Strokes**: a stroke primitive taking per-point positions and radii plus an intra-stroke smoothing parameter, addable to a layer like any other primitive.
- **Extended combine modes**: `groove`, `tongue`, `pipe`, `engrave`, `emboss`, `inset`, `shell`, and `replace` SHALL be selectable as ops, with the parameters each mode consumes (blend radius, and the item rounding that groove/tongue read as channel half-width) settable from Python.
- **Voxel grids**: palette management, single/brush/box/line edits, mirrored edits, flood select, greedy meshing, occupancy queries, and the voxel↔SDF bridges (rasterize a document into voxels; sample the step field).
- **Mesher selection**: the marching, surface-nets, and dual-contouring meshers SHALL be selectable, with dual contouring reachable only through its experimental opt-in.
- **Picking**: scene raycast returning hit position, normal, and layer/item attribution; surface snapping; voxel cell and entry-face picking; selection bounds.

#### Scenario: Authoring flow
- **WHEN** a script builds a layer with `body.add(clay.Sphere(r=1.0), blend=clay.Smooth(0.2), color="#38a6cf")`, meshes it, and saves `body.clayspace`
- **THEN** the resulting file opens in any claycore consumer (including the iPad app) and evaluates identically

#### Scenario: Stroke authoring
- **WHEN** a script adds a stroke of N points with per-point radii and meshes the layer
- **THEN** the field matches the same stroke authored through the C++ API, and the stroke is one edit item (not N)

#### Scenario: Extended op reachable
- **WHEN** a script adds an item with an extended op such as `clay.Op.GROOVE` and a blend radius
- **THEN** the evaluated field equals the C++ result for the same document, and the op survives a `.clayspace` round trip

#### Scenario: Voxel round trip
- **WHEN** a script fills a voxel grid, greedy-meshes it, saves the document, and reloads it
- **THEN** the reloaded grid has identical occupancy and palette, and the mesh is unchanged

#### Scenario: Mesher selection
- **WHEN** the same document is meshed with the marching and surface-nets meshers
- **THEN** both produce non-empty geometry and the surface-nets result has strictly fewer triangles

#### Scenario: Picking from Python
- **WHEN** a script raycasts a document along a ray that hits a known item
- **THEN** the hit reports a position on the surface, an outward normal, and the id of the layer and item that own that surface

## ADDED Requirements

### Requirement: numpy batch forms for the widened surface
Every widened API that can take many inputs SHALL accept numpy arrays in the same layout discipline as evaluation — stroke points as `(N,4) float32` (xyz + radius), voxel coordinates as `(N,3) int32`, and rays as `(N,6) float32` (origin + direction) — returning arrays rather than Python lists, so batch workloads never loop in Python. Sequences of tuples SHALL remain accepted for ergonomics.

#### Scenario: Batch ray query
- **WHEN** a script raycasts with an `(N,6) float32` array of rays
- **THEN** it receives arrays of hit flags, distances, positions, and normals of matching length, and no per-ray Python call is required

#### Scenario: Batch voxel edit
- **WHEN** a script sets voxels from an `(N,3) int32` coordinate array
- **THEN** every listed cell is written in one call
