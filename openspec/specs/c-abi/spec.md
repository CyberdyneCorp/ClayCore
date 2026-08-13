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
The C API SHALL accept control points with per-point radius, type and handles, a closed flag and a tolerance, and SHALL expose replacing a placed item's points and READING them back. The readback SHALL take the same arguments as the replace, with the count as an in/out pointer, and SHALL follow the size-query convention: a null point buffer answers with the count, an undersized buffer yields a too-small error carrying the needed count and writes nothing, and the optional parallel arrays are each independently omittable. The points SHALL come back as authored rather than tessellated, so that feeding a readback into the replace leaves the document unchanged. A guide belonging to a swept item SHALL be readable and replaceable through the same calls, since a guide is an ordinary curve, and the replace SHALL enforce on it the same rules placing a swept item enforces: a guide SHALL NOT be closed, and SHALL NOT be left with fewer than two points. Reading SHALL NOT be refused on a protected or hidden layer, because protection refuses edits.

#### Scenario: A curve means the same through both bindings
- **WHEN** a C consumer builds a curve with given control points, types and tolerance
- **THEN** the field matches what `pyclay` produces for the same curve

#### Scenario: A reloaded document's curve round trips
- **WHEN** a host reads a placed curve's points back and feeds them straight into the replace
- **THEN** every point, type and handle comes back unchanged and the field is what it was

#### Scenario: A profiled tube's guide is reachable
- **WHEN** a host reads back a node built by the tube call with a profile
- **THEN** it receives the guide's control points, and replacing them reshapes the tube

#### Scenario: A guide keeps the rules its item was placed under
- **WHEN** the replace would close a swept item's guide, or leave it under two points
- **THEN** it is refused with the reason, and the guide is what it was

#### Scenario: A protected layer still answers a read
- **WHEN** a curve on a ghosted or locked layer is read back
- **THEN** the points are returned, while an edit to the same curve is still refused

#### Scenario: A short buffer says what it needed
- **WHEN** a host asks for the points with a buffer smaller than the point count
- **THEN** the call reports too-small, writes nothing, and leaves the needed count in the count argument

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

### Requirement: Masking through the C ABI is a stroke
The ABI SHALL expose the stroke engine's mask consumer beside the ones that write voxels and emit items, so a host paints a mask with the drag it already resolved rather than by looping single stamps and re-deriving spacing itself.

It SHALL also expose the bounded fill and invert, and SHALL accept an optional mask on the relax and flatten parameter blocks — added at the END of those structs, with `struct_size` deciding whether the field is present, so a caller compiled against the older layout keeps working unchanged.

#### Scenario: A host paints a mask along a drag
- **WHEN** a host resolves a stroke and applies it to a mask
- **THEN** the mask is painted along the path and the call reports how many stamps ran

#### Scenario: An older caller is unaffected
- **WHEN** relax or flatten is called with a `struct_size` from before the mask field existed
- **THEN** the call succeeds and behaves exactly as it did, with no mask

#### Scenario: Inverting within a box
- **WHEN** a host inverts a mask within a world-space box
- **THEN** the complement is taken over that box and nothing outside it changes

### Requirement: Mask extrude through the C ABI
The ABI SHALL expose the mask-to-distance conversion and both extrudes — into a volume item and into a voxel grid — with a versioned parameter block carrying the thickness, side, threshold, rim rounding, border smoothing and sampling, as the relax and flatten blocks already do.

A refused extrude SHALL report a typed error rather than handing back an empty item, so a host distinguishes "the mask missed the surface" from "the surface is small".

The result SHALL be a handle the CALLER owns, on the same lifetime rule the rest of the voxel and volume surface follows.

#### Scenario: A host extracts a plate
- **WHEN** a host paints a mask on a layer and extrudes it
- **THEN** it receives a new item or grid holding the plate, and the source layer is untouched

#### Scenario: A refusal is typed
- **WHEN** an extrude is asked for with an empty mask
- **THEN** the call returns a typed error and produces no handle

### Requirement: The C ABI can bake a document into a volume
The C ABI SHALL sample a document's own field into an item carrying a volume, with an optional explicit region.

Without it the volume operations are unreachable for anything an app made itself: relax and flatten both act on a volume, and a mesh was the only source, so an app could smooth an imported scan but not its own sculpt, and could not collapse a long edit list into a single item.

A cell size SHALL be required rather than derived. A mesh's bounds imply one; a document has no intrinsic scale, and after a bake the resolution IS the shape — so guessing would silently pick a resolution the caller never chose.

A region containing no surface SHALL be refused rather than returning an item that contributes nothing. Note that such a volume is not "empty" in the structural sense: it has a full brick index and stores no samples, and it evaluates perfectly well as a lower bound on the distance to anything.

#### Scenario: A document becomes a volume
- **WHEN** a C caller bakes a document containing a shape
- **THEN** the resulting item's field is that shape

#### Scenario: The baked volume can then be relaxed and flattened
- **WHEN** a C caller bakes a document and applies relax or flatten to the result
- **THEN** both succeed, which they could not before for anything but an imported mesh

#### Scenario: A missing cell size is refused
- **WHEN** a C caller bakes a document without giving a cell size
- **THEN** the call fails rather than choosing a resolution on the caller's behalf

#### Scenario: A region with no surface is refused
- **WHEN** a C caller bakes a region of a document that contains no surface
- **THEN** the call fails rather than returning an item that would contribute nothing

### Requirement: The import budget is settable across the ABI
An importer's guardrail SHALL be settable by a C caller, not only enforced against the library's defaults, because the point of a budget is to be tightened for input the caller does not trust. It SHALL be checked against a file's DECLARED counts before anything is allocated.

A null budget SHALL mean the library's defaults, and a zeroed field SHALL mean the default for that field rather than "allow nothing", since a zeroed descriptor would otherwise refuse every file.

#### Scenario: A tight budget is enforced
- **WHEN** a mesh is loaded with a budget smaller than the file declares
- **THEN** the load fails with a budget error and nothing is allocated

#### Scenario: A null budget uses the defaults
- **WHEN** a mesh is loaded with no budget given
- **THEN** it loads under the library's defaults

#### Scenario: A zeroed field means the default
- **WHEN** a mesh is loaded with a budget whose fields are zero
- **THEN** it loads rather than being refused

### Requirement: A file extension is matched case-insensitively
An importer SHALL match a file's extension without regard to case, because a file named `MODEL.OBJ` is an OBJ file. The Python loader has always done so; the C one did not, and refused such a file as an unknown format.

#### Scenario: An upper-case extension loads
- **WHEN** a mesh file whose extension is upper-case is loaded
- **THEN** it loads, rather than being reported as an unknown format

### Requirement: Relief is reachable as an ordinary op
Relief SHALL be a combine op on the existing vocabulary, so that an app places a relief item exactly as it places any other item and gets undo, coalescing, serialization, picking and masking without any of them learning about it.

It SHALL follow the existing parameter convention rather than introducing one: the blend_k field carries the mode's depth, and the item's rounding carries the falloff width, as the groove and tongue modes already do.

#### Scenario: A relief item is placed like any other
- **WHEN** a C caller adds an item with the relief op and an amplitude
- **THEN** the document's field shows the surface displaced over that item's region

#### Scenario: It survives a save
- **WHEN** a document containing a relief item is saved and reloaded
- **THEN** the field is unchanged

### Requirement: A host can move a surface through the C ABI
The ABI SHALL expose the Move brush as one call taking a world centre, radius and displacement and applying the resolved warps to a layer, and SHALL report how many items were affected so a host can tell "the drag missed everything" from "the drag did nothing visible".

It SHALL also expose replacing a node's deformer chain, since that is the mutation the Move is built on and a host has no other way to reach it: `clay_item_add_deformer` acts on a builder, not on a node already in a document.

With undo enabled the whole move SHALL be ONE step however many items it touched, using the existing grouping — a drag is one gesture, and undoing it item by item would be an artifact of the implementation showing through.

#### Scenario: A host drags a blended form
- **WHEN** a host resolves a drag over a layer built from several blended items
- **THEN** the surface moves as one, and the call reports how many items took a warp

#### Scenario: One gesture, one undo step
- **WHEN** a move touching several items is undone
- **THEN** the whole drag reverts in a single step

#### Scenario: A drag that reaches nothing
- **WHEN** a drag is placed far from every item
- **THEN** the call succeeds, reports zero items affected, and changes nothing

### Requirement: Previewing a move across the C ABI
The ABI SHALL expose previewing a drag, following the size-query convention every other list-returning entry point here uses. It SHALL validate its arguments exactly as applying the move does, so a preview cannot succeed where the move would be refused.

#### Scenario: A host previews before committing
- **WHEN** a host previews a drag
- **THEN** it receives the nodes the move would warp, and the document is unchanged

#### Scenario: A preview refuses what the move refuses
- **WHEN** a preview is asked with a radius that is not positive, an unknown layer, or a malformed descriptor
- **THEN** it fails with the same code applying the move would give

### Requirement: Flatten mode across the C ABI
The flatten descriptor SHALL carry the mode, appended so that a caller compiled against the previous layout still describes a two-sided flatten and still works.

#### Scenario: An older descriptor still means two-sided
- **WHEN** a caller passes a descriptor sized to the layout that predates the mode
- **THEN** the call succeeds and performs a two-sided flatten

#### Scenario: An unknown mode is refused
- **WHEN** a descriptor names a mode the ABI does not define
- **THEN** it is refused rather than silently treated as two-sided

### Requirement: Flattening a trim curve across the C ABI
The ABI SHALL expose flattening an OPEN control-point curve into a trim outline, beside the closed-lasso flattener it already has, following the same size-query convention. An unknown side, a tolerance that is not positive, or fewer points than describe a stroke SHALL be refused.

#### Scenario: A host flattens a trim
- **WHEN** a host flattens an open curve for a side
- **THEN** it receives an outline whose closing edge runs along the frame bound on that side

#### Scenario: The other side closes the other way
- **WHEN** the same curve is flattened for the opposite side
- **THEN** the closing edge runs along the opposite bound

### Requirement: A topological move across the C ABI
The ABI SHALL expose applying a topological move to an item carrying a volume, with its parameters in a versioned descriptor struct as every other multi-parameter entry point here uses. An item that carries no volume, a radius that is not positive, or an unknown easing curve SHALL be refused rather than ignored.

#### Scenario: A host moves one part and not its neighbour
- **WHEN** a host applies a topological move to a volume holding two parts close in space and joined only through a distant path
- **THEN** only the part connected to the anchor along the material moves

#### Scenario: It refuses what it cannot do
- **WHEN** the call names an item with no volume, or a radius that is not positive
- **THEN** it is refused rather than silently doing nothing

### Requirement: Resolving a tube across the C ABI
The ABI SHALL expose resolving a path into a tube, with its settings in a versioned descriptor struct, returning an ordinary item the caller owns. A parametric profile SHALL select the swept representation and its absence the exact swept-sphere one, as it does elsewhere.

Fewer points than describe a path, a radius positive nowhere, an unknown point type, or a polygon profile SHALL be refused rather than yielding an item that contributes nothing.

#### Scenario: A round tube stays exact across the boundary
- **WHEN** a host resolves a tube with no profile and adds it to a document
- **THEN** the document's safe step scale is 1

#### Scenario: A profiled tube declares its cost
- **WHEN** the same path is resolved with a box profile
- **THEN** the safe step scale is below 1

#### Scenario: Degenerate input is refused
- **WHEN** a tube is asked for from one point, or with every radius zero
- **THEN** no item is returned

### Requirement: Replacing a primitive refuses the kinds that carry out-of-line data
`clay_layer_set_prim` SHALL refuse a stroke, a lift, a loft, a sweep or a volume, with `CLAY_ERROR_INVALID_ARGUMENT`. That entry point replaces a node's primitive alone and has no way to supply the payload those kinds read, so accepting one leaves a node the evaluator cannot evaluate.

This is the refusal `clay_add_item` already makes for the same set through the flat descriptor.

#### Scenario: A node cannot be turned into a loft
- **WHEN** `clay_layer_set_prim` is called with `CLAY_PRIM_LOFT`
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT` and the node is unchanged

#### Scenario: The document still evaluates afterwards
- **WHEN** a refused replacement is followed by evaluating the document
- **THEN** evaluation returns the original primitive's field

#### Scenario: An ordinary replacement still works
- **WHEN** `clay_layer_set_prim` is called with a primitive whose parameters fit the params block
- **THEN** it succeeds as before

### Requirement: Every out-of-line count is bounded
A function taking a caller-supplied count and a pointer SHALL check both against the batch ceiling before reserving for them, so that a bogus count is refused rather than reaching the allocator. An allocation failure cannot be reported across this boundary: the library builds without exceptions, so it would end the host process.

#### Scenario: A tube with an impossible point count
- **WHEN** `clay_tube_create` is given a count above the batch limit
- **THEN** it returns null with `CLAY_ERROR_INVALID_ARGUMENT` and the host continues

### Requirement: A requested meshing resolution is priced before it is allocated
`clay_document_mesh` SHALL reject a voxel size that is not finite and positive, and SHALL reject a resolution whose implied dense sample grid exceeds the batch ceiling, before any meshing begins.

#### Scenario: An over-fine resolution is refused
- **WHEN** `clay_document_mesh` is asked for a voxel size that implies more than the ceiling of grid samples over the scene bounds
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT` rather than attempting the allocation

#### Scenario: A sane resolution still meshes
- **WHEN** `clay_document_mesh` is asked for an ordinary resolution
- **THEN** it meshes as before

#### Scenario: The documented resolution is not refused
- **WHEN** `clay_document_mesh` is asked for the resolution the library's own documentation advertises
- **THEN** it meshes

The ceiling SHALL be the mesher's own limit rather than the batch limit: the batch limit bounds how many items cross the boundary in one call, which is a different quantity and far below what this call legitimately needs.

### Requirement: Mirroring a layer is an ordinary layer edit
`clay_set_layer_mirror` SHALL apply through the command vocabulary, so that it refuses a protected layer as every other layer edit does and is recorded on the undo stack.

Setting a mirror SHALL be sufficient for the layer to evaluate mirrored: items participate by default (scene-model spec), and the per-item flag — `clay_item_desc.mirror` and `clay_item_set_mirror` — SHALL read one rule at both entry points: negative excludes the item, 0 and 1 follow the layer's mirror. A zeroed descriptor therefore mirrors, which is what makes the layer call sufficient.

#### Scenario: A zeroed descriptor follows the layer mirror
- **WHEN** a mirror is set on a layer and an item is added from a zeroed descriptor
- **THEN** the item and its reflection both evaluate, with no other call required

#### Scenario: A locked layer refuses a mirror
- **WHEN** `clay_set_layer_mirror` names a locked layer
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT` and the layer is unchanged

#### Scenario: A mirror can be undone
- **WHEN** a mirror is set with undo enabled and then undone
- **THEN** the layer returns to its previous mirror state

### Requirement: Moving a layer is one undo step
`clay_document_move_layer` SHALL group the removal and reinsertion it performs, so that a single undo restores the previous order with every layer still present.

#### Scenario: One undo restores the order
- **WHEN** a layer is moved with undo enabled and undone once
- **THEN** the document has the same layers it had before the move, in the same order

### Requirement: A document reuses its compiled tape until it changes
A document SHALL compile its tape once and reuse it for every read until something changes what that tape would contain. Reads are on the interactive path and compiling is proportional to the whole document, which grows with every brush stamp, so recompiling per read makes looking at a sculpt cost more the longer it has been worked on.

The tape picking uses excludes ghosted layers and is therefore a different tape; it SHALL be remembered separately rather than sharing one slot that would be rebuilt alternately by the two.

#### Scenario: Repeated reads of an unchanged document compile once
- **WHEN** the field is read many times without any intervening edit
- **THEN** every read returns what a fresh compile would have returned

#### Scenario: Picking and evaluation do not evict each other
- **WHEN** picking and field evaluation are interleaved on an unchanged document
- **THEN** neither causes the other to recompile

### Requirement: Every mutation is visible to the next read
Any entry point that changes what the compiled tape would contain SHALL invalidate the remembered tape. This includes edits applied through the command vocabulary, undo and redo, and any layer added outside that vocabulary.

Failing to invalidate is silent: the call succeeds, nothing reports an error, and every later read answers with the field as it was before the edit. An entry point that invalidates unnecessarily merely recompiles, which is the behaviour that existed before any of this was remembered — so where it is not obvious, the tape SHALL be invalidated.

#### Scenario: Each mutating entry point is reflected
- **WHEN** the field is read, mutated through any entry point that changes it, and read again
- **THEN** the second read differs from the first

#### Scenario: Undo restores the previous field exactly
- **WHEN** an edit is undone
- **THEN** the field reads exactly as it did before that edit, and redoing restores it again

#### Scenario: Ghosting changes picking and not evaluation
- **WHEN** a layer is ghosted
- **THEN** picking stops reporting it while the evaluated field is unchanged

### Requirement: A document stays readable from several threads at once
Reading a document from more than one thread concurrently SHALL remain safe. It was safe before the tape was remembered, because compiling took the document by const reference and returned a fresh result, and remembering the result SHALL NOT take that away.

A reader SHALL receive a snapshot that stays valid for the duration of its call, so that another thread invalidating and rebuilding cannot pull the tape out from under it.

#### Scenario: Concurrent readers agree
- **WHEN** several threads evaluate and pick against one unchanged document at once
- **THEN** every reader gets the same answer a single-threaded reader would

### Requirement: The brick cache across the C ABI
The ABI SHALL expose the brick cache as an opaque handle the caller creates from a versioned configuration descriptor and destroys, never bound to a document, alongside the three calls that make it usable: dense-grid evaluation with an optional cull region, and the influence bound of a node and of a layer.

There SHALL be exactly one refill path — mark dirty, drain requests, evaluate, submit — with the drain taking a capacity and reporting a count and a remainder rather than accepting a NULL buffer as a size query, and with the outcome of a submission (accepted, stale, over budget) crossing as an out-parameter with a success return, on the same footing as "nothing to undo".

The handle SHALL take no lock and own no thread; the header SHALL state that calls on one handle are the host's to serialize and that the batched evaluation call, which takes no handle, is free-threaded against one const document. A batched const query — the many-ray raycast — MAY fan its rays out across the engine's shared worker pool, provided the call returns only after every worker is done with the cache and each output slot is byte-identical to what the single-ray call reports for the same ray.

A dirty region SHALL be validated in 64-bit before the engine converts it: a non-finite or empty region, a brick coordinate outside `int32`, and a span above the batch ceiling SHALL each be refused with the cache left unchanged. Dirtying everything the cache tracks SHALL be spelled as the absence of a region, never as a region carrying an infinity.

#### Scenario: A host refills from the header alone
- **WHEN** a consumer with only `clay.h` marks a layer's influence bound dirty, drains the requests in fixed-size chunks, evaluates them and submits the values
- **THEN** every brick is accepted, the pending count reaches zero, and the surface bricks read back at a fixed stride in the engine's own fp16 bits

#### Scenario: A count is never inferred
- **WHEN** a value buffer is passed whose length is not exactly the request count times the brick's sample count
- **THEN** the call is refused rather than reading or writing what the caller did not allocate

#### Scenario: A request carries everything its evaluation needs
- **WHEN** a request is drained and evaluated
- **THEN** the lattice AND the band come with it, so the evaluator culls against the brick dilated by the band without consulting the cache, and there is no value a caller can supply wrongly

#### Scenario: The bound to dirty is not the bound to frame on
- **WHEN** a consumer asks for a node's influence bound
- **THEN** it receives a box no tighter than the layer bounds query reports, and an explicit flag for the items whose influence is unbounded

#### Scenario: A batched raycast parallelizes without changing its answers
- **WHEN** a host casts the same rays through the batched brick-cache raycast and one at a time through the single-ray call
- **THEN** every batch slot holds exactly the hit flag, distance, position and normal the single-ray call reports for that ray, and no engine thread touches the cache after the batched call returns

### Requirement: A voxel edit's effect is readable across the ABI
The ABI SHALL expose the grid's change count as a query alongside the occupied count, taking a `uint64_t` out-parameter rather than a `size_t` because the counter is never reset and a 32-bit host would wrap it in a long session. A NULL out-parameter SHALL be tolerated, matching the other grid queries; a NULL grid SHALL be refused with an invalid-argument error.

The header SHALL state why it exists — sub-cell drags and other effect-free edits are legal and common, and neither the result code nor the occupied count can distinguish them — what it counts, that it is monotone and meaningful only as a difference, the pinch/magnify upper bound, and that its unit is cells changed and NOT the stamps-run or items-warped unit of the `out_applied` parameters elsewhere in the header.

This SHALL be a single grid-level query rather than a per-verb sibling: an entry point differing from an existing one only by an extra out-parameter would be two ways to say one thing, and covering the verbs that share the blind spot would take eleven of them plus one per verb added later.

#### Scenario: A drag that reached nothing is distinguishable from one that did
- **WHEN** a host reads the change count, applies a sub-cell grab, reads it again, applies a supra-cell grab and reads it a third time
- **THEN** the first difference is zero, the second is non-zero, and both calls returned success

#### Scenario: The counter agrees with the engine
- **WHEN** the same sequence of edits is applied across the ABI and directly on the engine's grid
- **THEN** the count read across the ABI equals the engine's

#### Scenario: A NULL grid is refused, a NULL out-parameter is not
- **WHEN** the query is called with a NULL grid, and again with a valid grid and a NULL out-parameter
- **THEN** the first is refused with an invalid-argument error and the second succeeds

### Requirement: A valid edit with no effect stays CLAY_OK
An entry point whose call was well-formed and whose effect was nothing SHALL return `CLAY_OK`. No `clay_result` value SHALL be added to mean "valid but had no effect": the enum is ABI-stable, and an existing entry point returning a new non-zero value would make every already-compiled caller treat a success as a failure. An outcome that a host wants to observe SHALL arrive through an out-parameter or a query, on the same footing as a rejected brick submission and as "nothing to undo".

The sculpting-verbs section of the header SHALL state this once for all of them, rather than leaving each verb to imply it: a flatten on an already-flat region, a sub-cell smudge, a dithered stamp that misses every cell and a footprint over empty space are all ordinary successes.

#### Scenario: A sub-cell grab succeeds
- **WHEN** a grab crosses the ABI with a displacement shorter than half a cell on every axis
- **THEN** the call returns `CLAY_OK` and the grid is unchanged

#### Scenario: No new result code appears
- **WHEN** the result enum is compared against the previous ABI minor
- **THEN** it holds the same values, so a caller compiled against the older header still classifies every outcome as it did

### Requirement: Armatures across the ABI
The C API SHALL expose building an armature item from nodes and their parents, and the tree edits, and SHALL be purely additive: no existing signature changes and no struct grows.

Each node SHALL carry a sign, +1 or -1, positive by default, set beside the parents by a signs setter that mirrors the parents setter — one entry per node — and any value other than +1 or -1 SHALL be refused as a typed invalid argument, because the negative-radius convention would legalise input the point setter refuses today. A fifth tree edit SHALL set a placed node's sign, carrying it in the existing radius argument, so no signature changes shape; it SHALL be one undoable command and SHALL be refused on a protected layer, exactly as the other four edits are. A negative node SHALL NOT be required to be a leaf.

Reading a placed armature's tree back SHALL be split the way the setters are split, because an armature IS a stroke plus a tree: the point readback SHALL accept the armature primitive and serve its nodes as the same x, y, z, radius list it serves for strokes and guides, and a parents readback of its own SHALL serve one parent index per node, a root naming itself, by the same size-query pattern — a null buffer answers with the count, an undersized buffer yields a too-small error carrying the needed count and writes nothing. A signs readback SHALL serve one sign per node by the same pattern, and every readback SHALL agree on the count: both are counted in nodes, a tree stored with fewer parents than nodes reads back padded with roots, and signs stored shorter than nodes read back padded positive — exactly the reading compilation makes, so what comes back is the tree the document evaluates. The parent indices SHALL be the indices the tree edits take, and a node that is not an armature SHALL be refused the parents and signs readbacks with a typed invalid-argument error. Reading SHALL NOT be refused on a protected or hidden layer, because protection refuses edits.

Replacing a placed armature's points through the curve replace SHALL remain refused: points replaced alone would desynchronise from the parents, and the tree edits own that half.

#### Scenario: An armature round trips through a host
- **WHEN** a host reads a placed armature's nodes and parents, moves one node, and writes the tree back
- **THEN** the document reflects the move, with the subtree carried

#### Scenario: A malformed tree is refused
- **WHEN** an armature is built with a parent index outside the node range, or with a cycle
- **THEN** it is refused with the reason, and the document is unchanged

#### Scenario: A reloaded branching rig is re-posable
- **WHEN** a host saves a document holding an armature whose tree branches, reloads it, reads both halves back, and moves a node through the tree edits using the read-back indices
- **THEN** the nodes and parents match what was authored, including the branch, and the move carries exactly the subtree the read-back tree names

#### Scenario: The parents readback keeps the refusals typed
- **WHEN** the parents of a stroke, a group or a missing node are asked for, or a buffer smaller than the node count is passed
- **THEN** the stroke and the group are refused as invalid arguments, the missing node as not found, and the short buffer with the too-small error carrying the needed count and nothing written

#### Scenario: A negative node survives the round trip
- **WHEN** a host sets a sign array with one negative node, saves, reloads, and reads the signs back
- **THEN** the signs match what was authored, and flipping the node positive through the sign edit restores the all-positive field

#### Scenario: The signs surface keeps the refusals typed
- **WHEN** a sign of 0 or ±2 is passed to the setter or the sign edit, the signs of a stroke are asked for, or a short buffer is passed to the signs readback
- **THEN** each is refused with its typed error — invalid argument for the values and the stroke, too-small carrying the needed count for the buffer — and nothing is written

#### Scenario: A negative node carries children
- **WHEN** a node with descendants is set negative
- **THEN** the edit succeeds, the descendants keep their own signs, and moving the negative node still carries its subtree

### Requirement: A host can display a voxel sculpt incrementally
The ABI SHALL expose the grid's dirty-chunk drain and its regional mesh, so that a host can display a sculpt by re-meshing what changed rather than the whole grid. The two calls SHALL follow the brick cache's refill vocabulary rather than inventing a parallel one: drain the keys, mesh the keys, patch the ranges.

The drain SHALL take a capacity and report a count and a remainder, exactly as the brick-cache drain does, and SHALL NOT accept a NULL buffer as a size query — the two shapes are already distinguished in this header and mixing them is what makes a retry loop ambiguous. A host SHALL be able to call it in a loop until the remainder is zero, or to stop early and leave the rest queued for the next frame. Keys SHALL cross as packed `int32` triples, the same spelling `clay_voxel_flood_select` and `clay_brick_cache_surface_bricks` use.

The regional mesh SHALL take that key list and SHALL fill, when asked, one range record per key in the order given, carrying the key and its vertex and index ranges — an array ELEMENT with a fixed layout, not a versioned descriptor, for the same reason `clay_brick_mesh_range` is one. The header SHALL state that these ranges PARTITION the mesh with no vertex shared between keys, which is the difference from the brick ranges and the property that lets a host free one key's slice without consulting its neighbours. It SHALL state why: a voxel face belongs to one cell in one chunk, so clamping the merge to a chunk boundary splits quads instead of cracking the surface, and there are no straddlers to attribute.

Asking for ranges without a key list SHALL be refused, as the brick mesh refuses it and for the same reason: with no key list there is no count the caller could have sized the buffer from.

A key naming a chunk the grid does not hold SHALL contribute an empty range and SHALL NOT be an error, because a drained set routinely names a chunk that has since been emptied and that is precisely the key whose geometry the host must drop.

`clay_voxel_mesh` SHALL keep meaning "mesh the whole grid" and SHALL be unchanged by this addition — same signature, same output byte for byte — so export and a first full display keep the merge that spans chunk boundaries. The addition SHALL be purely additive: no existing signature changes, no struct grows, no enumerator's value changes.

Both calls SHALL act on the grid's ACTIVE level, as every other cell-addressed call in this header does, and the header SHALL say so, because the dirty set is per level.

#### Scenario: A host displays a stroke without re-meshing the model
- **WHEN** a host rasterizes a sculpt, meshes it whole once, then applies a dab, drains the dirty chunks and meshes exactly those keys
- **THEN** it receives one range per key, and patching those ranges over the previous per-chunk geometry yields the same surface a whole-grid re-mesh describes

#### Scenario: The drain is capacity-in, count-out
- **WHEN** the drain is called with a buffer smaller than the number of dirty chunks
- **THEN** it writes what fits, reports how many it wrote and how many remain, and a loop that keeps calling until the remainder is zero receives every key exactly once

#### Scenario: A NULL buffer is not a size query
- **WHEN** the drain is called with a NULL key buffer
- **THEN** it is refused as an invalid argument rather than reporting a required size

#### Scenario: Ranges require a key list
- **WHEN** the regional mesh is asked for ranges with no keys
- **THEN** it is refused rather than inferring a count from the grid's current chunk set

#### Scenario: A stale key is an ordinary key
- **WHEN** a key drained before the chunk was emptied is passed to the regional mesh
- **THEN** the call succeeds and reports a zero-length range for it

#### Scenario: The whole-grid call did not move
- **WHEN** the same grids are meshed by `clay_voxel_mesh` before and after this change
- **THEN** the vertex count, the index count and a hash of every attribute and index buffer are identical

### Requirement: A host can mesh a level of the brick cache
The C API SHALL let a host mesh a LEVEL of the brick cache, not only its full-resolution bricks. The level SHALL be named the way the readback names it — 0 for the evaluated bricks, 1 for their mips — so that a host that can build a mip and read one can also mesh one without reimplementing the mesher over the stored samples.

The addition SHALL be purely additive: the existing meshing entry point SHALL keep its signature and its behaviour, and SHALL be the new one at level 0. Both SHALL share one implementation so the two cannot drift.

At level 1 the key list SHALL name COARSE keys — the 2x2x2 block keys the mip build and the current-level query already take — and a null key list SHALL still mean every brick the level stores. Everything else about the call SHALL be unchanged by the level: the marching, the seam welding, the per-key ranges and the straddler attribution.

A level above 1 SHALL be rejected rather than clamped, for the reason the readback rejects it: there is one mip level, and silently meshing a finer one would put geometry at the wrong size on screen.

#### Scenario: A built mip meshes, and meshes coarser
- **WHEN** a host fills the cache, builds the mips covering its surface bricks, and meshes at level 1
- **THEN** it receives real geometry whose triangle count is a small multiple lower than the same surface at level 0, and whose bounds agree with the level-0 mesh's to within one coarse cell

#### Scenario: The older entry point is unchanged
- **WHEN** the same whole-cache and key-subset requests are made through the existing entry point and through the new one at level 0, with and without a document
- **THEN** the meshes are identical byte for byte — positions, normals, colours, uvs and indices — and so are the per-key ranges

#### Scenario: A level above the one that exists is refused
- **WHEN** a host asks for a level above 1, or a negative one
- **THEN** the call is refused as an invalid argument and no mesh is produced

### Requirement: An unbuilt level is reported, never answered with an empty mesh
An empty mesh already means "this cache holds no surface bricks", which is an ordinary state of a session. A level that has not been built SHALL therefore be a typed not-found rather than an empty mesh, so that a host can tell a missing mip — whose remedy is to build it — from a missing surface, whose remedy is to sculpt.

A named coarse key with no valid mip SHALL be refused, unlike level 0 where a key that stores no lattice is an ordinary uniform brick and contributes nothing: at level 1 there is no uniform mip, so an absent one means "not yet". A whole-level request SHALL be refused when the cache holds surface bricks and not one mip.

A cache holding nothing at all SHALL still mesh EMPTY at every valid level, because there is nothing there to be mistaken about.

#### Scenario: Meshing a level nobody built
- **WHEN** a host fills the cache, builds no mip, and meshes at level 1
- **THEN** the call is refused as not found, and the same request answers once the mip is built

#### Scenario: The refusal is per key
- **WHEN** one coarse key's mip is built and a different coarse key is named
- **THEN** the request naming the built key succeeds and the request naming the unbuilt one is refused as not found

#### Scenario: An empty cache is empty, not unbuilt
- **WHEN** a cache that was never marked or filled is meshed at level 0 and at level 1
- **THEN** both succeed and return a mesh with no vertices and no indices

### Requirement: Field attributes stay at level 0
Colours and gradient normals SHALL be refused at level 1 rather than approximated. They are evaluated through per-brick culled tapes whose agreement with the whole document's rests on a vertex sitting on the FIELD's surface, well inside the band; a coarse vertex sits on the mip's surface, which can be most of a coarse cell away, where the two tapes are only both out of band rather than equal. The mip also carries no colour lattice of its own, which the brick readback already reports rather than averaging.

Face normals are computed from the triangles, need no field, and SHALL work at every level — otherwise "refused" would silently mean "no normals at level 1".

#### Scenario: The refusal is about the level, not the parameters
- **WHEN** the same gradient-normal and colour requests are made with a document at level 0 and at level 1
- **THEN** level 0 succeeds and carries the attributes, and level 1 is refused as an invalid argument

#### Scenario: Face normals answer at every level
- **WHEN** a host meshes level 1 asking for face normals and passing no document
- **THEN** the mesh carries normals

### Requirement: A host can discover a layer's nodes
The C API SHALL let a host enumerate the nodes a layer holds by a count and an index, where the index is the layer's EVALUATION order, mirroring the layer-level pair the discovery requirement above defines. An index at or beyond the count SHALL be a typed not-found, so a host walks to the end without a sentinel, and a layer id that names no layer SHALL be a typed not-found as well.

The enumeration SHALL cover the layer's TOP-LEVEL nodes only, and the header SHALL say so plainly: it is the sibling of the group-children query, which continues to descend, so the whole tree is walked by pairing the two — enumerate the layer's roots, ask the node-primitive query what each one is, and recurse through the ones it refuses as groups. A layer's root SHALL NOT be addressable as a node id, because id 0 is the no-node sentinel and a call that answered for it could no longer refuse an id that does not exist.

Enumeration SHALL go through the index rather than the id space, because node ids are not dense and nothing bounds the length of a gap left by a removal: probing ids upward against the node-primitive query loses every node past the longest run of misses the caller happened to tolerate. A layer that carries no SDF content — a voxel or a mesh layer — SHALL count zero rather than fail, the same reading the per-layer field evaluation makes of it. Reading is not editing: a ghosted, locked or hidden layer SHALL answer normally. The addition SHALL be purely additive — no existing signature changes and no existing call's meaning moves.

#### Scenario: A reloaded document finds its armature without probing
- **WHEN** a document whose layer holds an item, a group and an armature placed after a run of removed nodes is saved, reloaded, and walked by enumerating layers, then enumerating that layer's nodes, then asking each node which primitive it carries
- **THEN** the armature is found without probing any node id and without a tolerance for missing ids, and its points and its parents read back exactly as authored

#### Scenario: Enumeration is top level, children descends
- **WHEN** a layer holds a loose item and a group containing two items, and the layer's nodes are enumerated
- **THEN** exactly the loose item and the group are reported, the group's items are not, and the group's items are reached by the group-children query on the enumerated group id

#### Scenario: Removed nodes leave gaps the index steps over
- **WHEN** several consecutive nodes are removed and a node is added after them
- **THEN** enumeration reports the surviving nodes and only those, in evaluation order, while each removed id is refused by the node-primitive query

#### Scenario: The enumerators keep their refusals typed
- **WHEN** the count or index query is given an id that is not a layer's, an index at or past the count, or a null out-parameter
- **THEN** the first two are refused as not found and the last as an invalid argument, with nothing written

### Requirement: A host can rename a layer
The C API SHALL let a host change a layer's name after creation, so that the name a document persists is the name the artist chose. Until this existed a layer was named only by the call that created it, the rename lived in the host alone and was lost on the next save, and the layer-discovery surface reported the stale creation name back with no way to tell it from a current one.

The rename SHALL go through the same command vocabulary as every other layer mutation: one rename is ONE undo step whose inverse restores the previous name exactly, and a ghosted or locked layer SHALL refuse it with the typed invalid-argument refusal that every other edit of a protected layer gives, rather than applying it silently.

A NULL name and an empty name SHALL be refused, and a refused rename SHALL leave the name unchanged. The name SHALL have no length limit imposed by this call: the saved layer record length-prefixes it and the name query sizes before it writes, so a long name costs a reader a larger buffer rather than a truncation.

Names SHALL NOT be required to be unique, because the calls that create layers have never required it and a uniqueness enforced on renames alone would be a guarantee the document does not keep. Instead the meaning of a duplicate SHALL be documented at the call: the by-name layer lookups answer with the FIRST layer in stack order carrying the name, so a rename onto a name already in use shadows the other layer rather than rebinding it, and a host whose lookup must survive a rename holds the layer id, which is stable across a save and reload.

The addition SHALL be purely additive — no existing signature changes, no enumerator's value changes, and no document format version moves, since the layer record has always carried the name.

#### Scenario: A rename survives a save and reload
- **WHEN** a layer is created, renamed, and the document is saved and loaded into a fresh document
- **THEN** the layer's name reads back as the NEW name, for an SDF, a voxel and a mesh layer alike

#### Scenario: A rename is one undo step
- **WHEN** a layer is renamed twice with undo enabled and undone twice
- **THEN** each undo restores exactly the previous name, ending at the creation name, and redo re-applies each rename in order

#### Scenario: The by-name lookup follows the rename
- **WHEN** a voxel layer is renamed and its grid is looked up by the new name
- **THEN** the lookup answers with that layer's grid, and the old name is refused as not found

#### Scenario: A duplicate name shadows rather than rebinds
- **WHEN** a layer is renamed to a name another layer already carries
- **THEN** the rename is accepted, both layers keep the name, and the by-name lookup answers with whichever of them is first in stack order

#### Scenario: A protected layer refuses the rename
- **WHEN** a ghosted or locked layer is renamed
- **THEN** the call is refused as an invalid argument, the name is unchanged, and releasing the protection makes the rename possible again

#### Scenario: An unusable name is refused before it replaces a good one
- **WHEN** a rename is called with a null document, a null name, an empty name, or an id no layer has
- **THEN** it is refused — invalid argument for the first three, not found for the last — and no layer's name changes

### Requirement: A host can tell a degraded backend from an absent one
The C ABI SHALL let a caller ask which operations a named backend can run, and SHALL let a caller read why a backend is absent or degraded. `clay_list_backends` answering `cpu` currently means either "no GPU backend is compiled into this build" or "one is, and it was discarded" — two states a host must act on differently and cannot distinguish.

`clay_backend_supports` SHALL report, for a named backend and a named operation, whether that operation is available. A backend that is not registered SHALL be `CLAY_ERROR_NOT_FOUND` rather than "supports nothing", because those are different answers: one is a backend that cannot do the thing, the other is a backend that is not there.

`clay_backend_diagnostic` SHALL return, by the size-query pattern `clay_list_backends` uses, a human-readable account of why a backend is unavailable or partial. It SHALL answer for a backend that is NOT registered — that is its main use — and SHALL yield an empty string for a backend that registered with every operation available, so "nothing to say" is expressible and is not an error.

The diagnostic SHALL carry the runtime's own words where it has any. A pipeline that failed to compile SHALL contribute the compiler's log rather than a summary of it: the log is what identifies the cause, and a host reporting a bug to us cannot recover what was already discarded.

Both calls SHALL be readable at any time and SHALL NOT require a document, a device or any prior call — a host decides which backend to ask for BEFORE it has built anything.

#### Scenario: A partial backend reports the operation it cannot run
- **WHEN** a backend is registered whose raycast pipeline is unavailable
- **THEN** `clay_backend_supports` reports the point and grid operations as available and raycast as unavailable, and `clay_backend_diagnostic` is non-empty and names the pipeline that failed

#### Scenario: An absent backend is not a silent one
- **WHEN** a backend is compiled in, fails to initialize, and is therefore not in `clay_list_backends`
- **THEN** `clay_backend_supports` for it is `CLAY_ERROR_NOT_FOUND` and `clay_backend_diagnostic` returns the reason it failed

#### Scenario: A backend that was never compiled in says so differently
- **WHEN** a backend name that this build does not contain is asked about
- **THEN** the diagnostic distinguishes "not compiled into this build" from "compiled in and failed", rather than answering the same way for both

#### Scenario: A fully working backend has nothing to say
- **WHEN** a registered backend has every operation available
- **THEN** `clay_backend_diagnostic` succeeds with an empty string, and `clay_backend_supports` reports every operation as available

