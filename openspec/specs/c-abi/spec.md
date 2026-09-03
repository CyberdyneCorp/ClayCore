# c-abi Specification

## Purpose
The boundary every host crosses, and the rules that keep it crossable.

Opaque handles, integer results, caller-owned buffers, no C++ type and no
exception in a signature — so that Swift, C# or Rust can consume it without a
shim. The versioned descriptor convention is what lets the surface GROW without
breaking a host compiled against an older header: a caller declares the layout
it knows, and a field appended later keeps its documented default.

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

The rule SHALL bind in BOTH directions. Where a descriptor is an OUTPUT, `struct_size` is the caller declaring how much of the struct exists rather than how much it filled in, and the library SHALL write no more than that many bytes — however many the struct has grown to in the build being called. Validating an incoming size and then filling the struct to the current `sizeof` is not compliance: it is a buffer overrun on exactly the caller the rule exists to serve, since a host that recompiles is never the one at risk. The declared size SHALL be returned unchanged, because it describes the caller's buffer; returning the library's own size would advertise fields that were never written.

Bounding the write SHALL NOT become a truncation for a current caller: a caller declaring the layout it was compiled against SHALL receive every field that layout contains, including fields appended after the original.

#### Scenario: Older caller against newer library
- **WHEN** a caller sets `struct_size` to the size it was compiled against and the library has since appended fields
- **THEN** the call succeeds, only the declared prefix is read, and the appended fields take their documented defaults

#### Scenario: An output descriptor from an older header
- **WHEN** a host compiled against an older layout passes an output descriptor declaring that layout's size, and the library has since appended fields to it
- **THEN** the library fills only the bytes the host declared, leaves everything past them untouched, and returns the size the host declared

#### Scenario: An output descriptor from the current header
- **WHEN** a caller declares the current layout of an output descriptor
- **THEN** every field is filled, including those appended after the original layout

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

Every entry point that bakes a DOCUMENT SHALL evaluate the tape in BLOCKS of sample positions rather than one point at a time, through the same batched evaluator a layer bake uses. A tape instruction costs roughly ten times its own arithmetic, so the interpreter is most of a bake and a per-point walk pays that interpreter once per sample instead of once per block; the difference is more than an order of magnitude on a document of a few hundred items. The result SHALL be byte-identical to the per-point walk, not merely close — each sample depends only on its own position and blocks are assembled in slot order however they were computed, so no scheduling can change any of them — and a benchmark pairing the two SHALL gate it, because this is a property that was already true of one bake path and silently untrue of the others.

A verb that SHAPES the field as it bakes — flatten is the one — SHALL apply that shaping to the sampled block BEFORE the sparsity decision is taken, not to the volume afterwards. Which bricks a bake keeps is decided from the values it is handed, and a verb that moves the surface by more than a band would otherwise keep the bricks around the surface its SOURCE had and leave the result's own surface in a region nothing sampled.

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

#### Scenario: The batched bake and the per-point walk agree exactly
- **GIVEN** a document whose field is steep enough that kept bricks hold values far past the band
- **WHEN** it is baked once through the batched evaluator and once through a per-point walk of the same tape, at the same cell and band
- **THEN** the two volumes serialize to the same bytes — the same samples, the same sparsity, the same bounds

#### Scenario: A document-sourced flatten keeps the bricks around the flattened surface
- **WHEN** a document-sourced flatten draws the surface onto a plane several band widths from where the source put it
- **THEN** the bricks the result stores are the ones around the flattened surface, and its band tracks that surface rather than the source's

#### Scenario: A bake without a backend still bakes
- **GIVEN** a build in which no CPU backend is registered
- **WHEN** a document is baked
- **THEN** it produces the same volume, evaluated serially, rather than failing

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

An EXPORTER SHALL match it the same way, and by the same normalisation the importer uses. The two disagreed: `clay_mesh_load` lowercased the extension and `clay_mesh_save` compared it as given, so a host could load `MODEL.OBJ` and then be refused when it saved back to the path it had just read from. A format name given to a memory entry point SHALL be normalised identically, so that one rule covers reading, writing, paths and buffers.

#### Scenario: An upper-case extension loads
- **WHEN** a mesh file whose extension is upper-case is loaded
- **THEN** it loads, rather than being reported as an unknown format

#### Scenario: An upper-case extension saves
- **WHEN** a mesh is saved to a path whose extension is upper-case
- **THEN** it is written in that format, rather than refused as unknown

#### Scenario: One rule for names and extensions
- **WHEN** the same format is named in upper case to a memory entry point and in upper case as an extension to a path entry point
- **THEN** both are accepted and produce the same format

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

The refill path SHALL carry colour when the cache was configured for it: the batched evaluation call SHALL be able to produce a colour lattice beside the distances, and submission SHALL accept it, so that colour reaches the cache by the same route distance does and there is no second path to keep consistent.

The handle SHALL take no lock and own no thread; the header SHALL state that calls on one handle are the host's to serialize and that the batched evaluation call, which takes no handle, is free-threaded against one const document. A batched const query — the many-ray raycast — MAY fan its rays out across the engine's shared worker pool, provided the call returns only after every worker is done with the cache and each output slot is byte-identical to what the single-ray call reports for the same ray.

A dirty region SHALL be validated in 64-bit before the engine converts it: a non-finite or empty region, a brick coordinate outside `int32`, and a span above the batch ceiling SHALL each be refused with the cache left unchanged. Dirtying everything the cache tracks SHALL be spelled as the absence of a region, never as a region carrying an infinity.

Brick readback SHALL write a consumer's own buffer at a fixed stride in the engine's stored bits, and SHALL accept an optional apron so that the stride is a padded, directly filterable tile when the consumer asks for one. The buffer length SHALL continue to be required exactly rather than inferred, against whichever stride the call was asked for.

Meshing the cache SHALL accept an optional set of brick keys and an optional per-key range report, so a consumer can re-mesh and re-upload what a drain reported dirty. Passing no set SHALL mean every surface brick, which is the behaviour a caller has today.

Brick raycasting SHALL have a batched form matching the document-level batched raycast in ray layout and in optional outputs.

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

#### Scenario: A host uploads a colour atlas without meshing
- **WHEN** a consumer configures a cache for colour, refills it, and reads the surface bricks back with colour and a one-voxel apron
- **THEN** it holds directly uploadable, directly filterable distance and colour tiles for the whole narrow band, having compiled no kernel of its own

#### Scenario: A dirty drain feeds the mesher
- **WHEN** the keys a drain reported are handed to the cache mesher as its key set
- **THEN** only those bricks are marched, and the per-key ranges name where each landed in the output

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

### Requirement: The compiled tape is exportable
A consumer SHALL be able to obtain a document's compiled tape through the C ABI: the instruction array, the parameter array and the out-of-line blob, in the layout the published kernel headers define, since the consumer's evaluator is compiled from those same headers.

The export SHALL include what an evaluator cannot derive from the buffers alone: the field info the safe step scale comes from, and the tape's bounds. A host that guesses its step scale draws a wrong frame; one that guesses its bounds draws a slow one.

The export SHALL carry the document revision the tape was compiled at, so a consumer can tell whether the copy it holds is still current without comparing buffers.

Ownership across the boundary SHALL be explicit and SHALL NOT depend on the consumer noticing a mutation: no exported pointer may be silently invalidated by a subsequent edit.

The tape encoding SHALL be versioned with the published kernel package, and a version the consumer does not support SHALL be detectable and refused rather than reinterpreted.

#### Scenario: A host evaluates the exported tape
- **WHEN** a consumer exports a tape and evaluates it with `ctape_eval` from the published kernel headers at the same points the library evaluates
- **THEN** the results agree within the host-parity tolerance the fixture already gates

#### Scenario: An edit is detectable without re-reading the tape
- **WHEN** the document is edited after a tape was exported
- **THEN** the consumer can tell that its copy is stale from the revision alone

#### Scenario: An edit does not invalidate an export in use
- **WHEN** a consumer holds an exported tape and the document is edited
- **THEN** the data the consumer holds remains valid and readable for as long as the ownership rule says it does, and the rule does not depend on the consumer having observed the edit

#### Scenario: A version mismatch is refused
- **WHEN** a consumer built against a different kernel package version reads the export
- **THEN** the mismatch is detectable and the export is refused rather than interpreted under the wrong layout

#### Scenario: Exporting does not disturb evaluation
- **WHEN** a tape is exported while the same document is being evaluated from another thread
- **THEN** both proceed correctly, consistent with a document staying readable from several threads at once

### Requirement: A host can discover a document's layers
The C API SHALL let a host enumerate a document's layers by a count and an index, where the index is STACK POSITION — the order evaluation uses and `clay_document_move_layer` sets — so the set and the order of a reloaded document are recoverable together. An index at or beyond the count SHALL be a typed not-found, which is how a host walks without a sentinel. Ids SHALL remain stable across a save and reload, and enumeration SHALL go through the index rather than the id space, because a removal leaves a gap in the ids.

Everything settable about a layer SHALL be readable back: a versioned info descriptor (leading `struct_size`, per the descriptor convention) SHALL carry the layer's id, representation, stack index, visibility and both protection flags in one call, and the layer's name SHALL be returned by the size-query pattern, since it is the one layer property without a fixed size. The representation SHALL be declared as an enumeration whose values match the layer record's kind byte in a saved document, appended and never renumbered.

Reading is not editing: a ghosted, locked or hidden layer SHALL answer every discovery query normally. The addition SHALL be purely additive — no existing signature changes, no struct grows, no enumerator's value changes.

#### Scenario: A reloaded document comes back whole
- **WHEN** a document with SDF, voxel and mesh layers — renamed at creation, one hidden, one locked, one ghosted, reordered with a move, one layer removed — is saved, reloaded and enumerated
- **THEN** the count, the stack order and every layer's id, name, representation, visibility and protection match what was saved, in a number of calls proportional to the layer count

#### Scenario: Stack order is the enumeration order
- **WHEN** a layer is moved to a new stack position and the document is enumerated
- **THEN** the enumeration reflects the move, and each layer's reported stack index is the index that would re-address it

#### Scenario: A removed layer's id is a gap, not a guess
- **WHEN** a layer is removed and the document is saved and reloaded
- **THEN** enumeration yields the surviving ids only, and the removed id is refused as not found by the info and name queries

#### Scenario: The info descriptor is versioned like every other
- **WHEN** a caller passes an info struct whose `struct_size` is zero or below the original layout
- **THEN** the call is refused as an invalid argument rather than misread

#### Scenario: The name query sizes before it writes
- **WHEN** a host queries a layer's name with a null buffer and then with a buffer of the reported size
- **THEN** it receives the required size including the terminator and then the name; a buffer that is too small is refused with the needed size reported and nothing written

### Requirement: A mesh sculpting session
The C ABI SHALL expose a **sculptor handle** created over a mesh, owning the two structures that are expensive to build and cheap to keep: the vertex adjacency and the ray-query BVH.

The handle SHALL be the entry point for stamping, stroking and picking, because building an adjacency per stamp is the whole cost of a stroke and an interface that hid the build would pay it every time.

The adjacency SHALL NOT need rebuilding after any verb, because no verb changes topology; the header SHALL say so. The BVH SHALL be refreshable by an explicit call, and the header SHALL state that positions moving without a refresh make picking report the surface as it was.

The handle SHALL keep the mesh alive for its lifetime and SHALL be destroyed by its own destroy call.

#### Scenario: A session is built once and stamped many times
- **WHEN** a host creates a sculptor over a mesh and applies a hundred stamps
- **THEN** the adjacency is built once, no call rebuilds it, and the mesh's index and quad buffers are unchanged throughout

### Requirement: The verbs across the ABI
The C ABI SHALL expose every verb — grab, draw, inflate, smooth, pinch, flatten, clay, crease, scrape, polish and snakehook — as enumerators on ONE versioned descriptor carrying the brush centre, radius, strength, falloff curve, direction, the surface-versus-straight-line falloff choice, the flatten mode with its optional explicit plane, the polish angle and the smoothing iteration count.

The descriptor SHALL carry the leading `uint32_t struct_size` every descriptor in this ABI carries.

An unknown verb, an unknown falloff and an unknown flatten mode SHALL each be REFUSED with `CLAY_ERROR_INVALID_ARGUMENT` rather than mapped onto the default, as the mesher enums already are. A non-positive radius SHALL be refused. A smoothing iteration count SHALL be bounded at this boundary, because each iteration walks the region again.

#### Scenario: An unknown verb is refused
- **WHEN** a host passes a verb value outside the declared list
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` with a detail message and the mesh is unchanged

#### Scenario: A cost knob nobody could have meant is refused
- **WHEN** a host passes a smoothing iteration count far above what any brush would spend
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the mesh is unchanged

### Requirement: Strokes and masks across the ABI
The C ABI SHALL expose a whole-stroke entry point taking the stroke preset descriptor and the stroke samples it already declares, resolving them through the same `resolve_stroke` the voxel and mask consumers use, and SHALL accept an optional mask handle gating every verb by `1 - mask`.

It SHALL report how many stamps moved a vertex.

#### Scenario: A stroke and a mask reach a mesh
- **WHEN** a host resolves a stroke with a preset and applies it to a sculptor with a mask covering half the region
- **THEN** only the unmasked vertices moved and the reported stamp count is the number that acted

### Requirement: Vertex deltas across the ABI
The C ABI SHALL expose a **delta record handle** that a stamp or a stroke writes into, reporting the number of vertices it holds, reverting the mesh to its pre-record state, re-applying, and clearing.

Reverting SHALL restore positions and normals bit-exactly. The record SHALL coalesce, so passing one record through a whole gesture yields one undo step.

#### Scenario: A host gets undo without snapshotting the mesh
- **WHEN** a host passes one record through a stroke of many stamps and then reverts it
- **THEN** the mesh's vertex buffer is byte-identical to its pre-stroke contents, and the record's vertex count is at most the number of vertices the stroke reached

### Requirement: Mesh picking across the ABI
The sculptor handle SHALL answer a ray query with a hit flag, the world-space position and normal, the triangle index, the barycentric coordinates and the ray parameter, taking the layer transform as the query's frame.

#### Scenario: A host turns a tap into a brush centre
- **WHEN** a host casts a ray at a mesh layer and feeds the returned position into a stamp descriptor
- **THEN** the stamp lands on the surface the ray hit

### Requirement: Quad meshing across the ABI
The C ABI SHALL expose quad meshing for both sources: the document's SDF content and a voxel grid. Both SHALL take one versioned descriptor carrying the lattice cell size, an optional target quad count with its tolerance and iteration cap, and the mode.

The descriptor SHALL carry the leading `uint32_t struct_size` every descriptor in this ABI carries, so the count controls can be appended to later without a major bump.

The descriptor SHALL also carry the two controls that belong to the voxel source alone — the occupancy blur the smooth mesher already takes, and the resolution LEVEL, which in faces mode is the count lever. Both SHALL be ignored for a document, so one descriptor serves both entry points rather than two that must be kept in step.

The mode SHALL be checked against the declared list and an unknown value SHALL be rejected rather than mapped onto the default, as the mesher enum already is. The dual mode SHALL be zero, so a caller whose declared size predates the field gets the lattice dual.

A call naming NEITHER a cell size nor a target SHALL be refused: neither names a lattice, and picking one on the caller's behalf would spend an unbounded amount of work on a number nobody chose.

The iteration cap SHALL be bounded at this boundary and the target SHALL be bounded by `CLAY_MAX_BATCH`, because every iteration is a whole mesh: a byte count passed where a mesh count belongs, or a negative widened to an unsigned, would otherwise buy that many dense field evaluations. Both are refusals rather than clamps, for the reason every other count check here is.

A tolerance or an iteration cap of ZERO SHALL mean the default, because the appended descriptor fields arrive as zero from a caller who declared only the original layout. A NEGATIVE iteration cap SHALL be refused rather than read as that default — it is a mistake and not a request — and a tolerance of 1 or more SHALL be refused, because 100% of the target makes `within_tolerance` true for almost any count and so reports nothing. These rules SHALL hold identically in the Python binding: the two SHALL not disagree about what a value means.

The faces mode SHALL be voxels only. A document asked for it SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` rather than quietly given the dual: a silent substitution of a smooth mesh for a boxy one is visible in the render and invisible in the return code.

These SHALL be NEW entry points. `clay_document_mesh`, `clay_document_mesh_combined`, `clay_voxel_mesh`, `clay_voxel_mesh_smooth` and `clay_voxel_mesh_chunks` SHALL return exactly what they return today, carrying no quads.

The C header SHALL state, at these entry points, that the output is a lattice-derived quad grid and NOT field-aligned retopology — no edge loops, no feature-placed poles, not animation-ready.

#### Scenario: A document quad-meshes
- **WHEN** a host calls the document quad mesher with a cell size and the dual mode
- **THEN** it receives a mesh whose quad count is non-zero and whose triangle indices are that quad list's triangulation

#### Scenario: Faces mode on a document is refused
- **WHEN** a host asks a document for the faces mode
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` with a detail message, and no mesh is produced

#### Scenario: A cost knob nobody could have meant is refused
- **WHEN** a host passes an iteration cap far above what any search would spend, or a target above the batch ceiling
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and no mesh is produced

#### Scenario: The count knobs default at zero and are bounded at the far end
- **WHEN** a host passes a tolerance of zero and an iteration cap of zero alongside a target
- **THEN** the call meshes with the documented defaults and reports a search that ran
- **AND** a negative iteration cap, and a tolerance of 1 or more, each return `CLAY_ERROR_INVALID_ARGUMENT`

#### Scenario: A descriptor that predates the fields still meshes
- **WHEN** a caller declares the descriptor's original size
- **THEN** the call meshes with the dual mode and no target, the appended fields taking their zero defaults

#### Scenario: The existing meshers are untouched
- **WHEN** the existing document, voxel, smooth and per-chunk mesh calls run after quad meshing exists
- **THEN** each returns the same vertices and indices it returned before, and reports no quads

### Requirement: A mesh reports its quads and how it reached them
`clay_mesh` SHALL report its quad count, SHALL expose a borrowed pointer to the quad indices with the lifetime rule every other borrowed mesh pointer has, and SHALL offer a copy-into-a-caller-buffer form alongside the existing index copy, taking the exact element count for the same reason that one does.

A mesh carrying no quads SHALL report a count of zero and a null pointer, never a fabricated pairing of its triangles.

The existing accessors — vertex count, index count, positions, normals, colours, uvs, indices, the interleaved vertex copy, bounds, validation and save — SHALL be unaffected. The index accessors SHALL keep reporting the triangulation, because that is what a GPU consumer draws.

A mesh SHALL additionally report how it was produced: the lattice cell size it was meshed at, the target it was given (zero when none), the count it reached, the iterations the search spent, whether it landed inside the tolerance, and whether it clamped. This is the ONLY way a host learns that a target of fifty thousand produced thirty-one thousand because a ceiling stopped the search. Asking a mesh that was not quad-meshed SHALL be refused rather than answered with zeroes.

The report describes a meshing CALL and not a surface, and SHALL travel exactly as far as that statement stays true. A transform carries it, because the result is the same mesh moved. A concatenation SHALL NOT, because it was produced by no single call. A mesh borrowed back out of a document layer SHALL NOT either, for the same reason: the geometry crossed, the call did not.

`clamped` and `within_tolerance` are independent, and BOTH SHALL be reported as they are: a search that stopped at a limit and happened to land inside the tolerance there sets both, and each is a true statement about what happened.

#### Scenario: A host reads the quads
- **WHEN** a host quad-meshes and reads the quad count and pointer
- **THEN** the pointer addresses four indices per quad, all within the vertex count, valid until the mesh is destroyed

#### Scenario: A triangle mesh reports no quads
- **WHEN** a host reads the quad count of a mesh produced by any existing mesher, loaded from a file, or built from triangles
- **THEN** the count is zero and the pointer is null

#### Scenario: The report explains the number the host got
- **WHEN** a host asks for a target the resolution ceiling cannot reach and then reads the report
- **THEN** the report states the count actually produced, the cell size used, and that the search clamped without reaching the tolerance

#### Scenario: A mesh that was never quad-meshed has no report
- **WHEN** a host asks a mesh loaded from a file for its quad report
- **THEN** the call is refused with an error code rather than answering with zeroes

### Requirement: Quads follow a mesh through the calls that copy one
A mesh transform SHALL keep the quads it was given: it moves positions and rotates normals and does not touch indices.

Concatenation SHALL carry quads only when EVERY input carries them, rebasing them onto the concatenated vertices as it rebases the triangles, and SHALL drop them entirely otherwise. This is the attribute-drop rule the header already states for normals, colours and uvs, applied for the same reason: a result that was quads over part of itself and triangles over the rest is not a quad mesh, and no call in this ABI may return a mesh whose arrays contradict each other.

Attaching a mesh as a document layer SHALL copy its quads with its geometry, and the borrowed mesh SHALL report them.

Saving SHALL write quads in the formats that carry them. The header SHALL state at the save entry point that OBJ, PLY and FBX carry quads and that GLB does not, because glTF 2.0 has no quad primitive mode.

#### Scenario: A transformed quad mesh is still a quad mesh
- **WHEN** a quad mesh is transformed
- **THEN** the result carries the same quad list over the moved positions

#### Scenario: Mixed concatenation drops quads
- **WHEN** a quad mesh is concatenated with a mesh carrying none
- **THEN** the result carries no quads and its triangles are the concatenation, exactly as before

#### Scenario: A quad mesh layer keeps its quads
- **WHEN** a quad mesh is added as a document layer and the layer's mesh is borrowed back
- **THEN** the borrowed mesh reports the same quad count and the same quad indices

### Requirement: A host can display a voxel sculpt as a form
The C ABI SHALL expose the smooth voxel mesh, so a host can show a sculpt as a rounded form rather than as boxes without meshing the grid itself.

The call SHALL be additive and SHALL sit beside `clay_voxel_mesh` rather than replacing it: the blocky mesh stays reachable, keeps its behaviour byte for byte, and remains what a host uses for export and for hard-surface voxel work.

It SHALL take the smoothing setting as an explicit argument rather than reading a mode from the grid, so two hosts sharing a document cannot disagree about what the grid looks like, and so a host can offer both pictures of one sculpt without mutating it.

An empty grid SHALL yield an EMPTY mesh rather than an error, as `clay_voxel_mesh` does: a grid nobody has drawn in yet is an ordinary state of a session.

#### Scenario: The same grid answers both ways
- **WHEN** a host meshes one grid with `clay_voxel_mesh` and with the smooth call
- **THEN** both succeed, the blocky mesh is byte-identical to what it was before the smooth call existed, and the smooth mesh describes a rounded form over the same occupancy

#### Scenario: Smoothing is the caller's choice, not the document's
- **WHEN** two hosts mesh the same unmodified grid with different smoothing settings
- **THEN** each receives the mesh it asked for and the grid is unchanged by either call

### Requirement: A host converts a coloured sculpt in one call
Converting a voxel sculpt into the document SHALL be able to produce ONE volume carrying the palette, rather than one volume per palette entry. The per-entry conversion exists because a field had nowhere to store a palette; once it does, a forty-entry sculpt SHALL NOT become forty items.

`clay_voxel_to_layer` SHALL keep its signature and produce a single item. This changes what a host counts after converting, and SHALL be stated in the header rather than discovered: a caller that counted one node per palette entry will now count one.

Converting a SINGLE palette entry SHALL remain available, because it is how a caller assembles a sculpt by hand and how a host takes one part of a sculpt as its own operand.

A host SHALL be able to ask whether a volume item carries colour, so it can tell a converted sculpt from a bake that predates this and choose what to show.

#### Scenario: A coloured sculpt converts to one item
- **WHEN** a host converts a voxel sculpt carrying several palette entries
- **THEN** the layer holds one volume item, evaluating it reports each entry's colour in that entry's region, and the item reports that it carries colour

#### Scenario: One entry still converts alone
- **WHEN** a host converts a single palette index
- **THEN** it receives an item solid only where that entry's cells are, as before

### Requirement: A host converts a sculpt into a layer it can keep working on
The C ABI SHALL let a host convert a voxel sculpt into a layer of operands, so the sculpt can be booleaned, blended and deformed again rather than only displayed or exported.

The conversion SHALL be NON-DESTRUCTIVE: it SHALL create a new layer and SHALL leave the grid and the original layer untouched, so a host can offer "go back" by keeping what it had. The conversion is irreversible in what it discards — the procedural history — and a destructive default would cost a parametric model to one misclick.

It SHALL place one volume item per palette entry the grid carries, each with that entry's colour, unioned without a blend. Colour is authored data and a trip that drops it is unattractive whatever it does to the geometry; a blend between the parts would round an interface that is interior to the solid they make together.

It SHALL introduce no new layer kind and SHALL NOT move the document format version: the result is ordinary volume items in an ordinary SDF layer.

A conversion that cannot produce anything SHALL fail without having modified the document, rather than leaving an empty layer behind.

#### Scenario: The converted sculpt is an operand
- **WHEN** a host converts a two-colour voxel sculpt into a layer
- **THEN** the layer holds one item per palette entry, each carrying that entry's colour, and evaluating the layer reports solid inside the sculpt

#### Scenario: The original survives the conversion
- **WHEN** a sculpt is converted
- **THEN** the grid still holds the cells it held, and the layer it lives in is unchanged

#### Scenario: An empty grid leaves no wreckage
- **WHEN** a grid holding nothing is converted
- **THEN** the call is refused and the document has gained no layer

### Requirement: Rasterizing a mesh across the ABI
The C ABI SHALL expose mesh rasterization onto a voxel grid, taking a mesh handle and an OPTIONAL region.

Both region pointers SHALL be given or neither, as elsewhere in this ABI, and a region that is not finite, is empty, or is unbounded SHALL be REFUSED before the grid is touched — so a rejected call leaves the grid as it was rather than half-rasterized, exactly as the document rasterizer already guarantees.

A NULL region SHALL mean the mesh's own bounds rather than being an error, which is the one place this entry point differs from `clay_voxel_rasterize`: a document may have no bounded content and a mesh always does.

A mesh with no triangles SHALL be refused with a typed error rather than silently doing nothing, because a caller that loaded a file and got nothing wants to know.

The header SHALL carry the same statement of what the sampling preserves that the document rasterizer carries, and SHALL state that this is occupancy sampling and not retopology.

#### Scenario: A host rasterizes an imported model
- **WHEN** a host loads an OBJ and rasterizes it with a NULL region
- **THEN** the grid is filled over the mesh's own bounds and the call returns `CLAY_OK`

#### Scenario: A malformed region is refused and changes nothing
- **WHEN** a host passes one region pointer, or a region carrying a non-finite bound
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the grid is still empty

### Requirement: A mesh can be copied into a caller's own vertex layout
The ABI SHALL let a consumer copy a mesh's vertices into memory it owns, in an interleaved layout it describes, and its indices likewise — so that geometry reaches a mapped GPU buffer in one pass rather than through an interleave into a staging buffer followed by a copy.

The layout SHALL be a versioned descriptor naming a stride and a byte offset per attribute, with an attribute omitted by naming no offset for it. It SHALL describe placement only: attributes are copied in the float form the mesh holds, and format conversion is not part of this descriptor.

The destination length SHALL be required exactly and checked, never inferred, consistent with every other call that writes into a consumer's buffer. Requesting an attribute the mesh does not carry SHALL be refused rather than filled with a default, because a silently black or silently flat model is harder to diagnose than a returned error.

The mesh SHALL remain the engine's, produced and freed as it is today; this requirement adds a copy-out and SHALL NOT make meshing write into caller memory, since a mesher cannot report its vertex count before it has run.

#### Scenario: One pass into a mapped buffer
- **WHEN** a consumer describes a position/normal/colour interleaved layout and copies a mesh into a buffer sized from the mesh's vertex count
- **THEN** the buffer holds each vertex's attributes at the named offsets and stride, and the data matches the deinterleaved accessors element for element

#### Scenario: An absent attribute is refused, not invented
- **WHEN** a layout names a colour offset for a mesh that was meshed without colours
- **THEN** the call is refused and nothing is written

#### Scenario: A short destination is refused
- **WHEN** the destination is smaller than the stride times the vertex count
- **THEN** the call is refused rather than writing what the caller did not allocate

### Requirement: Every primitive is reachable on a host preview path
A consumer drawing this library's field on its own GPU SHALL be able to reproduce EVERY primitive the document can contain, including sampled volumes, without enumerating primitive kinds in its own code.

A host that implements a subset of the dialect draws a subset of the document, and the subset that goes missing is not arbitrary: sampled volumes are what every regional verb produces, so a preview lacking them shows nothing for a whole class of brush until a bake lands. Neither published path SHALL require a consumer to name primitives — one evaluates the compiled tape, whose out-of-line payload carries a volume's samples, and the other reads bricks filled by evaluating that same tape.

#### Scenario: A regional verb is visible before the bake
- **WHEN** a document containing a sampled volume is drawn through either the exported tape or the brick payloads
- **THEN** the volume contributes its surface, and the field a consumer evaluates agrees with the library's own at the same points

#### Scenario: A volume is found wherever it sits in the payload
- **WHEN** a sampled volume follows another item that carries out-of-line data, so it does not begin at the start of the payload
- **THEN** it is still evaluated correctly on both paths

### Requirement: A placed node answers what primitive it carries
The C API SHALL let a host ask a placed item which primitive it carries, returning the same enumeration item creation takes, so that a host that reloaded a document picks the reader that applies instead of probing readers until one stops refusing. A group SHALL be refused with a typed invalid-argument error — the dual of the children query refusing an item — so every node answers exactly one of the two questions. Reading SHALL NOT be refused on a protected or hidden layer.

#### Scenario: A reloading host finds its armature
- **WHEN** a host asks a placed armature node and a placed sphere node what they carry
- **THEN** it receives the armature and sphere enumerators it would have passed to create them

#### Scenario: A group is the other question
- **WHEN** the primitive of a group is asked for, and the children of an item
- **THEN** both are refused as invalid arguments, and each node answers exactly one of the two queries

### Requirement: Consolidation across the ABI
The C API SHALL expose consolidating a layer and reporting its cost before it is paid, reusing what a volume already reports — bytes, brick count, sample count and sample Lipschitz. The addition SHALL be purely additive.

The cost SHALL also carry what the sample Lipschitz IMPLIES — the declared
Lipschitz and the safe step scale — because those are the numbers a host budgets
a frame against, and deriving them from `sqrt(3) x max(l, 1)` in every binding
would be re-implementing a kernel combinator outside the kernel.

#### Scenario: The cost is knowable before consolidating
- **WHEN** a host asks what consolidating a layer would cost
- **THEN** it gets the memory and resolution it would spend, without the document changing

#### Scenario: The quote is the bill
- **WHEN** a host quotes a consolidation and then performs it with the same parameters
- **THEN** the brick count and byte count it was quoted are the ones it pays

### Requirement: The trigger is advisory across the ABI
The C API SHALL let a host measure a layer's degradation and SHALL NOT consolidate on its own. The threshold that turns a measurement into advice SHALL be an argument of the query rather than document state, because a tolerance for marching cost belongs to a viewport, a device and a frame budget rather than to the artwork — and storing it would need a document format bump to carry it.

#### Scenario: A host is told, and decides
- **WHEN** a host asks for a layer's field report with a step-scale threshold
- **THEN** it is told whether the layer has degraded past that threshold, and nothing is baked

#### Scenario: A measurement without a threshold makes no recommendation
- **WHEN** a host asks for a field report with a threshold of zero
- **THEN** it gets the numbers and no advice

### Requirement: Levels across the ABI
The C API SHALL expose adding, dropping, counting and selecting levels, and reporting one level's cell size and occupied count without making it active first. Every verb SHALL act on the level the grid has selected.

The addition SHALL be purely additive: it SHALL introduce new entry points only, SHALL NOT change the signature or layout of anything that already exists, and SHALL NOT move the ABI major. The level is therefore grid STATE rather than a parameter on every verb — passing it per call would have changed every voxel entry point's signature, which is exactly the break this requirement forbids.

An existing caller that never mentions a level SHALL get today's behaviour: a one-level grid, on which the level calls that ask for a second level are refused with `CLAY_ERROR_INVALID_ARGUMENT` and the grid untouched.

#### Scenario: An existing caller is unaffected
- **WHEN** a program compiled before this change runs against the new library
- **THEN** it links, and its grids behave as single-level grids exactly as before

#### Scenario: A level a grid does not have is refused, not guessed
- **WHEN** a caller selects, or asks the cell size or occupied count of, a level beyond the stack
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` with the grid unchanged, rather than returning a plausible-looking zero

### Requirement: Alphas reach the C ABI through their own entry point
The C API SHALL expose applying an alpha to an item through a dedicated call rather than the flat deformer call, because a variable-length sample array does not fit that signature — the same reason bend curve has its own entry point. The samples SHALL be COPIED, so a caller may free its buffer immediately.

#### Scenario: A malformed stamp is refused rather than applied
- **WHEN** a caller passes a null sample pointer, a non-positive dimension, or a dimension that disagrees with the sample count
- **THEN** the call is refused and the item is unchanged

### Requirement: The new mesh verbs and alphas reach the C ABI additively
The C API SHALL expose the added verbs as new enumerators and the alpha as new settings fields, so a caller compiled against the previous header behaves exactly as before.

Alpha samples SHALL be COPIED or borrowed only for the duration of the call, and the contract SHALL say which.

#### Scenario: An existing caller is unaffected
- **WHEN** a caller sets no alpha and uses no new verb
- **THEN** every existing call behaves exactly as before

#### Scenario: A malformed alpha is refused rather than applied
- **WHEN** a caller passes a null sample pointer or a non-positive dimension
- **THEN** the call is refused and the mesh is unchanged

### Requirement: Brick memory can be released across the C ABI
The C ABI is the only surface a packaged consumer has, so the cache's release path SHALL be reachable through it: trimming to a target number of bytes, and reporting the usage reached and the number of bricks dropped.

The surface SHALL mirror `brick::BrickCache` rather than inventing a second policy, and SHALL NOT publish an eviction loop, timer or threshold — a host asks, on its own schedule, for its own reasons (a platform memory warning being the expected one).

Whatever ordering decides which bricks go SHALL be stated in the ABI's documentation, and where the ordering depends on information only the host has, the host SHALL be able to supply it.

Statistics SHALL make visible the growth the budget does not bound, so a host can tell a cache that is holding data from one that is holding bookkeeping.

#### Scenario: A host answers a memory warning
- **WHEN** a host trims the cache to a target on receiving a platform memory warning
- **THEN** the call reports the usage reached and the bricks dropped, and every remaining brick is still readable and correct

#### Scenario: Trimming does not disturb the dirty set
- **WHEN** a trim happens while bricks are dirty and requests are outstanding
- **THEN** outstanding requests are still resolvable — accepted, or refused as stale by the ordinary generation rule — and no submitted brick lands in a slot it does not own

#### Scenario: Statistics show what the budget does not
- **WHEN** a host reads the cache statistics
- **THEN** it can see both the payload usage the budget bounds and the tracked-key growth it does not

### Requirement: A defaults call is an output descriptor
An entry point that fills a descriptor with engine defaults SHALL be treated as an output descriptor like any other: the caller SHALL set `struct_size` before the call, and the library SHALL bound its fill to that size. Setting `struct_size` on the caller's behalf SHALL NOT be offered as a convenience, because a descriptor the caller does not measure is one the library cannot bound — the fill then uses the library's own `sizeof`, which overruns any host built against a layout that has since grown.

A call whose descriptor declares no size, or a size below the descriptor's original layout, SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` and SHALL leave the caller's memory untouched. Refusal SHALL be understood as the best available outcome rather than a complete one: a host compiled before the requirement declares nothing, so it cannot be served correctly, and the choice is only between refusing it and corrupting it.

This SHALL apply however the descriptor is filled, including where an entry point fills it by delegating to another entry point rather than assigning it directly.

#### Scenario: A defaults call without a declared size
- **WHEN** a caller passes a descriptor whose `struct_size` is zero, or below the descriptor's original layout, to a defaults-style call
- **THEN** the call is refused with `CLAY_ERROR_INVALID_ARGUMENT` and no field of the caller's struct is written

#### Scenario: A defaults call from an older header
- **WHEN** a host declares the layout it was compiled against and that layout is shorter than the current one
- **THEN** the defaults are filled into exactly the bytes declared, nothing past them is touched, and the declared size is returned

#### Scenario: A defaults call from the current header
- **WHEN** a caller declares the current layout
- **THEN** every field is filled, including those appended after the original layout

### Requirement: The output-fill rule is enforced mechanically
The ABI hygiene gate SHALL verify that every entry point taking a versioned descriptor by mutable pointer fills it through a bounded write, rather than relying on review to notice. Searching the implementation for a single spelling SHALL NOT be considered sufficient: one site filled its output descriptor by delegating to another entry point, matched no such search, and was missed by a sweep that believed itself complete.

#### Scenario: An unbounded output fill is added
- **WHEN** an entry point writes a versioned output descriptor without a bounded fill
- **THEN** the C ABI hygiene check fails naming the entry point and the descriptor

### Requirement: A caller's device is adoptable across the ABI
The ABI SHALL let a consumer hand the library the device, queue and context it already owns, as an opaque handle the consumer creates and releases, and SHALL let evaluation run against that handle instead of against a device the library created.

Native objects SHALL cross as untyped pointers under a named graphics API, and `clay.h` SHALL NOT include, require or name any vendor header, since the header is read by bindings generators and its declared surface SHALL NOT vary with build configuration.

An API whose backend is not built, or a descriptor whose handles are incomplete for the API named, SHALL be refused when the device is adopted rather than at first use, so a consumer learns at the point it can still choose a fallback.

Adopting a device SHALL NOT change what the library computes. A consumer whose adoption is refused SHALL be able to use the registered backend and obtain the same values.

The header SHALL state that calls on one adopted device are the consumer's to serialize, as it already states for the brick cache.

#### Scenario: A host runs claycore on its own device
- **WHEN** a consumer adopts the device and queue it renders with, and evaluates through the resulting handle
- **THEN** the work runs on that device and the results match those the registered backend produces within the parity tolerance

#### Scenario: The header names no vendor type
- **WHEN** `clay.h` is parsed by a bindings generator with no graphics SDK present
- **THEN** it parses completely, and the set of functions it declares is the same one the built library exports

#### Scenario: An unbuilt API is refused at adoption
- **WHEN** a consumer names a graphics API whose backend was not compiled in
- **THEN** adoption fails with the reason available, nothing is retained, and the registered backends stay usable

### Requirement: Evaluation output can land in a caller's device buffer
The ABI SHALL expose grid evaluation and brick evaluation whose destination is a device buffer the consumer owns, described by an untyped handle, a byte offset and the size available from it.

The available size SHALL be required and checked against the lattice the call was given, and a destination too small SHALL be refused rather than partly written, consistent with this ABI never inferring a count.

The device form SHALL produce the same values as the host form for the same inputs, so that a consumer can verify one against the other.

The device-destination brick refill SHALL resume on the same terms as the host-memory one: a brick whose seed can be carried forward exactly SHALL be answered from it rather than by walking the whole surviving edit list, and a brick that the device walked in full SHALL leave a seed behind for the next call. The seed store SHALL be the DOCUMENT's, so a brick seeded through either refill entry point can be served by the other.

Where the adopted backend cannot move bytes between host memory and the consumer's buffer, the refill SHALL walk every brick in full — the behaviour it had before it resumed anything — rather than partially resuming, failing, or producing a field a full walk would not produce. The decision SHALL be taken before anything is written.

Where a seed cannot be described exactly for a brick this path answered — in particular where more than one visible SDF layer means a seed is two values and this path evaluated the document whole — the refill SHALL store no seed for it rather than store one whose meaning it cannot state.

#### Scenario: A brick refill never crosses host memory
- **WHEN** a consumer evaluates drained brick requests into its own device buffer
- **THEN** each brick occupies its own fixed stride in that buffer and no value is written to host memory

#### Scenario: A short device buffer is refused
- **WHEN** the size available from the offset is smaller than the results require
- **THEN** the call is refused and nothing is written

#### Scenario: A stroke resumes into the caller's buffer
- **GIVEN** a device-destination refill has left seeds for a window of bricks
- **AND** one item is appended to the active layer
- **WHEN** the same window is refilled into the consumer's buffer again
- **THEN** the bricks carrying a usable seed are answered from it
- **AND** what lands in the consumer's buffer matches a full walk of the same document to the backend-parity standard

#### Scenario: A resumed run lands where it belongs
- **GIVEN** a window whose resumable bricks neither begin nor end the batch
- **WHEN** it is refilled into the consumer's buffer
- **THEN** every brick occupies the same fixed slot it would have occupied had none of them been resumed

#### Scenario: Seeds cross between the two refill entry points
- **GIVEN** the host-memory refill has stored seeds for some bricks of a window
- **WHEN** the device-destination refill asks for that window
- **THEN** those bricks are answered from their seeds and the rest are walked in full

#### Scenario: A backend that cannot move bytes still answers correctly
- **GIVEN** an adopted backend that reports no host-to-device copy capability
- **WHEN** a consumer refills bricks into its device buffer
- **THEN** every brick is walked in full and the values are what a full walk produces

### Requirement: Undo reports the region it changed
The C API SHALL offer undo and redo entry points that additionally report the world-space INFLUENCE bound of what they applied, in the three-state shape the influence-bound queries already use: nothing changed, a finite box, or unbounded. The bound SHALL be usable directly as the region argument of the brick cache's dirty marking, with the unbounded state spelled the way that call already spells it.

Without this the narrowest region a host can honestly dirty after an undo is the whole layer, because nothing in the ABI says which nodes the step touched. Every alternative available to the host is worse: diffing the layer's nodes across the call misses an in-place change — an undone move, resize or colour edit keeps its node id — and under-dirtying leaves stale bricks at a blend seam, which is silent and on screen.

The bound SHALL be the union, over every command in the step, of what that command targets BEFORE it is applied and AFTER it is applied. One side alone cannot see a move (which has two ends), a removal (whose node is gone afterwards) or an add (whose node was not there before).

The bound MAY be larger than the region that actually changed and SHALL NOT be smaller. Where being tight would cost correctness it SHALL be conservative, and the two places that costs something are stated rather than left to the implementation:

A node inside a group SHALL report the bound of the NODE THE COMMAND NAMES, dilated by the blend support of each group on the path from that node up to its root — the group's blend spreads a child's influence past the child's own box, and that spread is what the ancestors' supports measure. It SHALL NOT report the whole root subtree's bound: a sibling's geometry is not something the edit can reach, and reporting it makes the region grow with the size of the group rather than with the size of the edit.

A command on content shared by instanced layers SHALL report the union over every layer that shares that content.

A node whose subtree combines non-locally — an intersect, an unbounded primitive, an infinite grid repeat — SHALL report the unbounded state, exactly as an influence-bound query does for the same node. The path dilation applies to the local case, and it SHALL NOT be used to turn a non-local subtree into a finite box.

A command that cannot change what the document evaluates to SHALL contribute nothing to the bound, so a step made only of such commands reports that there is nothing to dirty rather than reporting the layer.

The existing undo and redo entry points SHALL keep their signatures and their behaviour, and the reporting variants SHALL agree with them on what was undone and on the resulting document.

#### Scenario: Undoing a dab dirties the dab
- **WHEN** an item is added to a populated layer and the add is undone through the reporting variant
- **THEN** the reported bound contains the added item's influence bound and is strictly smaller than the layer's

#### Scenario: A move is bounded at both ends
- **WHEN** an item is transformed far from where it was and the transform is undone
- **THEN** the reported bound contains both the position it was moved to and the position it was moved back to

#### Scenario: An undone removal is bounded by what came back
- **WHEN** a node is removed and the removal is undone, restoring it
- **THEN** the reported bound contains the restored node's influence bound

#### Scenario: A group's blend is not cut off
- **WHEN** a child of a smooth-blended group is edited and the edit is undone
- **THEN** the reported bound reaches past the child's own box by the group's blend support, is never smaller than the child's own influence bound, and band-clamped values outside it are unchanged

#### Scenario: A sibling's geometry does not widen the bound
- **GIVEN** a group holding one small child and a large one far from it
- **WHEN** the small child is transformed and the transform is undone
- **THEN** the reported bound is strictly inside the group's own influence bound on the side facing the sibling, and it does not contain the far sibling's geometry

#### Scenario: Nested groups each contribute their support
- **GIVEN** a child inside a smooth-blended group which is itself inside another smooth-blended group
- **WHEN** the child is edited and the edit is undone
- **THEN** the reported bound is the child's bound dilated by both groups' blend supports, and band-clamped values outside it are unchanged

#### Scenario: A non-local subtree is still unbounded
- **WHEN** a child of a group whose combine is an intersect is edited and the edit is undone
- **THEN** the call reports the unbounded state rather than a finite box

#### Scenario: An instanced layer is not missed
- **WHEN** a layer is instanced, an edit is made through one instance, and that edit is undone
- **THEN** the reported bound covers where the change lands in every layer sharing the content, not only the layer named by the command

#### Scenario: Unbounded influence is expressible
- **WHEN** the undone step touched a node whose influence has no finite extent
- **THEN** the call reports the unbounded state rather than a finite box, and the host's honest response is to dirty everything

#### Scenario: An edit that changes no field dirties nothing
- **WHEN** a layer rename is undone
- **THEN** the call reports that there is nothing to dirty

#### Scenario: Nothing to undo is still not an error
- **WHEN** the reporting variant is called on a document with nothing to undo
- **THEN** it reports that nothing was undone, reports nothing to dirty, and returns success

### Requirement: The full mesh validation report crosses the ABI
The C API SHALL expose every quantity `mesh::ValidationReport` computes, through a versioned output descriptor rather than through individual out-parameters.

The descriptor SHALL carry the vertex and triangle counts, the watertight, manifold and oriented predicates, the boundary-edge, non-manifold-edge, degenerate-triangle, sliver-triangle and intersecting-pair counts, the Euler characteristic, and the derived clean predicate. It SHALL carry a leading `struct_size` and SHALL be filled bounded by the size the caller declares, per the versioned-descriptor rule.

The entry point SHALL accept the sampled self-intersection cap that the engine's validator accepts, so that the self-intersection pass named in the meshing capability is reachable from a binding at all. A cap of zero SHALL skip the pass, matching the engine's own default.

The report SHALL state whether the self-intersection pass ran, by carrying back the cap it was given. A caller SHALL be able to distinguish "no intersecting pairs were found" from "no intersecting pairs were looked for", because both leave the count at zero.

The existing two-boolean entry point SHALL keep working with identical results, defined as sugar over the report, so no consumer is broken.

#### Scenario: A hole is reported, not merely detected
- **WHEN** a mesh with one deleted triangle is validated through the report
- **THEN** the watertight predicate is false AND the boundary-edge count names how many edges are open, rather than the caller learning only that something is wrong

#### Scenario: The self-intersection pass is reachable
- **WHEN** a caller passes a non-zero self-intersection cap
- **THEN** the pass runs, the intersecting-pair count reflects it, and the report shows the cap that was used

#### Scenario: Not tested is distinguishable from none found
- **WHEN** a caller passes a cap of zero
- **THEN** the intersecting-pair count is zero, the reported cap is zero, and the caller can tell the pass did not run

#### Scenario: The old entry point is unchanged
- **WHEN** a caller uses the two-boolean validate entry point
- **THEN** it returns exactly the watertight and manifold values it returned before this change

#### Scenario: The descriptor obeys the versioned-descriptor rule
- **WHEN** a caller declares a `struct_size` below the report's layout, or a value too large to be any descriptor
- **THEN** the call is rejected with `CLAY_ERROR_INVALID_ARGUMENT` rather than reading or writing past the caller's object

#### Scenario: A newer caller's tail is ignored
- **WHEN** a caller declares a `struct_size` larger than this build's layout
- **THEN** the write is clamped to what the build knows, the unknown tail is left untouched, and the size the caller declared is returned unchanged

### Requirement: A mesh reports its volume and area
The C API SHALL expose the signed volume and the surface area of a mesh.

Both SHALL cross as `double`, matching the precision the engine computes them at, because a signed-volume sum over triangles cancels heavily and narrowing it at the boundary would discard the precision the engine chose deliberately.

The signed volume SHALL be positive when triangle normals point outward, so its sign is usable as an orientation check, and SHALL be reported for any mesh rather than refused for one that is not watertight — an open mesh has a divergence-theorem sum, and refusing to state it hides the number a caller uses to notice the mesh is open.

#### Scenario: Volume and area of a closed mesh
- **WHEN** a caller measures a closed, outward-oriented mesh
- **THEN** the signed volume is positive and both figures match the engine's own values

#### Scenario: An inverted mesh reports a negative volume
- **WHEN** a caller measures a closed mesh whose triangles are wound inward
- **THEN** the signed volume is negative, which is what makes it an orientation check

### Requirement: Serialized bytes cross the ABI
A caller SHALL be able to serialize a document or a mesh to memory, and to construct one from memory, without naming a filesystem path.

Serialized output SHALL be returned as an opaque owner handle carrying the bytes, with borrowing accessors for the pointer and the length and an explicit destroy — the pattern the mesh and tape handles already use. It SHALL NOT be returned by the size-query pattern, because answering the size would mean serializing the payload twice.

The borrowed pointer SHALL remain valid until the handle is destroyed and SHALL be unaffected by any subsequent edit to the object it came from, so a host may hand the bytes to an asynchronous writer without copying them first.

The bytes a memory save produces SHALL be byte-identical to what the corresponding file save writes for the same object, and a memory load SHALL accept exactly what a file load accepts. Neither direction SHALL introduce a format, a header, or a framing of its own.

A mesh memory entry point SHALL take the format by NAME rather than deriving it from an extension, because a buffer has none. The names SHALL be the file extensions without the leading dot and SHALL be matched case-insensitively, consistent with the existing extension rule. An unknown name SHALL be refused with the same code an unsupported extension is refused with, and SHALL NOT fall back to a default format.

#### Scenario: A document round-trips through memory
- **WHEN** a document is saved to memory and loaded back from those bytes
- **THEN** the loaded document evaluates identically to the original at every probe point, and its layers, names and stack order are recovered

#### Scenario: Memory and file agree byte for byte
- **WHEN** the same object is saved to a path and to memory
- **THEN** the file's contents and the buffer's bytes are identical

#### Scenario: The borrowed bytes survive an edit
- **WHEN** a host saves a document to memory and then edits the document before reading the buffer
- **THEN** the buffer still holds what was serialized, and destroying it is still the caller's single obligation

#### Scenario: An unknown format name is refused
- **WHEN** a caller asks to save a mesh to memory naming a format the library does not write
- **THEN** the call is refused rather than served in some default format

#### Scenario: Null and empty inputs
- **WHEN** a memory loader is given a null pointer, a zero length, or bytes that are not the format claimed
- **THEN** it fails with a typed error and produces no handle for the caller to free

### Requirement: The import guardrails apply to bytes where they still mean something
Every limit that guards a load from a path and that still has something to bound SHALL guard a load from memory identically. A limit that has nothing left to bound SHALL be stated as inapplicable rather than accepted and ignored.

The mesh import budget's vertex and triangle ceilings SHALL be accepted and enforced by the mesh memory loader, unchanged. A buffer is the more likely untrusted input of the two — it is what arrives from a network, a pasteboard or another process — so a memory path that skipped them would invert the protection they exist to give.

The budget's file-byte ceiling SHALL NOT be a parameter of any memory loader. It bounds the bytes a loader will read into memory before sizing a buffer, and a caller holding a buffer has already performed that read; accepting it there would be a parameter that cannot do anything, which is worse than not offering it. The document memory loader therefore SHALL take no budget at all, and the header SHALL say why rather than leave a reader to infer it from an absence.

Bounds checking SHALL NOT depend on the budget. Every memory loader SHALL refuse a truncated, corrupt or mis-declared buffer without reading past the length it was given, which is a property of the readers rather than of any ceiling.

#### Scenario: A budget refuses an oversized buffer
- **WHEN** a mesh is loaded from memory under a budget smaller than the mesh
- **THEN** the load is refused with the same error the file loader gives, and no mesh handle is produced

#### Scenario: A malformed buffer stays in bounds
- **WHEN** a truncated or corrupt buffer is loaded from memory
- **THEN** the loader refuses it without reading past the length it was given, whether or not a budget was supplied

### Requirement: A host can set and clear a layer's radial symmetry
The C ABI SHALL expose setting a layer's radial symmetry by count, axis and seam blend, and SHALL treat a count of 0 or 1 as clearing it. The call SHALL respect a locked layer and SHALL be undoable, matching the layer mirror rather than writing the field directly — the defect the mirror entry point was created to fix.

An axis outside 0..2, or a negative blend, SHALL be rejected with an invalid-argument result rather than clamped.

#### Scenario: Setting and clearing round-trips
- **WHEN** a host sets a radial count of 8 and then sets 0
- **THEN** both calls succeed, the second restores the un-arrayed field, and each is a separate undo step

#### Scenario: A locked layer refuses
- **WHEN** a host sets a radial count on a locked layer
- **THEN** the call fails and the layer is unchanged

### Requirement: Radial symmetry survives a document round-trip
A document written and read back SHALL preserve a layer's radial count, axis and seam blend. A document written by a build that predates the field SHALL load with the mode off rather than failing.

#### Scenario: Save and load preserves the mode
- **WHEN** a document with a radial layer is serialized and read back
- **THEN** the layer's count, axis and blend match, and the field evaluates identically

#### Scenario: An older document loads with it off
- **WHEN** a document written before this field existed is read
- **THEN** it loads successfully with a radial count of 0

### Requirement: A document reports what it costs, broken down by subsystem
The library SHALL report, in one call, the memory a document holds, separated into per-subsystem figures.

The report SHALL cover every subsystem a document owns: the edit list, voxel content, the sculpt layers held beside that content, masks, mesh layers, the undo history, and the passthrough blobs a document carries without interpreting. A subsystem the document owns SHALL NOT be omitted from the breakdown, because a figure that silently excludes the largest thing in the document is worse than no figure.

Voxel content and voxel sculpt layers SHALL be reported as SEPARATE figures. They live in the same object and one is the user's model while the other is undo for it; a combined figure would hide the only voxel bytes a host is permitted to release.

The individual figures SHALL sum to the reported total, so a host can attribute every byte it is told about.

Figures SHALL account for what containers have ALLOCATED, not for what they logically hold, since that is what the process is charged for. A report MAY therefore exceed the size the same document serializes to, and the interface SHALL say so rather than let a host read the difference as a defect.

The report SHALL be a versioned descriptor, since every subsystem added later becomes a field in it.

A memory report SHALL NOT include memory the document does not own. In particular the evaluation brick cache, which belongs to an evaluator and has its own accounting and its own trim, SHALL NOT be counted in a document figure.

#### Scenario: The report moves with the content
- **WHEN** content is rasterized into a voxel layer of a document
- **THEN** the reported voxel content figure rises, and the reported total rises with it

#### Scenario: The parts account for the whole
- **WHEN** a memory report is read for a document holding an edit list, a voxel layer, a mask and history
- **THEN** the per-subsystem figures sum to the reported total

#### Scenario: A cost is attributed to the subsystem that incurred it
- **WHEN** a mask is painted and nothing else is changed
- **THEN** the mask figure rises and the voxel content and edit list figures are unchanged

#### Scenario: An empty document reports a total
- **WHEN** a memory report is read for a document with no layers
- **THEN** the call succeeds and reports a total rather than an error

### Requirement: Memory is reportable for a single layer
The library SHALL report the same breakdown for ONE layer of a document, so a host can attribute a large document to the layer responsible for it rather than only learn that it is large.

A per-layer report SHALL use the SAME descriptor as the document-wide report. Fields that are document-wide rather than per-layer SHALL be reported as zero and documented as such, so that one struct serves both.

The CONTENT figures — voxel content, voxel sculpt layers, masks and mesh layers — SHALL sum exactly across the layers to the document-wide figures, since every byte of content belongs to exactly one layer.

The edit-list figure SHALL NOT be required to sum exactly, and the interface SHALL say why rather than let a host discover the gap and read it as a defect. Two things make it inexact and both are deliberate: the document-wide figure includes container overhead that belongs to no single layer, and content SHARED by instance layers is counted ONCE document-wide while each instance reports it in full. Reporting a shared payload once per instance would tell a host to release memory that was never allocated; reporting zero for an instance would tell it the layer is free.

Requesting a report for a layer that does not exist SHALL be an error and SHALL NOT return a zeroed report, which a host would read as an empty layer.

#### Scenario: A heavy layer is identifiable
- **WHEN** two voxel layers hold different amounts of content and each is reported
- **THEN** the layer holding more content reports the larger voxel content figure

#### Scenario: The layers account for all the content
- **WHEN** every layer of a document is reported and the per-layer content figures are summed
- **THEN** the sum equals the document-wide content figures exactly

#### Scenario: Shared instance content is counted once for the document
- **WHEN** a layer is instanced, so that two layers share one edit list
- **THEN** the document-wide edit-list figure counts the shared content once, and each layer reports it in full

#### Scenario: An unknown layer is an error
- **WHEN** a report is requested for a layer id that does not exist
- **THEN** the call fails rather than reporting an empty layer

### Requirement: Transient memory is reported separately from resident memory
Memory held only for the duration of an in-flight operation SHALL be reported as its own figure rather than folded into the subsystem it belongs to.

A mask copies its storage when a recorded step opens, so a mask costs roughly double for the duration of that step. A figure that is about to fall on its own SHALL NOT be indistinguishable from one that will still be held afterwards.

The interface SHALL state where this figure can and cannot be observed, rather than describe a behaviour a caller cannot reach. Through the C ABI it is always zero, because every mask entry point opens its step and closes it before returning and calls on one document must be serialized — so no caller can hold a handle while a step is open. The field SHALL still be reported, so that the total remains the sum of the reported fields if an entry point spanning a step is ever added; a total that silently gained bytes belonging to no reported field would be worse than a field that reads zero.

#### Scenario: A snapshot taken during a step is visible and then released
- **WHEN** memory is reported by an embedder that holds a recorded mask step open across two edits, and again after the step closes
- **THEN** the transient figure is non-zero while the step is open and zero after it

#### Scenario: The C ABI reports no transient memory
- **WHEN** a document is asked for its memory after any sequence of mask edits through the C entry points
- **THEN** the transient figure is zero, because no step can be open at that moment

### Requirement: An append reuses what an edit did not change
A document that is edited by appending an item SHALL rebuild its remembered tape by reusing the part the append did not touch, rather than recompiling the whole document. Appending is how a stroke works — one node per brush stamp — and recompiling the whole document per stamp makes each dab cost more the longer the sculpt has been worked on, on the path where the host is already waiting to place the next one.

Mutating a document while another thread reads it was never supported and still is not; this concerns readers racing each other, and the rebuild one of them triggers.

This SHALL NOT weaken any promise the remembered tape already makes. Every mutation SHALL still be visible to the next read; where the ABI cannot establish that an edit was an append — including undo, redo, event replay, and any layer or document change applied outside the command vocabulary — it SHALL invalidate and recompile in full, as it does today. A reader SHALL still receive a snapshot that stays valid for its whole call, so a concurrent append cannot pull the tape out from under it.

#### Scenario: A stroke's cost per dab stops growing with the document
- **WHEN** items are appended one at a time to a large document and the field is read after each
- **THEN** every read returns what a fresh compile would have returned, and the work of rebuilding the tape is proportional to the appended item rather than to the whole document

#### Scenario: An append is still visible to the next read
- **WHEN** the field is read, an item is appended, and the field is read again
- **THEN** the second read reflects the appended item

#### Scenario: Undo after an append is exact
- **WHEN** an appended item is undone and the field is read
- **THEN** it reads exactly as it did before the append, and redoing restores it again

#### Scenario: Concurrent readers are unaffected by prefix reuse
- **WHEN** several threads evaluate and pick against one document whose remembered tape is stale from appends not yet consumed
- **THEN** every reader gets the answer a single-threaded reader would, the rebuild happens once however many readers race for it, and no reader observes a partially rebuilt tape

### Requirement: An edit only invalidates the bricks it can reach
An edit that is not an append SHALL NOT discard a brick's kept value when the edit cannot change what that brick evaluates to. A kept value is the value of that brick's CULLED tape, and an item whose influence misses the brick's cull region is dropped from that tape — so editing it leaves the brick's answer exactly as it was, and the value SHALL be carried forward to the new revision rather than recomputed.

The region an edit reaches SHALL be taken on BOTH sides of it and unioned. One side is not an answer: an item being added is not there beforehand, one being removed is not there afterwards, and one being moved has two ends.

An edit whose region is EMPTY — one that cannot change what the document evaluates to, such as a rename — SHALL keep every value. One whose region is unbounded SHALL discard them all.

An edit whose reach is NOT known SHALL discard everything, and that SHALL remain the default. An entry point that does not positively know what it changed must land there, exactly as it must for an append.

A kept value already at the current revision SHALL be served as it is, since nothing remains to fold into it.

#### Scenario: An edit outside every brick read costs nothing
- **GIVEN** bricks refilled once, and an item edited that lies outside all of their cull regions
- **WHEN** those bricks are refilled again
- **THEN** the values equal a full refill's, and what the refill costs is set by the edit rather than by the length of the edit list

#### Scenario: An edit the bricks do reach is recomputed
- **WHEN** an item within the bricks' cull regions is removed
- **THEN** those bricks are evaluated again and their values equal a full refill's

#### Scenario: Refilling twice with no edit between costs nothing
- **WHEN** the same bricks are refilled twice and the document did not change
- **THEN** the second refill returns the values the first produced

### Requirement: A layer's bounds answer from whichever representation it holds
`clay_layer_bounds` SHALL report the tight world-space extent of a layer's content whatever representation that content is, and SHALL NOT report the absence of bounds for a layer that holds material. A mesh layer answers from its vertex positions and a voxel layer from its occupied cells, as an SDF layer answers from its shapes.

The reason this is a requirement rather than a convenience is that the alternative is not a conservative answer, it is a WRONG one: a mesh's vertices are a box and a grid's occupied cells are a box, so "this layer is nowhere" is false for either whenever it holds anything, and a host cannot tell that answer apart from an empty layer's.

A voxel layer's extent SHALL treat a cell as the BOX it is rather than as a point, so the far corner covers the whole of the last occupied cell. A single occupied cell therefore has the extent of one cell rather than none, which is what stops a one-cell grid reporting an empty box and reading as nowhere again.

Every representation SHALL answer in WORLD space, composing the layer transform, since a caller comparing two layers, framing a camera or placing a manipulator is asking one question and would otherwise be answered in two different spaces.

A layer holding NO material SHALL still report no bounds. An empty grid is genuinely nowhere, and that is a different answer from a representation that cannot say.

The composition SHALL live where the document that owns every representation is in scope, and SHALL NOT be obtained by giving `clay::scene` sight of the voxel or mesh modules. The layering rule that withholds them is what makes "this content does not change what the document evaluates to" structural, and a bounds query is not a reason to weaken it.

#### Scenario: A mesh layer reports the mesh's own box
- **GIVEN** a mesh attached as a layer
- **WHEN** the layer's bounds and the mesh's own bounds are both read
- **THEN** the two are the same box, and the layer reports that it has bounds

#### Scenario: A voxel layer follows its occupied cells
- **GIVEN** a voxel layer with a single occupied cell at the origin
- **WHEN** its layer bounds are read
- **THEN** they span that one cell rather than collapsing to a point
- **AND** occupying a second, distant cell grows the box to cover both whole cells

#### Scenario: An empty layer is still nowhere
- **GIVEN** a voxel layer with no occupied cells
- **WHEN** its layer bounds are read
- **THEN** it reports no bounds, as an SDF layer holding no shapes does

#### Scenario: Moving a layer moves its bounds
- **GIVEN** a mesh or voxel layer with a non-identity layer transform
- **WHEN** its layer bounds are read
- **THEN** they are the content's extent under that transform, as an SDF layer's are

#### Scenario: A mesh layer's bounds are a region the mesh rasterizer accepts
- **GIVEN** a mesh layer in a document
- **WHEN** its layer bounds are passed as the region to the mesh-to-voxel rasterization
- **THEN** the call is accepted and rasterizes the geometry, rather than refusing for want of a region

### Requirement: An influence bound covers every place the node is compiled
A bound reported for a NODE SHALL cover every place that node can change the field, which is once per layer sharing its content and not only the layer the caller named. Instancing a layer shares one edit list between layers with different transforms, so a single node is compiled once per instancing layer and an edit moves every copy.

The bound a host DIRTIES BY and the bound a host is TOLD SHALL be the same union, since a host that dirties by what it was told and is left with stale geometry has no way to discover the disagreement.

The per-layer bound SHALL remain available and unchanged for the compiler, which compiles one tape per layer and wants the box for the layer it is compiling.

#### Scenario: Moving a node changes nothing outside its declared box
- **GIVEN** any document and any visible item in it
- **WHEN** the item is moved and the band-clamped field is compared before and after
- **THEN** every point outside the union of the bound before and the bound after, dilated by the band and the chain pad, evaluates to exactly what it did

#### Scenario: An instanced layer's other copy is inside the box
- **GIVEN** a layer instanced into a second layer at a different transform
- **WHEN** a node of the shared content is moved
- **THEN** the second layer's copy lies inside the declared box, so a host dirtying by it refills that copy too

#### Scenario: A document with no instancing is unaffected
- **GIVEN** a document in which no layer shares content with another
- **WHEN** a node's influence bound is read
- **THEN** it is the bound the single layer reports

### Requirement: A transform's arrays are required, and a missing one is named
Every entry point taking a transform as position, rotation axis, angle and scale SHALL require BOTH arrays and SHALL refuse a null one with `CLAY_ERROR_INVALID_ARGUMENT`, leaving the document unchanged. A null rotation axis SHALL NOT be read as "no rotation": these calls take the whole transform rather than a partial update, so a null that meant identity would also silently decide the fate of the position beside it, and the caller who passed it wanted the position applied.

The refusal SHALL name WHICH argument was missing. A message covering both equally tells a caller that one of two was null and leaves them to guess, and the refusal is the only thing standing between a host and an edit it believes landed.

The axis refusal SHALL name what to pass instead, since a caller reaches it wanting no rotation and the signature already expresses that as any non-zero axis with an angle of 0 — which is what the readback answers for an unrotated node, so the round trip closes.

#### Scenario: A null rotation axis is refused and nothing moves
- **WHEN** a node's transform is set with a position and a null rotation axis
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the node's placement reads back exactly as it did before, position included

#### Scenario: The message names the missing argument
- **WHEN** the same call is made once with a null axis and once with a null position
- **THEN** the two diagnostics differ, and each names the argument that was missing

#### Scenario: No rotation is said with an axis and a zero angle
- **WHEN** the refused call is repeated with any non-zero axis and `rotation_angle` 0
- **THEN** it succeeds, the node moves to the position given, and the rotation reads back as 0

#### Scenario: One rule across every transform entry point
- **WHEN** a null array is passed to the node transform, the per-axis node transform, the layer transform or either mesh transform
- **THEN** each refuses on the same terms, so a host cannot learn the rule from one call and be caught by the next

### Requirement: a resumed refill does not hold the document cache lock while it evaluates

`clay_brick_cache_eval_requests` SHALL NOT hold the document's cache mutex while
it compiles a brick's suffix or evaluates it. The mutex SHALL be held only to
resolve the resume plans and read the seeds, and again to store what the bricks
reached.

A seed SHALL be COPIED out of the seed store while the mutex is held. No pointer
into the seed store may be read after it is released: the store is a hash map
another refill on another thread may be writing to, and the entry a raw pointer
names can be rewritten or evicted under it.

Before a brick's answer is kept as its next seed, the document's revision SHALL
be re-checked against the one the plan was made at — the same check the full
path's store makes — and the answer discarded as a seed, but still returned to
the caller, when it has moved.

What a refill returns SHALL NOT depend on any of this: a resumed brick is
bit-identical to what the full walk gives, whether or not other threads were
refilling or reading the same document at the time.

#### Scenario: a refill racing readers

- **GIVEN** a document with a stored seed for every brick of a window
- **AND** one item is appended to the active layer
- **WHEN** several threads refill that window while several others evaluate
  points against the same document
- **THEN** every refill is bit-identical to the same window refilled from a
  document holding the same items that never resumed anything
- **AND** every point evaluation agrees with the same document read alone

### Requirement: a brick proven uniform is classified without a walk

`clay_brick_cache_eval_requests` and `clay_brick_cache_eval_requests_device`
MAY answer a brick without evaluating its lattice when one evaluation of the
brick's own culled tape at the lattice centre, together with that tape's
declared Lipschitz bound, proves every sample beyond the band with the centre's
sign. Only the whole-document evaluation may be so gated; the per-layer halves
a multi-layer refill evaluates SHALL always be walked.

What `clay_brick_cache_submit` stores for a gated brick — its state, and its
uniform colour, read from sample dim^3/2 — SHALL be bit-identical to what it
would have stored from the walked samples. The values written to a gated
brick's slot SHALL every one lie beyond the band with the brick's sign and
carry the field's own colour at sample dim^3/2; they are otherwise a stand-in,
and the entry point documents them as such.

A gated brick SHALL NOT store its stand-in values as a seed. It SHALL store the
proof in the seed's place, and a later refill SHALL either carry that proof
through the appended items — folding them onto the stored centre and colour
sample values with the walk's own arithmetic, and re-proving under a bound that
is exact for what was appended — or take the full path. A refill that resumes
from a proof SHALL produce, after submit, the same stored brick as a refill of
the same document from scratch.

#### Scenario: a fill with and without the gate stores the same bricks
- **GIVEN** two documents holding the same worked, coloured sculpt
- **WHEN** every brick of the model is refilled and submitted for each, one with the gate disabled
- **THEN** every brick's state, stored halves and stored colours are identical between the two caches
- **AND** the gated document proved at least half of the uniform bricks and no surface brick

#### Scenario: a dab after a proof
- **GIVEN** a window filled with the gate, some of whose bricks were proven uniform
- **WHEN** an item that reaches those bricks but leaves them uniform is appended and the window is refilled
- **THEN** every brick of the window resumes, none walks
- **AND** the submitted cache equals one filled from scratch on a document holding the same items, with the gate enabled or disabled

#### Scenario: a carve that reaches a proven brick
- **GIVEN** the same window, warm
- **WHEN** a subtracted item brings the surface into bricks that were proven uniform
- **THEN** those bricks take the full path
- **AND** the submitted cache equals one filled from scratch

#### Scenario: a multi-layer refill is never gated
- **GIVEN** a document with two visible SDF layers
- **WHEN** a window is refilled
- **THEN** no brick is proven, and the cache equals one filled with the gate disabled

#### Scenario: no data race under a sanitizer

- **GIVEN** the concurrent refill above
- **WHEN** it is run under ThreadSanitizer
- **THEN** no data race is reported

#### Scenario: a seed the store no longer holds

- **GIVEN** two threads refilling one document at once
- **WHEN** one stores a brick's new value while the other is evaluating from
  that brick's seed
- **THEN** the evaluating thread reads its own copy of the seed and is
  unaffected

### Requirement: the deferred phase is parallel only when it is worth a dispatch

The compile-and-evaluate phase SHALL run over the shared thread pool
(`clay::parallel::ThreadPool`) only when the work it would spread exceeds the
pool's dispatch cost, measured as a count of samples times suffix length summed
over the bricks. Below that threshold it SHALL run on the calling thread — off
the lock either way.

Whether the pool is used SHALL NOT change what a refill answers.

#### Scenario: a small window

- **GIVEN** a refill of a few bricks whose suffix is one appended item
- **WHEN** the deferred phase runs
- **THEN** it runs on the calling thread, without a pool dispatch
- **AND** the values are the ones the pooled path would give

#### Scenario: a large window

- **GIVEN** a refill of a window large enough, or a suffix long enough, that the
  deferred work exceeds the threshold
- **WHEN** the deferred phase runs
- **THEN** it is spread over the thread pool
- **AND** the values are the ones the serial path would give

### Requirement: The seed store's eviction order describes the store
The structure that orders kept brick values for eviction SHALL hold exactly one
entry per stored value: none for a value that has been discarded, and never more
than one for the same brick.

Discarding a value — by eviction, or by an edit whose region reaches that
brick — SHALL remove its place in the order at the same time. Storing a brick
that already has a value SHALL NOT add a second place for it.

This matters because the order's own memory is NOT counted against the store's
byte budget: places left behind by a discarded value grow outside the ceiling
the budget describes, without bound, for as long as edits keep arriving.

#### Scenario: Repeated region-invalidating edits leave nothing behind
- **GIVEN** bricks refilled, then an item moved that reaches every one of them, repeatedly
- **WHEN** the store is asked how many places its eviction order holds
- **THEN** the count equals the number of values the store holds, after every cycle

#### Scenario: A brick discarded and stored again occupies one place
- **GIVEN** bricks whose values an edit discarded, and which a later refill stores again
- **THEN** the eviction order holds one place per brick, not one per time it was stored

### Requirement: A kept brick value is evicted by last use, not by first storage
When the store is over its byte budget it SHALL discard the LEAST RECENTLY USED
value first. A value is used when it is read to answer a refill and when it is
written by one; either SHALL make it the most recently used.

Ordering by first storage is not equivalent and is wrong for the access pattern
the store exists to serve: a stroke stores its working set at the first dab and
rewrites it at every dab after, so a first-storage order discards precisely the
bricks the next dab is about to ask for while keeping ground the brush crossed
once and left.

The most recently used value SHALL NOT be discarded, so that a budget smaller
than a single brick does not discard what was just stored.

#### Scenario: The rewritten brick survives and the abandoned one does not
- **GIVEN** a hot brick stored FIRST and a cold brick stored after it, a budget with room for both, and a stroke that rewrites only the hot one
- **WHEN** a third brick is stored and the store goes over budget
- **THEN** the cold brick's value is discarded and the hot brick's is still there to be resumed from

### Requirement: The seed store's byte budget bounds what it has allocated
The bytes the store reports and evicts against SHALL count the memory its
entries HOLD, not the memory they are currently using. A refill that carries no
colour empties an entry's colour buffer without releasing it, so a
usage-based count would report memory the store still holds as free and make the
ceiling optimistic by an amount no host can see.

The ceiling has one carve-out, which is the floor the requirement above places
on eviction: the single most recently used value is kept even when it alone
exceeds the budget. A budget with room for less than one value therefore reports
one entry and that entry's bytes, above the budget, rather than an empty store.

#### Scenario: The reported bytes stay at or under the budget
- **GIVEN** a budget with room for at least one stored value
- **WHEN** more bricks are stored than that budget has room for
- **THEN** the bytes reported by `clay_document_resume_stats` are at or under the budget it reports, and the entry count is what fits

#### Scenario: A budget below one value keeps that value anyway
- **GIVEN** a budget smaller than what one brick's value costs
- **WHEN** bricks are stored
- **THEN** the store holds exactly one entry — the most recently used — and reports its bytes, which are above the budget

### Requirement: The seed store's budget is not part of the C ABI
The store's byte budget SHALL NOT be host-settable, and the size of its eviction
order SHALL NOT be reported to hosts. A kept value is a pure performance cache —
discarding every one of them changes no geometry — and a store whose order size
differed from its value count would be describing its own defect rather than a
state a host could act on.

Reaching either from a test SHALL be through an internal header that is not
installed and carries no version guarantee, so that changing or removing it is
not an ABI break.

#### Scenario: The public descriptor is unchanged
- **WHEN** a host reads `clay_resume_stats`
- **THEN** it finds the same fields at the same offsets as before, with `budget` reporting the budget in force

### Requirement: A gesture invalidates once, for the region it states

An entry point that applies many commands as one gesture MAY state the region
those commands can reach and invalidate once for all of them, instead of having
a region derived per command.

A stated region SHALL cover everything the gesture changes. An entry point that
cannot state its reach SHALL keep the derived per-command region, which is
always correct.

The invalidation SHALL happen even when the gesture fails part way, because a
gesture that applied some of its commands has still changed the document.

`clay_layer_move_surface` SHALL state the drag's own region: the ball of the
drag radius about its centre, dilated by the displacement. Outside that ball the
warp's weight is zero, so no sample can evaluate differently.

#### Scenario: A drag across bricks the cache holds
- **GIVEN** a document whose bricks have been filled once
- **WHEN** a drag moves the surface those bricks read
- **THEN** a refill returns exactly what a full evaluation of the dragged document returns

#### Scenario: A drag repeated
- **GIVEN** a drag whose refill has seeded the bricks again
- **WHEN** the same drag is applied a second time
- **THEN** a refill still returns what a full evaluation returns

#### Scenario: A drag the bricks cannot reach
- **WHEN** a drag moves an item no brick under consideration reads
- **THEN** those bricks return what they returned before

#### Scenario: A gesture that fails part way
- **WHEN** a gesture applies some commands and then refuses
- **THEN** the region it stated is still invalidated

### Requirement: A layer's payload is reachable by layer id
The C API SHALL provide an id-addressed accessor for each representation that carries a payload beside the document: one that borrows a voxel layer's grid and one that borrows a mesh layer's geometry, each taking the layer id and no name.

This is a requirement rather than a convenience because the ABI already tells a host to hold the id — the rename call states that ids are stable across a save and load while names are not a key anything enforces — and for these two representations that advice could not be followed: the only route back to the payload of a reopened document's layer keyed on the NAME. The consequence is silent rather than loud. Two layers sharing a name shadow one another, the by-name lookup SUCCEEDS on the first in stack order, and an edit lands on the wrong layer with no error for the host to check. A host's only defence was to invent a uniqueness rule for one representation that the create calls have never asked for.

The by-name lookups SHALL remain and SHALL keep their behaviour, since they answer the question they are asked and a document with one layer of a given name is the ordinary case.

The addition SHALL be purely additive: no existing signature changes, no existing call's meaning moves, and no document format version moves, since layer ids are already stable across a save and reload.

#### Scenario: Two layers sharing a name are told apart by id
- **GIVEN** a document with two voxel layers carrying the same name and different cells, and two mesh layers carrying the same name and different geometry
- **WHEN** each layer's payload is fetched by its own id
- **THEN** each fetch reaches that layer's own payload
- **AND** the by-name lookup reaches only the first of each pair in stack order

#### Scenario: An id still reaches the payload after a save and reload
- **WHEN** a document holding two same-named voxel layers is saved and loaded into a fresh document, and each id is fetched again
- **THEN** each id reaches the same payload it reached before the round trip

#### Scenario: A rename does not move what an id reaches
- **WHEN** a layer is renamed, including onto a name another layer already carries
- **THEN** its id reaches the same payload it reached before the rename

### Requirement: An id-addressed payload lookup refuses what it cannot answer
The id-addressed accessors SHALL return the not-found refusal when no layer carries the id, when the layer carrying it is of another representation, and when the layer is of the right representation but has no payload entry.

The layer SHALL be resolved in the DOCUMENT first, not in the payload table. A payload deliberately outlives its layer: undoing the creation of a voxel layer removes the layer and KEEPS the grid beside the document, so that a redo brings the layer back with its cells. An accessor that resolved the id in the payload table alone would therefore hand back a grid whose layer is currently undone — a state the by-name lookup reports as not found, and reaching it would be a new hole rather than a new capability. Whatever the by-name lookup refuses for a given layer, the by-id lookup SHALL refuse.

The output pointer SHALL be REQUIRED, and a null one SHALL be refused as an invalid argument. This deliberately differs from the by-name lookups, where a null output is meaningful because the call doubles as an existence probe and still reports the resolved id. Here the caller supplied the id and the borrowed handle is the only answer the call has, so a null output asks nothing; the layer-info query is the call that answers whether a layer exists and what representation it is. A null document SHALL be refused the same way, and a refused call SHALL write nothing through the output pointer.

#### Scenario: An id of the wrong representation is not found
- **WHEN** a mesh layer's id is passed to the voxel accessor, or a voxel layer's id to the mesh accessor, or an SDF layer's id to either
- **THEN** the call returns not-found and writes nothing

#### Scenario: An unknown id is not found
- **WHEN** an id no layer carries is passed to either accessor
- **THEN** the call returns not-found and writes nothing

#### Scenario: An undone creation is not reachable by id
- **GIVEN** a voxel layer whose creation has been undone, so the layer is gone and its grid is still held beside the document
- **WHEN** its id is passed to the voxel accessor
- **THEN** the call returns not-found, as the by-name lookup does
- **AND WHEN** the creation is redone
- **THEN** the same id reaches the same grid, with the cells it held

#### Scenario: A layer whose payload is absent is not found
- **GIVEN** a loaded document carrying a voxel layer whose grid did not come with it, and one carrying a mesh layer whose geometry did not come with it
- **WHEN** each layer's id is passed to the accessor for its own representation
- **THEN** each call returns not-found rather than borrowing a payload that is not there
- **AND** the by-name lookup refuses the same layer, so the two agree

#### Scenario: The refusals are typed
- **WHEN** either accessor is called with a null document, or with a null output pointer
- **THEN** the call returns the invalid-argument refusal, and nothing is written

### Requirement: A layer can be instanced without copying its edit list
The C API SHALL offer an entry point that adds a layer SHARING an existing SDF layer's edit list, rather than copying it. The cost of the call SHALL NOT grow with the size of the source's edit list, which is the whole reason the call exists: a host duplicating a subtool today pays memory and time proportional to everything the artist has already sculpted, per copy.

The instance SHALL carry its OWN transform, name, visibility, protection, mirror and radial mode. Those SHALL start at the source's values, because the instance is a copy of the layer, and SHALL diverge freely afterwards — that is what makes ten instances of one blockout ten placements rather than ten identical layers.

An edit through EITHER layer SHALL be an edit to the shared content and SHALL be visible through both, and the dirty bounds reported for it SHALL cover every layer sharing the content, as the undo bounds contract already states.

Creation SHALL go through the same command vocabulary the other layer-creating entry points use, so that an enabled undo stack records it as ONE step and a single undo removes the instance.

Instancing an INSTANCE SHALL share the same content as the original, not chain. There is one allocation and the relation between the layers holding it is symmetric; a chain would invent a parent whose removal would then have to mean something.

A source that is not an SDF layer SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` and a message saying so, since a voxel grid and a mesh are held beside the document by layer id rather than by a shared pointer. A source id that does not exist SHALL be `CLAY_ERROR_NOT_FOUND`. A NULL or empty name SHALL be refused, matching the rename entry point: an empty name is what a cleared text field submits.

#### Scenario: An edit through the instance reaches the source
- **WHEN** a layer is instanced and an item is added through the INSTANCE's id
- **THEN** the source layer evaluates with that item too

#### Scenario: One edit list, two placements
- **WHEN** an instance is given a different layer transform from its source
- **THEN** the document evaluates the shared edit list at both placements

#### Scenario: Creating an instance is one undo step
- **WHEN** an instance is created on a document with undo enabled and one undo is performed
- **THEN** the instance is gone and the source is unchanged, and a redo brings the instance back still sharing the content

#### Scenario: The instance's own properties diverge
- **WHEN** an instance is renamed, hidden, ghosted or given a mirror
- **THEN** the source keeps the properties it had

#### Scenario: A voxel or mesh source is refused
- **WHEN** an instance is asked for with a voxel or mesh layer as the source
- **THEN** the call fails with an invalid-argument error naming the layer's representation

#### Scenario: An unknown source is not found
- **WHEN** an instance is asked for with a layer id the document does not have
- **THEN** the call fails with a not-found error

#### Scenario: An empty name is refused
- **WHEN** an instance is asked for with a NULL or empty name
- **THEN** the call fails and no layer is added

#### Scenario: An instance of an instance shares one edit list
- **WHEN** an instance is itself instanced
- **THEN** all three layers share one edit list and the document counts it once

### Requirement: A gesture that states its own reach covers every placement
An entry point that states the region it invalidates ANALYTICALLY instead of deriving it per command — the surface drag is the only one — SHALL widen that region to cover every layer sharing the edited content.

The drag's reach is a ball around the drag centre, and that ball is stated in the EDITED layer's placement. An instanced edit list is placed by every layer sharing it, so the same warp changes the field wherever those layers put the nodes, outside the ball. Left unwidened the host is told nothing about that region, its cached bricks there are advanced to the new revision while still holding pre-drag values, and it draws stale geometry with nothing to say so — the same failure the dirty-bounds contract already forbids for the per-command paths.

Only a shared edit list SHALL pay the widening; a layer nothing instances SHALL invalidate its ball alone, which is what makes the drag cheaper than the per-item union it replaced.

#### Scenario: A drag through one placement dirties the other
- **WHEN** a layer is instanced and placed elsewhere, a brick over the instance's placement is refilled, and the SOURCE layer's surface is then dragged
- **THEN** refilling that brick again yields what the document now evaluates to, not the values from before the drag

### Requirement: Reordering a shared layer keeps it shared
Reordering a layer is expressed as a remove and an add. When the layer's edit list is shared, the add SHALL name a surviving sharer as its content source, so that the pair carries a reference wherever it travels rather than only in memory.

Without it the reorder is silent in memory and wrong once serialized: the add writes the edit list inline, and a journal replay after a crash restores the layers UNLINKED and the edit list multiplied, with every shape right and nothing to see.

#### Scenario: Replaying a reorder keeps the sharing
- **WHEN** an instance is reordered on a document with undo enabled and the journal is replayed onto the snapshot it was taken against
- **THEN** the recovered layers still share one edit list, and an edit through one is visible through the other

### Requirement: An instance survives a save and load as a reference
A document holding instanced layers SHALL serialize the shared edit list ONCE and SHALL restore the sharing on load. A round trip SHALL NOT multiply the allocation the memory report promises to count once, and SHALL NOT silently unlink the layers.

The document-wide memory report for a document of N instances SHALL therefore be unchanged, within container overhead, by a save and reload — and each instance SHALL still report the content in full, exactly as it did before the round trip.

Removing the SOURCE layer while instances remain SHALL be legal: the content stays alive because the instances hold it. A surviving instance SHALL still evaluate, SHALL still save, and SHALL reload with its content intact.

#### Scenario: Ten instances reload as one allocation
- **WHEN** a document holding a source layer and nine instances is saved and reloaded
- **THEN** the document-wide edit-list figure is what it was before the round trip, and does not scale with the instance count

#### Scenario: The share survives the round trip
- **WHEN** a document with an instance is saved, reloaded, and an item is added through one of the two layers
- **THEN** the other layer evaluates with that item too

#### Scenario: An orphaned instance keeps its content
- **WHEN** the source layer is removed and the document is saved and reloaded
- **THEN** the surviving instance still evaluates to the same field

### Requirement: A host can see that a layer is an instance
The layer information descriptor SHALL report which layer a given layer takes its content from, and how many layers share that content. Without both, a host cannot draw the link: the id alone marks the following end of it and leaves the SOURCE indistinguishable from an ordinary layer.

The reported source SHALL be the FIRST layer in stack order holding that content, and a layer that IS that first layer SHALL report 0. This is the same rule the writer uses to decide which layer owns the content in the file, so what a host is told is what a save would write, and the answer SHALL be unchanged by a save and reload.

It follows that removing a source layer SHALL NOT leave a dangling reference: the first surviving sharer becomes the owner and reports 0, and any others report its id.

These fields SHALL be APPENDED to the existing descriptor, so a host compiled against the older layout declares a shorter `struct_size` and simply does not receive them.

#### Scenario: An instance names its source
- **WHEN** the layer information is read for an instanced layer
- **THEN** the reported content source is the source layer's id and the reported share count is 2

#### Scenario: A source reports no source of its own
- **WHEN** the layer information is read for the layer that was instanced
- **THEN** the reported content source is 0 and the reported share count is 2

#### Scenario: An ordinary layer shares with nobody
- **WHEN** the layer information is read for a layer nothing instances
- **THEN** the reported content source is 0 and the reported share count is 1

#### Scenario: The link survives a reload
- **WHEN** a document with an instance is saved and reloaded and the layer information is read again
- **THEN** the same source and the same share count are reported

#### Scenario: Removing the source re-homes the link
- **WHEN** the source layer of two instances is removed
- **THEN** the first surviving instance reports 0 and the second reports the first's id

### Requirement: Consolidating an instance severs it
Consolidating a layer whose edit list is shared SHALL give that layer a PRIVATE copy of the content before baking, so the bake replaces that layer's edit list alone and every other layer sharing the content is untouched.

Baking in place would rewrite the edit list of every instance, turning nine subtools into volumes because the artist baked the tenth. That is not a reading of "this shape is finished" anyone asks for, and it is silent.

The sever SHALL be part of the SAME undo step as the bake, so that one undo restores the layer with its original shared content and the link intact. A host SHALL be able to observe that the layers stopped sharing, through the layer information descriptor.

Measuring what a consolidation would cost SHALL NOT sever anything, since it changes nothing.

#### Scenario: Baking one instance leaves the other parametric
- **WHEN** a layer with two items is instanced and the instance is consolidated
- **THEN** the instance holds a single baked item and the source still holds its two

#### Scenario: A severed instance stops following the source
- **WHEN** an item is added to the source after its instance was consolidated
- **THEN** the consolidated layer is unchanged

#### Scenario: Undoing a bake restores the share
- **WHEN** an instance is consolidated and the consolidation is undone
- **THEN** the layer holds its original items again and shares them with the source

#### Scenario: Measuring a consolidation changes nothing
- **WHEN** the cost of consolidating an instance is read
- **THEN** the layers still share one edit list

### Requirement: A drag under symmetry reaches every image and keeps its history
`clay_layer_move_surface` SHALL state its reach as one box per image the layer's symmetry makes of the drag — the ball, one reflection per mirror axis, one rotation per radial copy — since the copies a mirrored or radial layer emits move where the images are. The boxes SHALL be taken as one invalidation, one revision, under one lock, and SHALL NOT be replaced by their union: the union of two balls a diameter apart is the slab between them, which under a mirror is the whole document.

Under symmetry the drag SHALL state the frontier of the items it actually reaches, so a mirrored drag on late-history items keeps the prefix seeds it would keep unmirrored. A drag whose image does reach root ordinal 0 still takes the legacy drop, by design.

`*out_applied` and the preview SHALL count ITEMS: an item that both the ball and an image reach takes its grabs in one command and is reported once.

A live Move transaction SHALL report every grab the last update resolved for an affected node — `clay_sdf_move_preview_grab_count` gives how many, one without symmetry and one per image of the drag that reaches the node with it, and `clay_sdf_move_preview_grab` gives the grab at an index, refusing an index past the count — since a host that drew the first grab alone would preview half of a straddler's drag.

#### Scenario: The reflected side is not stale
- **GIVEN** a mirrored layer and bricks warm on the side the ball's reflection covers
- **WHEN** a drag is applied on the other side and those bricks are refilled
- **THEN** the values match a fresh mirrored document's cold refill bit for bit, and differ from the undragged document's

#### Scenario: A mirrored drag resumes
- **GIVEN** a mirrored layer whose dragged items were appended last, and a warm window of bricks over them
- **WHEN** a continuing drag is applied frame after frame
- **THEN** the frontier probe reports the dragged items' own root ordinal, the window resumes exactly as the unmirrored layer's does, and the refill matches a fresh mirrored oracle bit for bit

#### Scenario: A transaction reports one grab per reaching image
- **WHEN** a live Move on an unmirrored layer is asked for an affected node's grabs
- **THEN** the count is one and index 1 is refused, while a node the drag does not reach is not found

#### Scenario: A straddling item counts once
- **WHEN** a mirrored drag's ball and reflection both reach one item
- **THEN** the preview names it once and `*out_applied` counts it once, and the two agree

### Requirement: A host can sculpt an adaptive surface across the ABI
The C ABI SHALL expose the adaptive surface and its sculptor as opaque handles, with versioned descriptors for the surface, the topology policy and the stamp report, following the established `struct_size` pattern with bounded output fills.

`clay_mesh_sculptor` SHALL keep its semantics unchanged. A host compiled against the current header relies on stable vertex and index counts and on borrowed buffers; adaptive topology SHALL NOT reach it.

The ABI SHALL report topology, geometry and attribute revisions separately, and SHALL expose the changed partitions of a stroke with caller-owned buffers and a capacity query rather than copying the whole surface per stamp.

Long operations — construction, global remesh, conversion, serialization — SHALL accept the cancellation token, and a cancelled operation SHALL leave the surface byte-identical.

#### Scenario: The fixed sculptor is unchanged
- **WHEN** a host built against the previous header calls the fixed mesh sculptor after this change
- **THEN** it behaves identically, and no adaptive behaviour reaches it

#### Scenario: A stroke updates only what changed
- **WHEN** a host drives a stroke and drains the changed partitions each frame
- **THEN** the bytes it copies follow the changed partitions rather than the size of the surface

#### Scenario: A cancelled build changes nothing
- **WHEN** a long adaptive operation is cancelled through the token
- **THEN** the call reports cancellation and the surface is byte-identical to before it started

### Requirement: A host can drive a hierarchy across the ABI
The C ABI SHALL expose the multiresolution surface as an opaque handle with level creation and removal, independent sculpt and display levels, sculpting at the active level, and export of any level as a mesh.

Adding a level SHALL report its predicted cost and SHALL fail with a typed budget error rather than allocating part of it.

The ABI SHALL report revisions for the base, the detail and the evaluated surface separately, and SHALL expose changed blocks with caller-owned buffers and a capacity query rather than copying a display-level mesh per stamp.

Descriptors SHALL follow the established `struct_size` pattern with bounded output fills, and long operations SHALL accept the cancellation token.

#### Scenario: A detail stamp does not copy the display mesh
- **WHEN** a host stamps detail on a deep hierarchy and drains the changed blocks
- **THEN** the bytes copied follow the changed blocks rather than the display level's size

#### Scenario: An over-budget level is refused across the ABI
- **WHEN** a host requests a level whose predicted cost exceeds the budget it declared
- **THEN** the call returns a typed budget error and the surface is unchanged

### Requirement: A host can carry a brush preset across the ABI
The C ABI SHALL expose the brush preset — the stroke preset it contains and the model axes the mesh path already honours — through versioned descriptors following the established `struct_size` pattern, with bounded output fills.

Serialization SHALL cross as bytes rather than as a path, matching every other format the library writes, so a host holding a preset library in its own container never writes a temporary file.

Image content SHALL remain borrowed for the duration of a call. The ABI SHALL NOT take ownership of alpha or displacement samples, and SHALL NOT copy them into a preset.

Existing mesh brush entry points SHALL keep their semantics unchanged. A host compiled against the current header SHALL build and behave identically after this change.

#### Scenario: A preset crosses and comes back
- **WHEN** a preset is serialized through the ABI, deserialized, and used to resolve a stroke
- **THEN** the resolved stamps equal those from the original preset

#### Scenario: An older descriptor is honoured
- **WHEN** a host passes a descriptor whose `struct_size` predates a field added later
- **THEN** the call succeeds using defaults for the fields it does not carry, and writes no byte past the size the caller declared

### Requirement: Voxel remesh over the C ABI
The C ABI SHALL expose the global voxel remesh and its preflight estimate, taking a versioned `struct_size`-prefixed parameter descriptor and filling versioned `struct_size`-prefixed estimate and report descriptors.

Every descriptor the library FILLS SHALL be written bounded by the size the caller declared, never by the size this build compiled, so a caller built against an older header is not written past. A defaults accessor SHALL be provided for the parameter descriptor so a caller can obtain the library's documented defaults without transcribing them.

The remesh SHALL accept the ABI's existing cancellation token, so a host can drive it from a worker thread and stop it, and SHALL report a cancelled call as a distinct result code rather than as a generic failure.

Failure SHALL be distinguishable by kind: an invalid resolution, a request over budget, an open surface refused by policy, a result that failed its own validation and a cancellation SHALL NOT collapse into one code.

#### Scenario: An older caller is not written past
- **WHEN** a caller declares an estimate or report descriptor shorter than this build's
- **THEN** only the bytes the caller declared are written, and the fields the caller does know are correct

#### Scenario: Defaults are obtainable, not transcribed
- **WHEN** a caller asks for the voxel remesh parameter defaults
- **THEN** it receives a filled descriptor whose values match the C++ defaults, and a remesh with it behaves as a remesh with the C++ defaults

#### Scenario: Failure kinds stay distinct
- **WHEN** a remesh is refused for an invalid resolution, for exceeding a budget, and for an open surface under a rejecting policy
- **THEN** the three calls return three different result codes

#### Scenario: A cancelled remesh is reported as cancelled
- **WHEN** a remesh is cancelled through the ABI's cancellation token
- **THEN** the call returns the cancelled result code and produces no mesh

### Requirement: Spatial scalar transfer over the C ABI
The C ABI SHALL expose the spatial resampling of a caller-owned per-vertex scalar array from one mesh onto another, so a host holding a mask outside the mesh can carry it across a remesh.

The call SHALL require the caller to state the length of both the input array and the output buffer, and SHALL refuse a length that does not match the corresponding mesh's vertex count rather than reading or writing what it was not given.

#### Scenario: A mask crosses a remesh
- **WHEN** a host transfers a per-vertex mask from a source mesh onto its remeshed result
- **THEN** the output buffer holds one value per result vertex, resampled from the source by closest point

#### Scenario: A wrong length is refused
- **WHEN** the call is given an array whose length is not the source's vertex count
- **THEN** it returns an invalid-argument result and writes nothing

### Requirement: A mesh layer can be rebuilt through the document
The C ABI SHALL expose a rebuild that targets a mesh LAYER: capture, rebuild, validate, replace and record, as one call and one undo step, taking the same versioned parameter descriptor and filling the same versioned report as the pure mesh-to-mesh form.

It SHALL be transactional. Nothing is written until the rebuild has succeeded and validated, so a refusal, a validation failure or a cancellation leaves the layer byte-identical and adds no undo step.

A protected layer SHALL be refused BEFORE the rebuild rather than after it: rebuilding a locked layer for several seconds and then declining to commit is a worse answer than declining immediately.

The ABI SHALL also expose the layer's geometry revision and a revision-checked replacement, so a host that ran the pure rebuild on its own worker thread can commit it without overwriting newer work. A stale commit SHALL be refused with a result code distinct from the codes for a bad argument and a missing layer.

#### Scenario: One call, one undo step
- **WHEN** a host rebuilds a mesh layer through the document with undo enabled
- **THEN** the layer holds the rebuilt triangles, the report describes them, and the undo depth grew by exactly one

#### Scenario: A cancelled rebuild leaves the layer alone
- **WHEN** a rebuild through the document is cancelled
- **THEN** it returns the cancelled result code, the layer's triangles are unchanged, and no undo step was added

#### Scenario: A stale commit is refused distinctly
- **WHEN** a host commits a rebuild at a revision the layer has moved past
- **THEN** the commit returns a result code distinct from an invalid argument and from a missing layer, and the layer keeps the newer geometry

### Requirement: Welding is reachable over the C ABI
The C ABI SHALL expose the weld through a versioned descriptor with a defaults accessor, filling a versioned report bounded by the size the caller declared.

Welding a mesh LAYER SHALL respect the same protection every other edit does, and SHALL bump the layer's geometry revision when — and only when — it actually changed something. A weld rewrites the triangles, so it invalidates a cached adjacency, spatial index or sculpting session exactly as a rebuild does; a weld that merged nothing must not invalidate them.

#### Scenario: A weld reports and invalidates
- **WHEN** a mesh layer holding a marched mesh is welded
- **THEN** the report names the triangles collapsed and the tolerance used, and the layer's geometry revision has moved

#### Scenario: A weld that did nothing invalidates nothing
- **WHEN** the same layer is welded a second time
- **THEN** the report says nothing merged, and the geometry revision is unchanged

#### Scenario: A protected layer and a bad tolerance are refused
- **WHEN** a ghosted or locked layer is welded, or a negative tolerance is given
- **THEN** the call is refused and the mesh is unchanged

### Requirement: The alpha entry point refuses a degenerate stamp
`clay_item_add_alpha` SHALL refuse a direction with no length and a non-positive radius, returning an invalid-argument result and leaving the item unchanged, and its documentation SHALL name the space its coordinates are in.

Both inputs were previously accepted with a success result and appended a deformer that did nothing — the case the header's own note calls harder to notice than a failure.

#### Scenario: The two new refusals leave the item alone
- **WHEN** an alpha is added with a zero-length direction, and again with a non-positive radius
- **THEN** both calls return an invalid-argument result, and adding the item afterwards yields the same document as one to which the alpha was never offered

### Requirement: A magnify on the assembled surface is reachable from C
The C ABI SHALL expose the surface magnify as `clay_layer_magnify_surface`, with a `_preview` counterpart and a versioned `clay_magnify_params`, on the contract `clay_layer_move_surface` already has: it resolves against every item the region reaches, maps the gesture into each item's frame, makes the whole gesture ONE undo step, and issues ONE invalidation for it.

The strength SHALL be a separate argument rather than a field of the params struct, because a live gesture holds its region fixed and grows only the strength.

The gesture SHALL invalidate its own ball, one per image the layer's symmetry makes of it, with NO dilation — outside the radius the region weight is zero and the point is returned unchanged, for either sign of the strength — widened by each sharer's influence bound when the edit list is instanced.

A strength of zero SHALL be refused rather than accepted as a no-op: it scales by one, so it is not a gesture, and a host that reached the call by accident should hear about it at the boundary.

#### Scenario: A gesture reaches every contributing item
- **WHEN** clay_layer_magnify_surface is called over a form built from two blended items
- **THEN** it reports two items applied and the surface changes symmetrically about the gesture's centre

#### Scenario: One undo step
- **WHEN** a magnify that warps several items is applied with undo enabled
- **THEN** the undo depth grows by one, and undoing it restores the field exactly

#### Scenario: A gesture that reaches nothing succeeds
- **WHEN** the region is far from any item
- **THEN** the call returns OK, reports zero applied, and the document is unchanged

#### Scenario: Malformed gestures are refused
- **WHEN** the call is made with a null centre or params, an unknown layer, a non-positive radius, a strength of zero, or a params struct whose struct_size is stale
- **THEN** it is refused, and the preview refuses the same cases

### Requirement: The field report names the mechanism and is advised on it
`clay_field_report` SHALL carry the deformer mechanism's own factor, the count of nodes that are evaluated, and a `clay_degradation` naming which mechanism is costing the marcher. `advises_consolidation` SHALL be keyed on that mechanism rather than on the step scale alone.

The fields SHALL be appended behind `struct_size`, so a caller built against the earlier struct keeps working and nothing is written past the end of the struct it owns.

#### Scenario: An older caller is unaffected
- **WHEN** clay_layer_field_report is called with the struct_size of the earlier struct
- **THEN** it succeeds, fills the fields that struct has, and writes nothing past its end

#### Scenario: The advice follows the mechanism
- **WHEN** a degraded layer holds one evaluated item carrying a brush chain
- **THEN** `degradation` is CLAY_DEGRADATION_DEFORMERS and `advises_consolidation` is 0

### Requirement: A voxel drag is reachable from C as a transaction
The C ABI SHALL expose the voxel grab gesture as `clay_voxel_grab_begin`, `_update`, `_written_box`, `_commit`, `_cancel` and `_destroy`, on the shape `clay_sdf_move_begin` already has on the field side.

`_update` SHALL take the TOTAL displacement from the anchor. `_written_box` SHALL report the brush's footprint, fixed for the whole gesture whatever the displacement grows to, so a host has its invalidation region from the first frame. `_destroy` SHALL cancel an uncommitted transaction.

Every write SHALL raise the same undo step and dirty-region bookkeeping a stateless verb does.

`clay_voxel_sculpt_grab`'s documentation SHALL state that it does not compose and point at the transaction.

#### Scenario: A drag delivered in pieces lands where one delivered whole does
- **WHEN** the same total drag is delivered through the transaction as one, two, four and eight updates
- **THEN** the grid is identical in every case, and differs from the untouched grid

#### Scenario: The written box does not grow with the drag
- **WHEN** the box is read at the start of a gesture and after a long update
- **THEN** it is the same box

#### Scenario: A spent transaction is refused, not silently accepted
- **WHEN** update or commit is called after a commit or a cancel
- **THEN** it is refused

### Requirement: A Move drag previews as an ordinary document from C
The C ABI SHALL expose a live Move transaction's preview as a borrowed, read-only `clay_document` carrying the real document's layers with the dragged one replaced by the preview. It SHALL be usable wherever a document is — evaluation, meshing, picking, brick refill — so a host draws the drag through machinery it already has, and the real document SHALL NOT be modified.

The handle SHALL be valid until the transaction is committed, cancelled or destroyed, and SHALL be NULL for a spent transaction. It SHALL share the transaction's content, so an update is visible through it without a refresh, and the document's compiled-tape cache SHALL be invalidated on each request, since the drag changes the edit list without going through a mutating entry point.

It SHALL carry the SDF layers. Voxel grids, masks and mesh layers are not part of the field tape, so their absence changes nothing that reads the field, and copying them would charge a drag for content it cannot change.

A drag that reaches no items SHALL still preview, as the layer unchanged.

#### Scenario: The preview carries the drag and the document does not move
- **WHEN** a live drag is updated and the preview document is evaluated
- **THEN** the surface shows the drag, the real document's field is unchanged, and its saved bytes are identical

#### Scenario: A second update is visible through the same handle
- **WHEN** the transaction is updated again and the preview document evaluated
- **THEN** it shows the newer drag rather than the previous frame's

#### Scenario: The preview is what the commit writes
- **WHEN** the drag is committed
- **THEN** the real document's field matches what the preview last showed

#### Scenario: The preview dies with the gesture
- **WHEN** the transaction is committed or cancelled
- **THEN** the preview document is no longer available

### Requirement: A placed deformer can be taken back
The C ABI SHALL offer an inverse for `clay_layer_add_deformer`: a count, a remove by index, and a clear. Undo SHALL NOT be the only way to remove a warp, since undoing also spends a history entry the caller never meant to make.

All three SHALL be undoable edits. Removing at an index past the end SHALL be refused rather than silently ignored; clearing a chain that is already empty SHALL succeed.

#### Scenario: A chain can be counted, shortened and emptied
- **WHEN** two deformers are added to a placed node and one is removed by index
- **THEN** the count reports two, then one, and clearing takes it to zero

#### Scenario: An index past the end is refused
- **WHEN** remove names an index the chain does not have
- **THEN** it is refused and the chain is unchanged

#### Scenario: Removing is undoable
- **WHEN** a deformer is removed and the edit undone
- **THEN** the chain is exactly what it was

### Requirement: A region of a layer can be merged from C
The C ABI SHALL expose the region merge as `clay_layer_consolidate_region`, with `clay_layer_plan_region_merge` reporting what it would absorb without baking, and a versioned `clay_region_merge` carrying the sampled box, the number of roots absorbed and whether the closure took the whole layer.

A missing or empty region SHALL be refused rather than treated as the whole layer: a region merge without a region is a whole-layer consolidation, and a caller should ask for that by name.

#### Scenario: The merge leaves the rest parametric
- **WHEN** a region over one of four well-separated items is merged
- **THEN** one item is absorbed, a volume takes its place, the other three remain, and the layer does not report itself consolidated

#### Scenario: Repeated gestures keep one baked item
- **WHEN** the same region is merged once per gesture over several gestures
- **THEN** the layer's node count does not grow

#### Scenario: A region it cannot make sense of is refused
- **WHEN** the call is made with a null or inverted region, an unknown layer, null params, a region over empty space, or a stale struct_size
- **THEN** it is refused and the document is unchanged

### Requirement: A host can evaluate the document without one layer

The API SHALL expose point evaluation, gradient evaluation and brick-request
evaluation over **every visible SDF layer except one named layer**, so a host
previewing a single layer through a transaction can draw the rest of the
document beside it.

The excluded forms SHALL answer exactly what the whole-document forms answer for
a document from which that layer had been removed. Visible SDF layers compose by
hard union, so a caller may take the minimum of an excluded evaluation and its
own preview and obtain the field the whole document would have — this is exact
composition, not an approximation, and the API SHALL say so where it is offered.

The excluded forms SHALL take the same arguments, honour the same backend
selection, and observe the same count ceilings as the whole-document forms they
mirror. Brick-request evaluation SHALL fill the same fixed per-brick slots at
the same stride.

Naming an excluded layer the document does not hold SHALL return a not-found
error. It SHALL NOT be read as "exclude nothing": a host whose layer id went
stale would otherwise be handed the whole document, and would draw the layer it
meant to exclude on top of the preview it drew itself.

Naming a layer that is hidden, or that carries no SDF content, SHALL succeed and
answer what the visible SDF layers evaluate to — such a layer contributes
nothing to the union, so excluding it is already a no-op and refusing it would
make a host branch on state it has no reason to track.

These entry points SHALL NOT modify the document. In particular a host SHALL NOT
need to toggle layer visibility to obtain this result, because visibility is an
edit and an edit taken during a transaction is one the transaction's commit
refuses.

#### Scenario: The rest of the document, during a gesture
- **WHEN** a consumer opens a sculpt transaction on one layer of a multi-layer document and evaluates the document excluding that layer
- **THEN** it receives the field of the other visible SDF layers, the document is unchanged, no undo entry is recorded, and the transaction's commit still succeeds

#### Scenario: The excluded evaluation composes exactly
- **WHEN** a consumer takes the minimum of an excluded evaluation and the excluded layer's own evaluation at the same points
- **THEN** the result equals the whole-document evaluation at those points

#### Scenario: A stale layer id is refused
- **WHEN** a consumer excludes a layer identifier the document does not hold
- **THEN** the call returns a not-found error and writes no distances

#### Scenario: Excluding a hidden layer is not an error
- **WHEN** a consumer excludes a layer that is hidden or carries no SDF content
- **THEN** the call succeeds and answers what the visible SDF layers evaluate to

#### Scenario: A brick refill without one layer
- **WHEN** a consumer evaluates brick requests excluding one layer
- **THEN** brick i occupies the same fixed slot it occupies in the whole-document form, holding what that brick would hold in a document without that layer
