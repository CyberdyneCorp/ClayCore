# python-bindings Specification

## Purpose
The surface a script, a test and a notebook use, and the reason this library can
be exercised without an application.

numpy-native throughout, because a binding that copies arrays across the
boundary is one nobody uses twice. `pyclay` is also the test harness the examples
and the parity gates are written in, which is why its coverage is held EQUAL to
the C ABI's rather than allowed to be a convenience subset —
`check_binding_parity` fails on a capability reachable from one and not the
other.

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
The module SHALL expose a swept item taking guide control points with their types and tolerance, and two or more profiles. A PLACED sweep's guide SHALL be editable through the same point-list replace a curve uses, since a guide is an ordinary curve; the replace SHALL refuse what the constructor refuses — closing a guide, which the constructor offers no flag for at all, and leaving one with fewer than two points.

#### Scenario: Sweeping a circle along a curve
- **WHEN** a script sweeps a circle along a spline guide
- **THEN** the field is material along the guide and empty away from it

#### Scenario: A placed sweep's guide is reshaped
- **WHEN** a script replaces a placed sweep's points with a differently bent guide
- **THEN** the field follows the new guide

#### Scenario: A guide cannot be closed after the fact
- **WHEN** the replace asks for a closed guide
- **THEN** it is refused with the reason, and the sweep is unchanged

#### Scenario: A guide cannot be cut below two points
- **WHEN** the replace hands a placed sweep a single-point guide
- **THEN** it is refused with the reason, and the sweep still follows its guide

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

### Requirement: Relaxing from Python
The module SHALL expose relaxing a volume, with the strength, the kernel radius, the iteration count and the region all under the caller's control.

The binding SHALL state plainly that relax BAKES: what comes back is a sampled volume, not the edit list that went in, so the items and their editable parameters are gone. That is inherent to relaxing a field rather than a limitation of the implementation, and a caller needs to know it before choosing a resolution.

#### Scenario: A document is relaxed into a volume
- **WHEN** a script samples a document, relaxes it and adds the result to a layer
- **THEN** the document's field is the smoothed shape

#### Scenario: The parameters do what they say
- **WHEN** a script relaxes the same shape at increasing strength or iterations
- **THEN** the surface is progressively smoother

### Requirement: An imported shape can be smoothed from the C ABI
An app's reachable workflow is to import a mesh and then smooth it, so the C ABI SHALL be able to relax an item that carries a volume. Asking it to relax an item that carries anything else SHALL be refused rather than ignored.

#### Scenario: A C-built volume is relaxed
- **WHEN** a C caller samples a mesh into an item and relaxes it
- **THEN** the item's field is the smoothed shape

#### Scenario: Relaxing something that is not a volume is refused
- **WHEN** a C caller asks to relax an item carrying an ordinary primitive
- **THEN** the call fails rather than silently doing nothing

### Requirement: Flattening from Python
The module SHALL expose flattening against a plane, sampling from a DOCUMENT, with the plane, the strength and the region under the caller's control. It SHALL also expose flattening a volume, for a shape with no document behind it — an imported mesh.

The binding SHALL state that flatten BAKES, for the same reason relax does, and SHALL state that the plane is the caller's to supply — the engine has no camera and does no picking.

#### Scenario: A document is flattened into a volume
- **WHEN** a script samples a document, flattens it against a plane and adds the result to a layer
- **THEN** the document's field has a facet on that plane

#### Scenario: The parameters do what they say
- **WHEN** a script flattens the same shape with increasing strength
- **THEN** the surface is progressively closer to the plane

#### Scenario: A region-less request is refused
- **WHEN** a script asks to flatten with no region
- **THEN** it gets an error rather than a shape replaced by a half-space

#### Scenario: A degenerate plane is refused
- **WHEN** a script passes a zero-length plane normal
- **THEN** it gets an error rather than a volume shaped by an arbitrary direction

### Requirement: An imported shape can be faceted from the C ABI
The C ABI SHALL be able to flatten an item that carries a volume, beside the existing relax entry point, so an imported scan can be faceted from an app. Asking it to flatten an item carrying anything else SHALL be refused rather than ignored.

#### Scenario: A C-built volume is flattened
- **WHEN** a C caller samples a mesh into an item and flattens it against a plane
- **THEN** the item's field has a facet on that plane

#### Scenario: Flattening something that is not a volume is refused
- **WHEN** a C caller asks to flatten an item carrying an ordinary primitive
- **THEN** the call fails rather than silently doing nothing

### Requirement: Pulling a tendril from Python
The module SHALL expose resolving a drag into a tendril, with the anchor, the normal, the path, the base and tip radii and the taper under the caller's control.

The binding SHALL state that this ADDS material rather than moving it: ZBrush's snakehook pulls existing surface, so the body dimples slightly where the tendril came from, and this does not.

#### Scenario: A script pulls a horn
- **WHEN** a script resolves a drag from a sphere's surface and adds the result to the layer
- **THEN** the document's field is the sphere with a tendril attached

#### Scenario: The taper is under control
- **WHEN** a script resolves the same drag with different tip radii
- **THEN** the tendril ends thicker or thinner accordingly

### Requirement: Magnify and pinch from Python
The module SHALL expose the radial scale deformer on items, alongside the other deformers, and the voxel magnify verb alongside the other sculpt verbs.

The binding SHALL state that one signed strength covers both directions, so a caller does not go looking for a separate pinch.

#### Scenario: A script swells a feature
- **WHEN** a script adds the deformer to an item with a positive strength
- **THEN** the document's field shows the surface swelled about that centre

#### Scenario: A script creases an edge
- **WHEN** the same call is made with a negative strength
- **THEN** the surface gathers toward the centre instead

### Requirement: Masking from Python is a stroke
The module SHALL let a script paint a mask from a resolved stroke, invert a mask within a box, and fill a box with a value. It SHALL accept a mask on the volume relax and flatten entry points.

The binding SHALL state that the footprint is derived from the stamp's world radius, so a script does not go looking for a size in mask cells and find none.

#### Scenario: A script paints a mask along a stroke
- **WHEN** a script resolves a stroke and applies it to a mask
- **THEN** the mask reads masked along the path

#### Scenario: A script freezes a region and relaxes around it
- **WHEN** a script relaxes a volume with a mask covering part of the relaxed region
- **THEN** the field under the mask is unchanged and the field beside it is smoothed

### Requirement: Mask extrude from Python
The module SHALL let a script convert a mask to a field and extrude a masked patch of a document or of a voxel grid into a new one.

The binding SHALL state that the mask is the region — there is no region radius to supply, unlike relax and flatten — because that is the first thing a script author will go looking for.

A refusal SHALL raise rather than return an empty result.

#### Scenario: A script extracts a plate from a document
- **WHEN** a script masks part of a document's surface and extrudes it
- **THEN** it gets back a volume holding the plate, which meshes and evaluates like any other

#### Scenario: A script extracts from voxels
- **WHEN** a script extrudes a masked region of a voxel grid
- **THEN** it gets back a new grid holding the extract, with the source's colours

#### Scenario: An impossible extrude raises
- **WHEN** a script extrudes with an empty mask
- **THEN** the call raises rather than returning something empty

### Requirement: Noise from Python
The module SHALL expose the noise deformer on items alongside the others, with the amplitude, frequency, octaves, gain and seed under the caller's control.

The binding SHALL state that the seed is an ordinary parameter rather than global state, so two items with the same seed look the same and an item's appearance does not depend on the order it was compiled in.

#### Scenario: A script roughens a shape
- **WHEN** a script adds the noise deformer to an item
- **THEN** the document's field shows an irregular surface

#### Scenario: The seed is reproducible
- **WHEN** the same script runs twice
- **THEN** it produces the same field both times

### Requirement: The Move brush from Python
The module SHALL let a script drag a layer's surface with a world centre, radius and displacement, and SHALL report which nodes took a warp.

The binding SHALL state that this differs from putting a `grab` on one item — that a grab is per item and in that item's own frame, so it pulls one item's share of a blended form and leaves the rest — because a script author reaching for `prim.grab(...)` will otherwise not find out until the result looks wrong.

It SHALL also state that the surface moves less than the displacement asked for, and why.

#### Scenario: A script drags a blended form
- **WHEN** a script moves a layer built from two blended items, centred between them
- **THEN** both sides move and the result is symmetric about the drag's centre

#### Scenario: The whole drag is one undo step
- **WHEN** a script with undo enabled moves a form and undoes it
- **THEN** the document is back where it started in a single step

### Requirement: Previewing a move from Python
The module SHALL expose previewing a drag, returning the nodes it would warp without touching the document.

#### Scenario: A script previews before committing
- **WHEN** a script previews a drag and then applies it
- **THEN** the preview leaves the document unchanged and names the same nodes the move reports

### Requirement: Choosing a flatten mode from Python
The module SHALL expose the flatten mode, defaulting to two-sided, and SHALL name the vendor brushes the one-sided modes correspond to so a caller can find them.

#### Scenario: A script polishes without filling
- **WHEN** a script flattens in cut-only mode over a surface with a hollow in it
- **THEN** the hollow is untouched and the high material is planed off

### Requirement: Trimming from Python
The module SHALL expose building a trim shape from an open curve and the side it covers, alongside the closed-lasso constructor, and SHALL say which is which so a caller does not reach for the lasso when it means a trim.

#### Scenario: A script trims a form in half
- **WHEN** a script resolves an open curve as a trim and places it with subtract
- **THEN** one side of the form is removed and the other is untouched

### Requirement: A topological move from Python
The module SHALL expose sampling a document through a topological move, with the anchor, geodesic radius, displacement and easing under the caller's control, and SHALL say how it differs from the Euclidean move so a caller knows which one it wants.

#### Scenario: A script moves one finger and not its neighbour
- **WHEN** a script applies a topological move to one of two adjacent parts
- **THEN** only the part connected to the anchor along the material moves

### Requirement: Drawing a tube from Python
The module SHALL expose resolving a path into a tube, with the point type, the start/middle/end radii, closure and an optional profile under the caller's control, and SHALL say which choices cost exactness.

#### Scenario: A script draws a tapered tube
- **WHEN** a script resolves a path with a wide start and a narrow end
- **THEN** it receives an item that tapers along its length and can be added to a layer

### Requirement: Armatures from Python
`pyclay` SHALL expose armatures with the same semantics as the C ABI, taking nodes as an (N, 4) array of position and radius plus an (N,) array of parent indices, matching how strokes and sweeps already take their points, and an optional (N,) array of signs, +1 or -1, absent meaning all positive. The signs SHALL be builder state readable and writable as a property, the placed sign edit SHALL be reachable through the armature edit call, and any sign other than +1 or -1 SHALL be refused.

`check_binding_parity` SHALL report no capability without a C counterpart.

#### Scenario: Both bindings agree
- **WHEN** the same armature is built through the C ABI and through pyclay
- **THEN** the two documents evaluate identically

#### Scenario: A negative node from Python
- **WHEN** a script builds an armature with one node's sign -1 and the same rig through the C ABI signs setter
- **THEN** the two documents evaluate identically, and the hollow is present in both

#### Scenario: The example carves
- **WHEN** the armature example runs
- **THEN** it builds a rig holding negative nodes and renders the carved result alongside the all-positive rig

### Requirement: MeshSculptor
`pyclay` SHALL expose a `MeshSculptor` constructed over a `Mesh` — including the borrowed mesh a mesh LAYER hands back, so sculpting a layer edits the document's own triangles rather than a copy.

It SHALL expose one stamp, one whole stroke, a ray query, a BVH refresh, and the counts a caller needs to see the structure it built.

Sculpting a borrowed mesh whose layer has been removed SHALL raise, as every other borrowed-mesh access already does.

A LOCKED or GHOSTED layer SHALL refuse to be sculpted, because those flags mean "never edited" and a vertex displacement is an edit.

#### Scenario: A mesh layer is sculpted in place
- **WHEN** a document's mesh layer is fetched, sculpted through a `MeshSculptor`, and the layer is read back
- **THEN** the layer's positions show the edit and its indices and quads are unchanged

#### Scenario: A protected layer refuses
- **WHEN** a sculptor is asked to stamp a mesh borrowed from a locked or ghosted layer
- **THEN** it raises and the mesh is unchanged

### Requirement: The verbs and their settings from Python
`pyclay` SHALL expose the verb set as an enumeration and the brush settings as keyword arguments with the same defaults the C++ header declares, so a Python caller and a C caller reading the same documentation get the same stroke.

The signed convention SHALL be preserved: one `pinch` verb whose negative strength magnifies, and one `flatten` verb taking the `TwoSided` / `CutOnly` / `FillOnly` mode.

Invalid arguments SHALL raise rather than be clamped, matching the C ABI's refusals so the two do not disagree about what a value means.

#### Scenario: The bindings agree with the ABI about a refusal
- **WHEN** a non-positive radius or an out-of-range iteration count is passed
- **THEN** Python raises and C returns `CLAY_ERROR_INVALID_ARGUMENT`

### Requirement: VertexDeltas from Python
`pyclay` SHALL expose the vertex-delta record with its vertex count, revert, apply and clear, and it SHALL be usable as one undo step across a whole gesture.

#### Scenario: Undo from Python is bit-exact
- **WHEN** a stroke is applied with a record and the record is reverted
- **THEN** the mesh's positions and normals compare equal bit for bit to a copy taken before the stroke

### Requirement: Binding parity holds
`tools/check_binding_parity.py` SHALL pass with every capability added here reachable from the C ABI, or exempt with a stated reason.

#### Scenario: The gate passes
- **WHEN** the parity gate runs against the built module and `clay.h`
- **THEN** it reports no unmatched capability and no stale exemption

### Requirement: Quad meshing from Python
`pyclay` SHALL expose quad meshing on a document and on a voxel grid, taking a lattice cell size OR a target quad count, a tolerance, an iteration cap and a mode, and returning a `Mesh`.

The mode SHALL be spelled as a string, as the mesher choice already is, and an unrecognised one SHALL raise rather than fall back. Asking a document for the voxel-only faces mode SHALL raise, for the same reason the C ABI refuses it.

Every entry point SHALL carry a name in the C ABI under the prefix rules the binding-parity gate applies, so the surface stays reachable from C without an exemption.

The docstrings SHALL state that this is a lattice-derived quad grid and NOT field-aligned retopology — no edge loops, no feature-placed poles, not animation-ready — and SHALL state that a target is approached rather than hit.

The count knobs SHALL take the C ABI's rules, value for value, so that one input gets one answer in both bindings. A tolerance or an iteration cap of zero or below SHALL mean the DEFAULT rather than raise, because that is what `clay_quad_params` documents and what a C caller who declared only the original struct layout sends; a negative iteration cap, a non-finite tolerance, a tolerance of 1 or more, and a target above `CLAY_MAX_BATCH` SHALL be refused in both. A target too large to be read as an integer at all SHALL raise this API's own error and not leak the binding library's cast failure.

#### Scenario: The count knobs default and refuse identically in both bindings
- **WHEN** a script passes a tolerance or an iteration cap of zero, the values the C descriptor treats as "use the default"
- **THEN** the call meshes with the defaults rather than raising
- **AND** a negative iteration cap, a tolerance of 1 or more, and a target above the batch ceiling each raise, as they do across the C ABI

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

### Requirement: VoxelGrid.rasterize_mesh
`pyclay` SHALL expose mesh rasterization on `VoxelGrid`, taking a `Mesh` and an optional region, with the region defaulting to the mesh's own bounds.

Invalid arguments SHALL raise rather than be clamped, matching the C ABI's refusals so the two do not disagree about what a value means.

The GIL SHALL be released around the work, as it is for the other heavy grid calls.

#### Scenario: An imported model reaches the voxel verbs
- **WHEN** a mesh is loaded and rasterized, and a sculpting verb is then applied
- **THEN** the grid is occupied and the verb changes cells, with no document constructed on the way

#### Scenario: The bindings agree with the ABI about a refusal
- **WHEN** an inverted or malformed region is passed
- **THEN** Python raises and C returns `CLAY_ERROR_INVALID_ARGUMENT`

<!-- Binding parity is NOT restated here. `mesh-fixed-topology-brushes` added
     "Binding parity holds" to this capability's living spec, and a delta that
     re-adds an existing requirement is refused on archive — correctly: the
     requirement is standing, and every change owes it rather than each one
     declaring its own copy. -->

### Requirement: A layer's radial symmetry is reachable from Python
`pyclay` SHALL expose a layer's radial symmetry with the same reach as the C ABI, following the shape of `Layer.mirror`: an axis named by string, a count, and a seam blend. Setting a count below 2 SHALL clear the mode.

#### Scenario: Python and the C ABI agree
- **WHEN** the binding-parity check runs
- **THEN** the radial entry point has both a Python and a C counterpart, and neither is exempted

### Requirement: Document memory is reportable from Python
The Python bindings SHALL expose the document memory report and the per-layer report, with the same breakdown the C ABI reports.

The report SHALL be returned as a structure whose fields are readable by name rather than as a bare total, since the breakdown is what makes the figure actionable.

#### Scenario: A Python host reads the breakdown
- **WHEN** a document holding voxel content is asked for its memory
- **THEN** the per-subsystem figures are readable by name and sum to the total

#### Scenario: A Python host attributes a document to a layer
- **WHEN** a layer of a document is asked for its memory
- **THEN** the same fields are readable and the voxel content figure reflects that layer alone

### Requirement: Adaptive sculpting is reachable from Python
`pyclay` SHALL expose the adaptive surface, its sculptor, its topology settings and its conversion to and from a mesh, so the binding-parity gate passes rather than recording an exemption.

Buffers SHALL be numpy-native, and the conversion to a mesh SHALL return the same arrays the existing mesh readback returns.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every adaptive-surface capability reachable from the C ABI is reachable from `pyclay`

### Requirement: The hierarchy is reachable from Python
`pyclay` SHALL expose level creation and removal, the sculpt and display levels, sculpting at a level, export at a level, and the memory accounting, so the binding-parity gate passes rather than recording an exemption.

Arrays SHALL be numpy-native, and an exported level SHALL return the same arrays the existing mesh readback returns.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every multiresolution capability reachable from the C ABI is reachable from `pyclay`

### Requirement: The brush model is reachable from Python
`pyclay` SHALL expose the brush model and preset surface the C ABI exposes, so that the binding-parity gate passes rather than recording an exemption.

Arrays SHALL be numpy-native where a sequence is natural, and a preset SHALL serialize to and from `bytes`.

#### Scenario: Parity holds
- **WHEN** `tools/check_binding_parity.py` runs after this change
- **THEN** every brush-model capability reachable from the C ABI is reachable from `pyclay`

### Requirement: Voxel remesh from Python
`pyclay` SHALL expose the global voxel remesh and its preflight estimate, taking resolution as either a longest-axis integer or a world voxel size, and returning the result mesh together with the report's fields as named values rather than as a positional tuple.

A refused remesh SHALL raise with a message naming which contract refused it — resolution, budget, open surface, validation or cancellation — rather than returning an empty mesh a caller has to diagnose.

The binding SHALL be covered by the repository's binding-parity check, so a parameter added to the C++ or C surface and not to Python is caught by the same gate every other surface is.

#### Scenario: A remesh runs from Python
- **WHEN** a mesh is remeshed from Python at a longest-axis resolution
- **THEN** a new mesh is returned whose triangle count differs from the source's, together with the resolved voxel size and the validation counts

#### Scenario: The estimate is reachable before the run
- **WHEN** the estimate is called from Python
- **THEN** it returns the resolved voxel size, the estimated memory and triangle range, and the open-boundary and component counts, without performing the remesh

#### Scenario: A refusal is an exception naming its cause
- **WHEN** a Python caller requests a remesh at an invalid resolution and one over a supplied memory budget
- **THEN** each raises, and the two messages name different causes

### Requirement: A document rebuilds one of its mesh layers
`pyclay` SHALL expose, on the document, a rebuild of one mesh layer that returns the report as named values; the layer's geometry revision; and a revision-checked replacement for a caller that ran the pure rebuild itself.

A stale commit and a refused rebuild SHALL raise with messages naming which contract refused them, rather than returning silently.

A sculpting session held over a layer that has since been rebuilt SHALL raise on its next operation, including when the replacement had the same vertex and triangle counts.

#### Scenario: The layer rebuild is one undo step from Python
- **WHEN** a mesh layer is rebuilt from Python with undo enabled
- **THEN** the layer holds the new triangles, the returned report carries the stage timings and the surface distance, and one undo restores the previous triangles

#### Scenario: A stale commit raises
- **WHEN** a caller commits a rebuild at a revision the layer has moved past
- **THEN** it raises, and the layer keeps the newer geometry

### Requirement: Welding is reachable from Python
`pyclay` SHALL expose the weld on a mesh, returning what it did as named values, and SHALL raise on a negative tolerance rather than clamping it.

#### Scenario: A marched mesh converts after welding
- **WHEN** a mesh produced by a voxel remesh is welded and then converted to an adaptive surface
- **THEN** the conversion succeeds and the surface's face count matches the welded mesh's triangle count

#### Scenario: The report says what happened
- **WHEN** a mesh with nothing to merge is welded
- **THEN** the returned values report zero merged and zero collapsed

### Requirement: A magnify on the assembled surface is reachable from pyclay
pyclay SHALL expose `Layer.magnify_surface` and `Layer.magnify_surface_preview` alongside `Layer.move_surface`, returning the nodes that took a warp, making the whole gesture one undo step, and refusing a zero strength or a non-positive radius with a ValueError.

#### Scenario: A blended form swells as one surface
- **WHEN** Layer.magnify_surface is called over a form built from two blended items
- **THEN** both items are named in the result and the surface swells symmetrically about the centre

#### Scenario: The preview is pure
- **WHEN** Layer.magnify_surface_preview is called
- **THEN** it names the nodes the apply would touch and the field is bit-for-bit unchanged

### Requirement: The field report names its mechanism from Python
`Layer.field_report` SHALL report `steepest_deformer_chain`, `drawable_count` and `degradation` — the last as one of "none", "volumes", "deformers", "both" — alongside what it already returns, and `advises_consolidation` SHALL follow the mechanism.

#### Scenario: A brush chain on one item is not advised
- **WHEN** a layer of one item carrying a deep grab is reported below the caller's threshold
- **THEN** `degradation` is "deformers" and `advises_consolidation` is False

#### Scenario: The same chain over an edit list is advised
- **WHEN** the same grab is applied to a layer of twenty items
- **THEN** `degradation` is "both" and `advises_consolidation` is True

### Requirement: A voxel drag is a context manager in pyclay
`VoxelGrid.grab` SHALL return a gesture usable as a context manager: leaving the block commits it and an exception cancels it. `update` SHALL take the total displacement from the anchor, and `written_box` SHALL report the footprint the gesture writes.

#### Scenario: However the drag is delivered, it lands the same
- **WHEN** the same total drag is delivered as one, two, four and eight updates
- **THEN** the occupied cells are identical, and differ from the untouched grid

#### Scenario: An exception inside the block cancels
- **WHEN** the block raises after an update
- **THEN** the material is what it was when the gesture began

### Requirement: A region of a layer can be merged from pyclay
`Layer.consolidate_region` SHALL bake the influence closure of a region and install it in the absorbed items' place, reporting the bake's cost alongside how many roots it absorbed and whether the closure took the whole layer. `Layer.plan_region_merge` SHALL report the same without baking.

#### Scenario: The rest stays parametric
- **WHEN** a region over one of four separated items is merged
- **THEN** one is absorbed, the item count is unchanged, and the surface outside the closure has not moved

#### Scenario: Repeated merges do not stack
- **WHEN** the same patch is merged six times
- **THEN** the item count is what it was after the first
