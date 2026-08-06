# c-abi Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Flat versioned C API
`bindings/c/clay.h` SHALL expose documents, layers, edit commands, evaluation, brick access, meshing, picking, and file I/O through a flat C API: opaque handles, integer error codes, caller-owned buffers, no C++ types and no exceptions crossing the boundary. The header SHALL carry an ABI version triple queryable at runtime (`clay_version()`), and the ABI SHALL follow SemVer: from 1.0 breaking changes only on major, and below 1.0 under SemVer's 0.x rule a minor bump MAY break the ABI. A break below 1.0 SHALL be stated in the header, the proposal and the release notes, and SHALL be detectable rather than silent: a call made in the older layout SHALL be rejected with an error code, never read as if it were the newer one.

The C API SHALL reach every authoring and query capability the Python bindings reach, so a Swift consumer is not restricted to a subset of what `pyclay` can drive. Where a Python construct has no direct C equivalent — chained modifiers, variable-length payloads — the C API SHALL provide an equivalent builder rather than omitting the capability.

#### Scenario: Pure C consumer
- **WHEN** a C11 test program includes only `clay.h` and links the library
- **THEN** it can create a document, add a sphere edit, evaluate points, mesh, and export OBJ — with every failure path returning an error code

#### Scenario: Version check
- **WHEN** a consumer compiled against header version X.Y links a library reporting major ≠ X, or reporting major 0 with a different minor
- **THEN** the documented init-time version check fails explicitly instead of undefined behavior

### Requirement: Error and memory discipline
Every fallible function SHALL return a `clay_result` code with a queryable thread-local detail message; all buffers crossing the ABI SHALL be caller-allocated (size-query pattern) or returned with an explicit `clay_free_*` owner function. Internal C++ errors SHALL be `std::expected`-style — no exception may propagate to the C boundary.

#### Scenario: Size-query pattern
- **WHEN** a caller requests mesh data with a null buffer
- **THEN** the call returns required sizes; a second call with adequate buffers fills them; an undersized buffer yields a too-small error, not a write overflow

### Requirement: Swift consumption via SwiftPM
The repository SHALL provide a SwiftPM wrapper target (prebuilt xcframework or source) so the ClaySpace Xcode project consumes claycore as a package, exposing `clay.h` through a module map. The wrapper SHALL contain no logic beyond packaging.

#### Scenario: App links the package
- **WHEN** the ClaySpace app adds the claycore SwiftPM package and calls `clay_version()` from Swift
- **THEN** it builds for iOS device and simulator, with the Metal backend registered on device

### Requirement: FFI-general design
The C API SHALL avoid patterns hostile to non-Swift FFI (C#, Rust): no variadic functions, no bitfields in public structs, fixed-size integer types, explicit struct layout/packing, and UTF-8 for all strings.

#### Scenario: Bindgen clean
- **WHEN** `clay.h` is processed by a standard bindings generator (e.g. rust-bindgen in CI)
- **THEN** it generates without manual patching or layout warnings

### Requirement: Item builder for composed edits
The API SHALL provide an opaque item builder so an edit can be composed from a primitive plus modifiers before being added to a layer. The builder SHALL accept transform, combine op, blend kind and radius, rounding, colour, mirror flag, a deformer chain, repetition, a profile (including polygon vertices), stroke points, and transition parameters. Modifier order SHALL be preserved, matching the Python bindings, because deformers do not commute.

The existing flat `clay_item_desc` entry point SHALL keep working, defined as sugar over the builder for edits that need no variable-length payload.

#### Scenario: Composed edit from C
- **WHEN** a C consumer builds a primitive, applies a twist then a bend, sets a radial repetition, and adds it to a layer
- **THEN** the resulting document evaluates identically to the same edit authored through `pyclay`

#### Scenario: Deformer order is preserved
- **WHEN** two items are built with twist-then-bend and bend-then-twist
- **THEN** their fields differ, in the same way they differ through the Python bindings

#### Scenario: Variable-length payloads
- **WHEN** a consumer builds a stroke of N points or a polygon profile of N vertices
- **THEN** the payload is carried by the builder without any fixed-size struct limit

#### Scenario: Existing flat path still works
- **WHEN** source written against the previous header is recompiled and calls `clay_add_item` with a `clay_item_desc` declaring its `struct_size`
- **THEN** every field means what it meant before and the edit lands on the same node

#### Scenario: A binary from the previous ABI is rejected, not misread
- **WHEN** a binary compiled against ABI 0.1.0 — whose `clay_item_desc` and `clay_mesh_params` predate the `struct_size` prefix — calls `clay_add_item` or `clay_document_mesh`
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT`, the document is unchanged, and the library reads nothing beyond the shorter object it was handed

### Requirement: Versioned descriptor structs
Every descriptor struct crossing the ABI SHALL carry a leading `uint32_t struct_size` set by the caller. The library SHALL read only the prefix the caller declares, so fields may be appended without a major version bump. Setting it SHALL be mandatory: a declared size below the struct's original layout — zero included — SHALL be rejected with `CLAY_ERROR_INVALID_ARGUMENT`, and so SHALL a value too large to be any descriptor, because both are what a caller that predates the convention leaves in that word. A declared size larger than the library knows SHALL be clamped, so an unknown tail is ignored rather than misread, and the library SHALL never copy more than the caller declared. The ABI hygiene gate SHALL fail if a public descriptor struct lacks the field.

#### Scenario: Older caller against newer library
- **WHEN** a caller sets `struct_size` to the size it was compiled against and the library has since appended fields
- **THEN** the call succeeds, only the declared prefix is read, and the appended fields take their documented defaults

#### Scenario: Gate rejects an unversioned struct
- **WHEN** a public descriptor struct is added without `struct_size`
- **THEN** the C ABI hygiene check fails naming the struct

### Requirement: Complete primitive, op and blend enumerations
`clay_prim` SHALL cover every primitive the scene model supports, including the lifts, and its values SHALL equal the corresponding tape opcodes so no translation table exists to drift. `clay_op` SHALL cover the boolean ops, paint, the eight extended combine modes and both transition morphs. `clay_blend` SHALL cover every blend profile.

#### Scenario: Enumerations stay in step with the engine
- **WHEN** a primitive, op or blend is added to the scene model without a matching C enumerator
- **THEN** a compile-time assertion fails rather than the C API silently lagging

Where an engine primitive constructor conditions its arguments, the C entry points SHALL condition them identically, so the same authored intent gives the same field through either binding: a plane's normal SHALL be normalized (a zero-length one rejected), and the sine/cosine pair the angle primitives take SHALL be rejected when it is not a unit pair.

#### Scenario: Every primitive is reachable
- **WHEN** a C consumer adds one edit of every `clay_prim` value to a document
- **THEN** each evaluates to the same field as the same primitive authored on the scene model, and a document of the bounded ones meshes

### Requirement: Voxel grids across the ABI
The API SHALL expose voxel grids through an opaque handle: palette management, single-cell and batch edits, cube and sphere brushes with falloff and strength, the sculpting verbs (smooth, inflate, flatten, pinch), box and line fills, mirrored edits, flood select, occupancy and bounds queries, greedy meshing, SDF rasterization, and step-field sampling.

Ownership SHALL be explicit: a grid created standalone is owned by the caller and destroyed with an explicit destroy call, while a grid obtained as a document layer is borrowed, remains owned by the document, and SHALL NOT be destroyed by the caller. Destroying a borrowed handle SHALL return an error rather than corrupting the document.

#### Scenario: Voxel sculpting from C
- **WHEN** a C consumer creates a grid, adds palette entries, stamps a sphere brush, runs a sculpting verb, and greedy-meshes the result
- **THEN** the mesh matches the same sequence performed through `pyclay`

#### Scenario: Borrowed layer handle is protected
- **WHEN** a consumer calls destroy on a handle obtained from a document voxel layer
- **THEN** the call returns an invalid-argument error and the document is unaffected

#### Scenario: Batch edits use the size-query pattern
- **WHEN** flood select is called with a null buffer
- **THEN** it reports the required cell count, and a second call with an adequate buffer fills it

#### Scenario: Falloff brushes are reproducible across the boundary
- **WHEN** a C consumer stamps a falloff brush with a given seed
- **THEN** the affected cells are identical to the same stamp through `pyclay`

#### Scenario: Brush strength is passed through, never reinterpreted
- **WHEN** a C consumer stamps a brush at any strength the boundary accepts
- **THEN** the coverage reaches the engine untouched, so the affected cells are identical to the same stamp through `pyclay`
- **AND WHEN** the strength is not greater than zero, which covers no cell at all
- **THEN** the call returns an invalid-argument error and the grid is unchanged, rather than the value being read as full coverage

#### Scenario: A region with a non-finite bound is refused
- **WHEN** rasterization is asked for a region whose bounds contain a NaN or an infinity
- **THEN** the call returns an invalid-argument error and the grid is unchanged

### Requirement: Picking and evaluation parity
The API SHALL expose gradients, field colours, batch raycast, safe step scale, surface snapping, layer bounds, selection bounds, voxel cell/face picking, build-plane picking, and raycast that attributes the hit to a layer and node. Mesh generation SHALL allow selecting the mesher, with the experimental one reachable only behind its explicit flag.

#### Scenario: Zoom to selection from C
- **WHEN** a consumer requests the bounds of a selection of nodes
- **THEN** it receives the same AABB the Python bindings report for that selection

#### Scenario: Attributed raycast
- **WHEN** a raycast hits an item in a multi-layer document
- **THEN** the call reports the layer and node identifiers alongside the hit position and normal

#### Scenario: Experimental mesher stays gated
- **WHEN** a consumer selects dual contouring without setting the experimental flag
- **THEN** the call returns an error rather than silently meshing

### Requirement: Batch counts are bounded
Every entry point taking an element count SHALL reject a count above a documented ceiling with an invalid-argument error rather than sizing a buffer from it. A count is the one argument the boundary cannot check against the caller's memory, and the library builds without exceptions, so an allocation failure would terminate the host process instead of returning an error code.

#### Scenario: A count that is not a count
- **WHEN** a consumer passes a byte length, or a negative signed value widened to `size_t`, where an element count belongs
- **THEN** the call returns an invalid-argument error and the process survives

### Requirement: Binding parity gate
The repository SHALL gate that the C ABI reaches the capability surface the Python bindings reach. A capability exposed by `pyclay` without a corresponding C entry point SHALL fail the gate unless it carries a recorded exemption naming why.

#### Scenario: A new binding without a C entry point
- **WHEN** a capability is added to `pyclay` and no C entry point is added
- **THEN** the parity gate fails in CI naming the missing capability

#### Scenario: Recorded exemption
- **WHEN** a capability is deliberately Python-only and carries an exemption
- **THEN** the gate passes and the exemption is visible in the gate's output

#### Scenario: A declaration without a definition
- **WHEN** `clay.h` declares an entry point the shared library does not export
- **THEN** the ABI gate fails naming it, rather than leaving the link error for every generated binding to discover

### Requirement: Node and layer editing across the ABI
The C API SHALL expose the same editing surface as the Python bindings: node transform, primitive, colour, op/blend/rounding, move and remove; layer add, remove, reorder, visibility and transform; stroke append and trim. Edits SHALL be addressed by node or layer id and SHALL return `CLAY_ERROR_NOT_FOUND` for an id the document does not hold, leaving the document unchanged.

#### Scenario: Editing from Swift
- **WHEN** a C consumer adds an item, keeps its node id, and later sets a new transform and a new blend on it
- **THEN** the document evaluates identically to the same edits made through `pyclay`

#### Scenario: Unknown id is refused
- **WHEN** an edit names a node or layer id the document does not hold
- **THEN** the call returns `CLAY_ERROR_NOT_FOUND` and the document is bit-identical to before

#### Scenario: Editing a primitive keeps the modifiers
- **WHEN** a node's primitive is replaced on an item carrying a deformer chain
- **THEN** the deformer chain, repetition and profile survive the edit

### Requirement: Undo across the ABI
The C API SHALL expose the same opt-in undo stack as the Python bindings: enable, undo, redo, depths and grouping. Calling undo with an empty stack SHALL report that rather than failing, so a UI can drive it without tracking state itself.

#### Scenario: Undo from Swift
- **WHEN** a C consumer enables undo, edits, and undoes
- **THEN** the document serializes bit-identically to its pre-edit state, matching what `pyclay` produces for the same sequence

#### Scenario: Empty stack is not an error
- **WHEN** undo is called on a document with nothing to undo
- **THEN** the call reports that nothing was undone without returning a failure code

### Requirement: wrap_around across the ABI
`clay_deform` SHALL include a wrap enumerator taking `x0` and `x1`, so a C consumer composes the same wrapped item the Python bindings do.

#### Scenario: Wrapping from C
- **WHEN** a C consumer appends a wrap deformer to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

### Requirement: elongate across the ABI
`clay_deform` SHALL include an elongate enumerator taking the three half-extents, so a C consumer composes the same stretched item the Python bindings do.

#### Scenario: Stretching from C
- **WHEN** a C consumer appends an elongate deformer to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

### Requirement: bend_linear and bend_radial across the ABI
`clay_deform` SHALL include enumerators for both ramped bends, taking nine and three parameters respectively, so a C consumer composes the same item the Python bindings do.

#### Scenario: Ramping from C
- **WHEN** a C consumer appends either bend to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

#### Scenario: A degenerate span is refused
- **WHEN** either bend is given a zero-length span
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT`

### Requirement: elongate_axis across the ABI
`clay_deform` SHALL include a per-axis elongate enumerator taking the three half-extents.

#### Scenario: Stretching from C
- **WHEN** a C consumer appends a per-axis elongate to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

### Requirement: grab and pose across the ABI
`clay_deform` SHALL include grab and pose enumerators, and the voxel surface SHALL gain a grab entry point, so a Swift consumer drives the same tools the Python bindings do.

#### Scenario: Grabbing from C
- **WHEN** a C consumer appends a grab deformer to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

#### Scenario: A non-positive radius is refused
- **WHEN** either entry point is given a radius of zero
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT`

### Requirement: pose_line across the ABI
`clay_deform` SHALL include a line-gradient pose enumerator taking anchor, end, axis and angle.

#### Scenario: Posing from C
- **WHEN** a C consumer appends a line pose to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

#### Scenario: A degenerate segment is refused
- **WHEN** the anchor and end coincide
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT`

### Requirement: Masks across the ABI
The C API SHALL expose mask creation on a layer, painting, the region operations, batch sampling through the size-query pattern, and masked voxel edits.

#### Scenario: Freezing from C
- **WHEN** a C consumer masks a region and stamps a brush across it
- **THEN** the masked cells are unchanged, matching what `pyclay` produces for the same sequence

### Requirement: Strokes across the ABI
The C API SHALL expose a versioned preset descriptor, stroke resolution through the size-query pattern, and stroke application to a voxel grid or an SDF layer with an optional mask.

#### Scenario: A stroke resolves identically through both bindings
- **WHEN** a C consumer resolves a stroke with a given preset and seed
- **THEN** the stamps match what `pyclay` produces for the same input

### Requirement: The flags across the ABI
The C API SHALL expose reading and setting a layer's ghost and lock flags, and SHALL return a typed error for an edit naming a protected layer.

#### Scenario: Editing a protected layer is a typed error
- **WHEN** a C consumer adds an item to a ghosted layer
- **THEN** the call returns an error naming the protection and the document is unchanged

### Requirement: Curves across the ABI
The C API SHALL accept control points with per-point radius, type and handles, a closed flag and a tolerance, and SHALL expose replacing a placed item's points.

#### Scenario: A curve means the same through both bindings
- **WHEN** a C consumer builds a curve with given control points, types and tolerance
- **THEN** the field matches what `pyclay` produces for the same curve

### Requirement: Cuts across the ABI
The C API SHALL expose a versioned cut descriptor carrying the frame, the shape and the extent, resolving it into an item handle the caller places like any other.

#### Scenario: A cut means the same through both bindings
- **WHEN** a C consumer resolves a cut with a given frame and shape
- **THEN** the field matches what `pyclay` produces for the same cut

### Requirement: The new verbs across the ABI
The C API SHALL expose the four verbs, with the alpha as a packed float array plus its width and height.

#### Scenario: A verb means the same through both bindings
- **WHEN** a C consumer runs each verb with given parameters
- **THEN** the grid matches what `pyclay` produces for the same call

### Requirement: Repair across the ABI
The C API SHALL expose the report through a versioned descriptor and both repairs, each taking an optional mask.

#### Scenario: A repair means the same through both bindings
- **WHEN** a C consumer repairs a grid
- **THEN** the result matches what `pyclay` produces for the same call

### Requirement: Lofts across the ABI
The C API SHALL expose adding profiles to a loft item, including polygon profiles with their vertices.

#### Scenario: A loft means the same through both bindings
- **WHEN** a C consumer builds a loft of the same profiles
- **THEN** the field matches what `pyclay` produces

### Requirement: Sweeps across the ABI
The C API SHALL expose setting a swept item's guide points, reusing the curve point encoding, alongside the loft profile calls.

#### Scenario: A sweep means the same through both bindings
- **WHEN** a C consumer sweeps the same profiles along the same guide
- **THEN** the field matches what `pyclay` produces

### Requirement: The C ABI can build a volume from a mesh
`add-sampled-fields` declared `CLAY_PRIM_VOLUME` but refused to construct one, because nothing in the C ABI could supply the samples. Mesh import supplies them, so the C ABI SHALL provide a producer that samples a mesh into an item carrying a volume, and construction SHALL no longer be refused.

#### Scenario: A mesh becomes an item through the C ABI
- **WHEN** a C caller loads a mesh and samples it into an item
- **THEN** the item carries a volume and evaluates as the mesh's shape

#### Scenario: A volume item survives the round trip
- **WHEN** a document containing a C-built volume item is saved and reloaded
- **THEN** the field is unchanged

#### Scenario: Degenerate input is refused where the item is built
- **WHEN** a C caller samples a mesh with no triangles, or passes a non-positive cell size
- **THEN** the call fails with an invalid-argument error

