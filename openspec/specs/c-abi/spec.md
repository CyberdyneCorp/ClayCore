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

