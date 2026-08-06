# python-bindings Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
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

### Requirement: numpy-native data exchange
All batch APIs SHALL accept and return numpy arrays without copies where layout permits: points as `(N,3) float32` in, distances `(N,) float32` out, gradients `(N,3) float32`, colors `(N,3)/(N,4)`, mesh buffers as arrays. Evaluation SHALL release the GIL.

#### Scenario: Zero-copy evaluation
- **WHEN** a C-contiguous `(N,3) float32` array is passed to `eval`
- **THEN** no input copy is made, the GIL is released during evaluation, and a `(N,) float32` array is returned

### Requirement: Backend selection from Python
`pyclay` SHALL expose backend enumeration and per-call backend selection (`backend="cpu" | "metal" | "cuda" | "opencl"`), defaulting to CPU, with a clear error when an unavailable backend is requested.

#### Scenario: Unavailable backend
- **WHEN** `backend="cuda"` is requested on a machine without CUDA
- **THEN** a Python exception names the backend as unregistered and lists available backends

### Requirement: Wheels
`pyclay` SHALL be packaged with scikit-build-core and built via cibuildwheel for macOS (arm64, x86-64), Linux (manylinux x86-64/aarch64), and Windows (x86-64), always containing the CPU backend and containing GPU backends where the build platform provides them.

#### Scenario: CPU wheel just works
- **WHEN** `pip install pyclay` runs on a clean supported machine and the quickstart script executes
- **THEN** evaluation, meshing, and file export succeed with no GPU or extra system dependencies

### Requirement: Python as test harness
The golden-scene test corpus and property-test scenarios SHALL be authorable in Python against `pyclay`, and CI SHALL run these Python-driven suites against the same library binary the native tests use.

#### Scenario: Golden corpus from Python
- **WHEN** CI runs the Python corpus generator and the meshing gate
- **THEN** every generated scene meshes watertight/manifold and matches stored golden hashes (or intentional-change review is triggered)

### Requirement: numpy batch forms for the widened surface
Every widened API that can take many inputs SHALL accept numpy arrays in the same layout discipline as evaluation — stroke points as `(N,4) float32` (xyz + radius), voxel coordinates as `(N,3) int32`, and rays as `(N,6) float32` (origin + direction) — returning arrays rather than Python lists, so batch workloads never loop in Python. Sequences of tuples SHALL remain accepted for ergonomics.

#### Scenario: Batch ray query
- **WHEN** a script raycasts with an `(N,6) float32` array of rays
- **THEN** it receives arrays of hit flags, distances, positions, and normals of matching length, and no per-ray Python call is required

#### Scenario: Batch voxel edit
- **WHEN** a script sets voxels from an `(N,3) int32` coordinate array
- **THEN** every listed cell is written in one call

### Requirement: Deformer modifiers on primitives
Primitives SHALL expose chainable deformer modifiers — `.twist(k)`, `.bend(k)`, `.taper(y0, y1, s0, s1, ease=…)`, and `.displace(amplitude, frequency)` — matching the `docs/05` §10 sample, applied in call order. Deformers SHALL be inspectable on the primitive and SHALL survive a `.clayspace` round trip. Constructs still absent from the tape (`wrap_around`, the two-subtree transitions) SHALL raise a clear error rather than silently doing nothing.

#### Scenario: Twisted primitive from the sample
- **WHEN** a script adds `clay.Box(size=(0.4, 0.4, 0.4)).twist(1.2)` with `op=clay.Op.SUBTRACT`
- **THEN** the document evaluates with the twist applied, and the same document authored through the C++ API yields the same field

#### Scenario: Chained deformers keep order
- **WHEN** a primitive is built as `.twist(1.0).taper(...)` and again as `.taper(...).twist(1.0)`
- **THEN** the two documents evaluate to different fields, each matching its authoring order

### Requirement: Transition combine modes from Python
`clay.Op` SHALL expose `TRANSITION_LINEAR` and `TRANSITION_RADIAL`, parameterized by a `transition=` argument taking `clay.TransitionLinear(a, b, ease=…)` or `clay.TransitionRadial(r0, r1, ease=…)`. Using a transition op without its parameters SHALL raise a clear error, and the parameters SHALL survive a `.clayspace` round trip.

#### Scenario: Morph between two shapes
- **WHEN** a script adds a second primitive with `op=clay.Op.TRANSITION_LINEAR` and a segment spanning the scene
- **THEN** the document evaluates to the first shape at one end of the segment and the second at the other

#### Scenario: Missing parameters are rejected
- **WHEN** a transition op is used without a `transition=` argument
- **THEN** a `ValueError` names the missing argument instead of silently producing a degenerate morph

### Requirement: Profile primitives from Python
The module SHALL expose profile objects (`Circle2`, `Box2`, `Hexagon2`, `Triangle2`, `Trapezoid2`, `Vesica2`, `Polygon`) and the lifts that consume them (`Extrude(profile, half_depth)`, `Revolve(profile, offset)`), with `Polygon` accepting an `(N,2)` float32 array or a sequence of pairs. Profiles SHALL survive a `.clayspace` round trip, and lifting an unsupported open curve SHALL raise a clear error naming the flattening workaround.

#### Scenario: Extruded polygon from numpy
- **WHEN** a script extrudes a polygon given as an `(N,2)` array and meshes the document
- **THEN** the mesh is watertight and its cross-section matches the polygon

#### Scenario: Revolved profile builds a torus
- **WHEN** a circle profile of radius r is revolved at offset R
- **THEN** the field matches `clay.Torus(R=R, r=r)` within tolerance

### Requirement: Repetition modifiers on primitives
Primitives SHALL expose chainable repetition modifiers: `.repeat_grid(spacing, counts=None)` — infinite when `counts` is omitted, finite otherwise — and `.repeat_radial(count, offset)`. Repetition SHALL survive a `.clayspace` round trip and compose with deformers.

#### Scenario: Finite array from Python
- **WHEN** a script adds `clay.Sphere(r=0.2).repeat_grid(spacing=1.0, counts=(2, 0, 0))`
- **THEN** the document contains copies at each cell of the range and none beyond it

#### Scenario: Radial array from Python
- **WHEN** a script adds a primitive with `.repeat_radial(count=6, offset=1.0)`
- **THEN** the field is periodic under rotation by one sixth of a turn about the axis

### Requirement: Every tape primitive has a Python class
The module SHALL expose a class for every primitive the tape can express, with the same placement keywords as the existing ones (`position`, `rotation_axis_angle`, `scale`) and parameter names matching the kernel's documented meaning.

#### Scenario: Full primitive coverage
- **WHEN** a test enumerates the module's primitive classes and adds one of each to a document
- **THEN** every one evaluates, meshes, and round-trips through `.clayspace`

### Requirement: Paint brush and mirrored paint in Python
The module SHALL expose `paint_brush(cell, size, index, shape=...)` and `paint_mirrored(cell, index, axes=...)`, matching the C++ surface so that every brush and mirrored edit implemented by the engine is reachable from Python.

#### Scenario: Recolouring a region without adding voxels
- **WHEN** a script calls `paint_brush` over a region that is partly empty
- **THEN** the occupied cells change palette index and the occupied count is unchanged

#### Scenario: Mirrored paint
- **WHEN** a script calls `paint_mirrored` with `axes="x"` on a symmetric model
- **THEN** both the authored cell and its mirror are recoloured

### Requirement: Brush shape selection from Python
Brush operations SHALL accept a `shape` argument taking `"cube"` (default) or `"sphere"`, matching the string-argument convention used elsewhere in the bindings. An unrecognised shape SHALL raise rather than silently falling back.

#### Scenario: Sphere brush from Python
- **WHEN** a script calls `set_brush(cell, 5, index, shape="sphere")`
- **THEN** fewer cells are set than the cube of the same size, and every set cell lies within the sphere radius

#### Scenario: Unknown shape is rejected
- **WHEN** a script passes `shape="blob"`
- **THEN** the call raises a `ValueError` naming the accepted shapes

### Requirement: Falloff arguments and sculpting verbs in Python
The module SHALL expose the sculpting verbs `sculpt_smooth`, `sculpt_inflate`, `sculpt_flatten` and `sculpt_pinch`, and SHALL accept `falloff`, `strength` and `seed` arguments on the brush operations. Falloff SHALL be named by string, matching the convention used for `shape` and `axes`, and an unrecognised name SHALL raise rather than silently defaulting.

#### Scenario: Soft brush from Python
- **WHEN** a script calls `set_brush(cell, 9, index, falloff="smooth", strength=0.5)`
- **THEN** fewer cells are set than with the default constant falloff, and repeating the call with the same seed sets the same cells

#### Scenario: Sculpting a surface from Python
- **WHEN** a script calls `sculpt_smooth` or `sculpt_inflate` on a grid
- **THEN** the occupied set changes according to the verb and no exception is raised

#### Scenario: Unknown falloff is rejected
- **WHEN** a script passes `falloff="wobble"`
- **THEN** the call raises a `ValueError` naming the accepted curves

### Requirement: Node and layer editing from Python
The module SHALL expose editing of an existing document, not only construction: for a node, setting its transform, primitive, colour, op/blend/rounding, moving it to a new parent and index, and removing it; for a layer, adding, removing, reordering, setting visibility and setting its transform; and for a stroke, appending points and trimming the last N. Every edit SHALL be addressed by node or layer id, and ids SHALL remain valid across unrelated edits.

#### Scenario: Moving a placed item
- **WHEN** a script adds a sphere, keeps its node id, and later sets a new transform on that id
- **THEN** the field reflects the new position and the node id is unchanged

#### Scenario: Removing an item
- **WHEN** a script removes a node by id
- **THEN** the node is gone from the layer and the remaining nodes keep their ids

#### Scenario: Layer visibility
- **WHEN** a layer is set invisible
- **THEN** the document evaluates as though that layer's content were absent, and setting it visible again restores the original field exactly

#### Scenario: Editing a stroke in place
- **WHEN** points are appended to an existing stroke and then the last N are trimmed
- **THEN** the stroke's field matches a stroke authored with the surviving points

#### Scenario: Every edit goes through the command vocabulary
- **WHEN** any editing entry point is called
- **THEN** it applies commands from `scene::Command` rather than mutating the document directly, so its semantics match what the document format records. Layer reorder SHALL be documented as the remove-then-add pair the vocabulary expresses it with; every other edit is a single command.

### Requirement: Undo from Python
The module SHALL expose an opt-in undo stack per document: enabling it, `undo`, `redo`, the undo and redo depths, and grouping so a burst of edits undoes as one step. With no stack attached a document SHALL behave exactly as it does without this feature. Once attached, every editing entry point SHALL record its own inverse, so no reachable edit escapes undo.

#### Scenario: Undo restores the previous state exactly
- **WHEN** an edit is performed on a document with undo enabled and then undone
- **THEN** the document serializes bit-identically to its state before the edit

#### Scenario: Redo reapplies
- **WHEN** an edit is undone and then redone
- **THEN** the document matches the state after the original edit, and the redo depth returns to zero

#### Scenario: A stroke undoes as one step
- **WHEN** N point-append edits are made to one stroke and undo is called once
- **THEN** all N points are gone and the undo depth drops by one

#### Scenario: Grouped edits undo together
- **WHEN** several edits are bracketed by begin/end group and undo is called once
- **THEN** every edit in the group is reversed

#### Scenario: A new edit clears the redo stack
- **WHEN** an edit is undone and a different edit is then performed
- **THEN** the redo depth is zero

#### Scenario: Undo costs nothing when unused
- **WHEN** a document is edited without undo enabled
- **THEN** no inverse is recorded and the undo depth entry point reports the feature is off

### Requirement: wrap_around as a chainable modifier
`Prim.wrap_around(x0, x1)` SHALL append a wrap deformer to the primitive's chain, replacing the stub that raises. It SHALL compose with the other modifiers in call order and survive a `.clayspace` round trip.

#### Scenario: Wrapping from Python
- **WHEN** a script adds a primitive with `.wrap_around(x0, x1)`
- **THEN** the document evaluates a wrapped field and no exception is raised

#### Scenario: Degenerate interval is refused
- **WHEN** `x0` and `x1` are equal
- **THEN** the call raises rather than producing a zero-radius cylinder

### Requirement: elongate as a chainable modifier
`Prim.elongate(h)` SHALL append an elongation to the primitive's chain, taking the per-axis half-extent. It SHALL compose with the other modifiers in call order and survive a `.clayspace` round trip.

#### Scenario: Stretching from Python
- **WHEN** a script adds `clay.Sphere(r=0.5).elongate((1.0, 0, 0))`
- **THEN** the document evaluates a capsule-like field stretched along X

#### Scenario: Negative extents are refused
- **WHEN** any component of `h` is negative
- **THEN** the call raises, since a half-extent has no meaning below zero

### Requirement: bend_linear and bend_radial as chainable modifiers
`Prim.bend_linear(a, b, v, ease=0)` and `Prim.bend_radial(r0, r1, dz, ease=0)` SHALL append the corresponding deformer, compose in call order, and survive a `.clayspace` round trip.

#### Scenario: Ramping from Python
- **WHEN** a script adds a primitive with `.bend_linear(...)` or `.bend_radial(...)`
- **THEN** the document evaluates a displaced field and no exception is raised

#### Scenario: A degenerate span is refused
- **WHEN** the two ramp endpoints coincide, or `r0` equals `r1`
- **THEN** the call raises, since the ramp would divide by zero

### Requirement: elongate_axis as a chainable modifier
`Prim.elongate_axis(h)` SHALL append a per-axis elongation, taking the half-extents. It SHALL compose in call order and survive a `.clayspace` round trip.

#### Scenario: Stretching an asymmetric primitive from Python
- **WHEN** a script elongates a cone per axis
- **THEN** the document evaluates a stretched field and no exception is raised

#### Scenario: Negative extents are refused
- **WHEN** any component of `h` is negative
- **THEN** the call raises

### Requirement: grab and pose from Python
`Prim.grab(center, radius, displacement, ease=0, front_facing=False)` and `Prim.pose(center, radius, ...)` SHALL append the corresponding deformer, and `VoxelGrid.grab(cell, radius, displacement, ...)` SHALL apply the voxel verb. All SHALL compose in call order and survive a `.clayspace` round trip.

#### Scenario: Grabbing from Python
- **WHEN** a script grabs part of a primitive
- **THEN** the field changes only within the radius

#### Scenario: A non-positive radius is refused
- **WHEN** the radius is zero or negative
- **THEN** the call raises, since the falloff would divide by zero

### Requirement: pose_line as a chainable modifier
`Prim.pose_line(a, b, axis, angle, ease=0)` SHALL append a line-gradient pose, compose in call order, and survive a `.clayspace` round trip.

#### Scenario: Posing along a limb from Python
- **WHEN** a script poses a capsule from one end to the other
- **THEN** the anchor end is unmoved and the far end is rotated

#### Scenario: A degenerate segment is refused
- **WHEN** the anchor and end coincide
- **THEN** the call raises, since the ramp would divide by zero

### Requirement: Masks from Python
The module SHALL expose a mask on a layer: enabling it, painting with the brush vocabulary, the region operations, sampling at an (N, 3) array of world positions, and passing it to voxel edits.

#### Scenario: Freezing from Python
- **WHEN** a script masks a region and then stamps a brush across it
- **THEN** the masked cells are unchanged

#### Scenario: Sampling returns an array
- **WHEN** a script samples the mask at an (N, 3) array of positions
- **THEN** it receives an (N,) array of values in [0, 1]

### Requirement: Strokes from Python
The module SHALL expose a preset, resolution of an (N, samples) array of stroke samples into stamps, and application of a stroke to a voxel grid or an SDF layer with an optional mask.

#### Scenario: Resolving a stroke returns stamps
- **WHEN** a script resolves a stroke from an array of samples
- **THEN** it receives one entry per stamp, with position, radius and strength

#### Scenario: A stroke applied to a layer is undoable
- **WHEN** a script applies a stroke to an SDF layer with undo enabled and undoes it
- **THEN** the document is restored exactly

### Requirement: The flags from Python
The module SHALL expose reading and setting a layer's ghost and lock flags, and SHALL raise on an edit to a protected layer rather than silently dropping it.

#### Scenario: Editing a locked layer raises
- **WHEN** a script adds an item to a locked layer
- **THEN** an error is raised and the layer is unchanged

### Requirement: Curves from Python
The module SHALL expose a curve constructor taking control points with per-point radius and type, optional Bezier handles, a closed flag and a tolerance, and SHALL expose replacing an existing item's points.

#### Scenario: Authoring a curve
- **WHEN** a script builds a closed spline curve and evaluates the document
- **THEN** the field is a smooth tube through the control points

#### Scenario: Editing a curve
- **WHEN** a script replaces a placed curve's points
- **THEN** the field changes, and undoing restores it

### Requirement: Cuts from Python
The module SHALL expose resolving a cut frame and shape into an item, and placing it on a layer with a chosen op, defaulting the swept region to the document's own bounds.

#### Scenario: Cutting a hole
- **WHEN** a script cuts a circle from a solid and evaluates the document
- **THEN** the field reports empty inside the circle's sweep and solid outside it

### Requirement: The new verbs from Python
The module SHALL expose fill-cavities, scrape, smudge and carve-with-alpha, taking the alpha as an (H, W) array.

#### Scenario: Carving with an alpha array
- **WHEN** a script carves with an (H, W) alpha that is opaque on one half
- **THEN** material is removed under the opaque half only

### Requirement: Repair from Python
The module SHALL expose the report and both repairs, with the report as a readable structure rather than a bare count.

#### Scenario: Reporting then repairing
- **WHEN** a script reports a hollow shell, fills its voids, and reports again
- **THEN** the first report says not airtight and the second says airtight

### Requirement: Lofts from Python
The module SHALL expose a loft taking two or more profiles and a half-depth, with an easing curve over the interpolation.

#### Scenario: Lofting a circle to a box
- **WHEN** a script lofts a circle to a box and evaluates the ends
- **THEN** each end matches its own profile

### Requirement: Sweeps from Python
The module SHALL expose a swept item taking guide control points with their types and tolerance, and two or more profiles.

#### Scenario: Sweeping a circle along a curve
- **WHEN** a script sweeps a circle along a spline guide
- **THEN** the field is material along the guide and empty away from it

### Requirement: Sampling a field from Python
The module SHALL expose building a volume by sampling an existing document over a region at a stated resolution, and placing the result as an item.

#### Scenario: Baking a document into a volume
- **WHEN** a script samples a document into a volume and evaluates both
- **THEN** the fields agree near the surface within the sampling tolerance

#### Scenario: The sampling can be inspected
- **WHEN** a script asks a volume for its cell size, band, brick count, size and bounds
- **THEN** it gets them, so the cost of a chosen resolution is visible before the volume is used

#### Scenario: The two halves of the bound can be told apart
- **WHEN** a script asks whether a point lands where the volume kept samples
- **THEN** it is told, so a test can hold the interpolated region and the bounded region to their different guarantees

### Requirement: The C ABI does not yet build one
A volume is only reachable by sampling something, and nothing in the C ABI can supply the samples until mesh import lands. Constructing one through the C ABI SHALL be refused rather than returning an item that could only ever be empty. The enumerator SHALL remain declared, because the value appears in saved documents, and documents containing one SHALL still load, evaluate and mesh.

#### Scenario: Constructing a volume through the C ABI is refused
- **WHEN** a C caller asks for an item of the volume primitive type
- **THEN** the call fails with an invalid-argument error rather than returning an empty item

### Requirement: Sampling a mesh from Python
The module SHALL expose building a volume from a loaded mesh at a stated resolution, so that a script can import a model and immediately place it in a document.

#### Scenario: A loaded mesh becomes an item
- **WHEN** a script loads a mesh, samples it into a volume and adds it to a layer
- **THEN** the document's field describes the mesh's shape

#### Scenario: An empty mesh is refused
- **WHEN** a script samples a mesh with no triangles
- **THEN** it gets an error rather than an empty volume

### Requirement: The sign can be inspected, not merely trusted
The import rests on the claim that the generalized winding number behaves — that a hole does not flip a half-space, and that summarizing distant geometry does not move a point across the surface. The module SHALL expose the underlying queries so a script can check that claim rather than take it on trust.

The acceleration structure SHALL be built where the caller can see it rather than per query, because building it is the expensive part.

#### Scenario: The winding number can be plotted across a hole
- **WHEN** a script queries the winding number along a line passing out through an opening in a mesh
- **THEN** it gets a continuous curve passing through a half rather than a step

#### Scenario: The approximation can be checked against the exact sum
- **WHEN** a script queries the winding number with summarization disabled and with it enabled
- **THEN** it can compare them, and no point changes which side of the surface it is on

#### Scenario: A mesh loads from a file
- **WHEN** a script loads a mesh by path
- **THEN** it gets a mesh it can sample, and an unsupported extension is reported rather than guessed at

