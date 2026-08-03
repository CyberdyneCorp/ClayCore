# c-abi — Flat stable C API and Swift consumption

Delta for `add-claycore-v1`.

## ADDED Requirements

### Requirement: Flat versioned C API
`bindings/c/clay.h` SHALL expose documents, layers, edit commands, evaluation, brick access, meshing, picking, and file I/O through a flat C API: opaque handles, integer error codes, caller-owned buffers, no C++ types and no exceptions crossing the boundary. The header SHALL carry an ABI version triple queryable at runtime (`clay_version()`), and the ABI SHALL follow SemVer (breaking changes only on major).

#### Scenario: Pure C consumer
- **WHEN** a C11 test program includes only `clay.h` and links the library
- **THEN** it can create a document, add a sphere edit, evaluate points, mesh, and export OBJ — with every failure path returning an error code

#### Scenario: Version check
- **WHEN** a consumer compiled against header version X.Y links a library reporting major ≠ X
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
