# build-packaging — the shipped artifact carries the backend it is for

Delta for `ship-metal-in-the-xcframework`.

## ADDED Requirements

### Requirement: The Apple artifact ships the Metal backend
`tools/build_xcframework.sh` SHALL build every slice with the Metal backend compiled in. The xcframework is the only distribution path for Apple hosts, and a consumer of a prebuilt static library CANNOT add a backend afterwards — the option decides which sources are compiled into the archive, so shipping it off pins every host to CPU evaluation with no recovery.

The build SHALL pass every backend option EXPLICITLY, including the ones left off. CMake caches `option()` values and these build directories survive between runs, so an inherited cache would otherwise decide what ships.

The Metal library SHALL be compiled for the SDK of the slice being built. A metallib is platform-specific: one built for macOS does not load on iOS, and a slice carrying the wrong one links cleanly, fails to register the backend at runtime, and is indistinguishable from a slice that never had it.

The build SHALL FAIL rather than emit a slice whose archive carries no Metal library. Shipping a CPU-only framework silently is the failure this requirement exists to prevent, so it must be loud at the point it is produced.

#### Scenario: Every slice carries Metal
- **WHEN** the xcframework is built
- **THEN** every slice's archive contains an embedded Metal library compiled for that slice's SDK

#### Scenario: A slice without Metal is not shipped
- **WHEN** a slice is built and no Metal library was produced for it
- **THEN** the build fails, naming the slice, and no xcframework is written

#### Scenario: A stale cache cannot decide what ships
- **WHEN** the build runs against a build directory configured differently by an earlier run
- **THEN** the backends compiled in are the ones this build asked for

### Requirement: The shipped artifact's backends are asserted, not assumed
The Swift smoke test SHALL assert which backends the linked library registers, so that a change to how the artifact is built cannot quietly change what a host gets.

This is a distinct check from the archive gate above and both SHALL exist: the archive gate covers what the build SHIPS and holds on any machine, while this covers what a host GETS and requires a device to be meaningful.

The SwiftPM package SHALL declare the frameworks the compiled-in backends require, since a `binaryTarget` cannot carry linker settings and a consumer linking the archive without them gets undefined symbols.

#### Scenario: The smoke test names the backends
- **WHEN** the Swift smoke test runs against a slice
- **THEN** it reports the registered backends and fails if the Metal backend is absent

#### Scenario: A SwiftPM consumer links what the archive needs
- **WHEN** an app consumes the package and links the xcframework
- **THEN** the frameworks the backends depend on are declared by the package rather than left for the app to discover from a linker error
