# c-abi — parity with the Python bindings

Delta for `widen-c-abi`.

## MODIFIED Requirements

### Requirement: Flat versioned C API
`bindings/c/clay.h` SHALL expose documents, layers, edit commands, evaluation, brick access, meshing, picking, and file I/O through a flat C API: opaque handles, integer error codes, caller-owned buffers, no C++ types and no exceptions crossing the boundary. The header SHALL carry an ABI version triple queryable at runtime (`clay_version()`), and the ABI SHALL follow SemVer (breaking changes only on major).

The C API SHALL reach every authoring and query capability the Python bindings reach, so that a Swift consumer is not restricted to a subset of what `pyclay` can drive. Where a Python construct has no direct C equivalent — chained modifiers, variable-length payloads — the C API SHALL provide an equivalent builder rather than omitting the capability.

#### Scenario: Pure C consumer
- **WHEN** a C11 test program includes only `clay.h` and links the library
- **THEN** it can create a document, add a sphere edit, evaluate points, mesh, and export OBJ — with every failure path returning an error code

#### Scenario: Version check
- **WHEN** a consumer compiled against header version X.Y links a library reporting major ≠ X
- **THEN** the documented init-time version check fails explicitly instead of undefined behavior

#### Scenario: Capability parity with pyclay
- **WHEN** the parity gate enumerates the engine capabilities the Python module exposes
- **THEN** each one has a corresponding C entry point, or an explicit recorded exemption naming why

## ADDED Requirements

### Requirement: Item builder for composed edits
The API SHALL provide an opaque item builder so an edit can be composed from a primitive plus modifiers before being added to a layer. The builder SHALL accept, in the order the caller applies them: transform, combine op, blend kind and radius, rounding, colour, mirror flag, a deformer chain, repetition, a profile (including polygon vertices), stroke points, and transition parameters. Modifier order SHALL be preserved, matching the Python bindings, because deformers do not commute.

The existing flat `clay_item_desc` entry point SHALL keep working, defined as sugar over the builder for edits that need no variable-length payload.

#### Scenario: Composed edit from C
- **WHEN** a C consumer builds a primitive, applies a twist then a bend, sets a radial repetition, and adds it to a layer
- **THEN** the resulting document evaluates identically to the same edit authored through `pyclay`

#### Scenario: Deformer order is preserved
- **WHEN** two items are built with twist-then-bend and bend-then-twist
- **THEN** their fields differ, in the same way they differ through the Python bindings

#### Scenario: Existing flat path still works
- **WHEN** a consumer compiled against the previous header calls `clay_add_item` with a `clay_item_desc`
- **THEN** it behaves exactly as before

### Requirement: Versioned descriptor structs
Every descriptor struct crossing the ABI SHALL carry a leading `uint32_t struct_size` set by the caller. The library SHALL read only the prefix the caller declares, so fields may be appended without a major version bump. The ABI hygiene gate SHALL fail if a public descriptor struct lacks the field.

#### Scenario: Older caller against newer library
- **WHEN** a caller sets `struct_size` to the size it was compiled against and the library has since appended fields
- **THEN** the call succeeds and the appended fields take documented defaults

#### Scenario: Gate rejects an unversioned struct
- **WHEN** a public descriptor struct is added without `struct_size`
- **THEN** the C ABI hygiene check fails naming the struct

### Requirement: Complete primitive, op and blend enumerations
`clay_prim` SHALL cover every primitive the scene model supports, including the lifts, and its values SHALL equal the corresponding tape opcodes so no translation table exists to drift. `clay_op` SHALL cover the boolean ops, paint, the eight extended combine modes and both transition morphs. `clay_blend` SHALL cover every blend profile.

#### Scenario: Enumerations stay in step with the engine
- **WHEN** a primitive, op or blend is added to the scene model without a matching C enumerator
- **THEN** a build-time or gate check fails rather than the C API silently lagging

#### Scenario: Every primitive is reachable
- **WHEN** a C consumer adds one edit of every `clay_prim` value to a document
- **THEN** each evaluates and the document meshes

### Requirement: Voxel grids across the ABI
The API SHALL expose voxel grids through an opaque handle: palette management, single-cell and batch edits, cube and sphere brushes with falloff and strength, the sculpting verbs (smooth, inflate, flatten, pinch), box and line fills, mirrored edits, flood select, occupancy and bounds queries, greedy meshing, SDF rasterization, step-field sampling, and voxel cell/face and build-plane picking.

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

### Requirement: Picking and evaluation parity
The API SHALL expose gradients, field colours, batch raycast, safe step scale, surface snapping, layer bounds and selection bounds, and raycast that attributes the hit to a layer and node.

#### Scenario: Zoom to selection from C
- **WHEN** a consumer requests the bounds of a selection of nodes
- **THEN** it receives the same AABB the Python bindings report for that selection

#### Scenario: Attributed raycast
- **WHEN** a raycast hits an item in a multi-layer document
- **THEN** the call reports the layer and node identifiers alongside the hit position and normal
