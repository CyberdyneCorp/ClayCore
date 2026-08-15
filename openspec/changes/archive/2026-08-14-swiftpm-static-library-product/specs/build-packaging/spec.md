# build-packaging — the SwiftPM product's linkage is a decision

Delta for `swiftpm-static-library-product`.

## ADDED Requirements

### Requirement: The SwiftPM library product is statically linked
The `claycore` SwiftPM library product SHALL be declared with an explicit
`type: .static` rather than left automatic.

An automatic product lets the toolchain choose, and Xcode 26 chooses a dynamic
`PackageProduct` framework in Debug builds. The product's only target carries no
code — it exists to hold the `Metal` and `Foundation` linker settings a
`binaryTarget` cannot — so nothing in it references the slice archive and the
linker pulls no objects from it. The framework then exports no `clay_*` symbol
and every consuming application fails to link, while the archive itself contains
every symbol.

A gate SHALL check this declaration in CI. Where a Swift toolchain is available
the gate SHALL read the product's type from SwiftPM itself; where one is not it
MAY read the manifest as text, and SHALL report which of the two it did, so a
passing result is not read as a stronger claim than it is.

The gate SHALL state, in its failure output, why the linkage matters — the
symptom is a wall of undefined symbols in a consuming app and points nowhere
near this manifest.

#### Scenario: An Xcode consumer links
- **WHEN** an application consumes the package on a toolchain that would build an automatic library product as a dynamic framework
- **THEN** the product is static, the archive's objects are linked into the client, and the application's `clay_*` symbols resolve

#### Scenario: The linkage cannot silently revert
- **WHEN** the explicit static declaration is removed from the manifest
- **THEN** the CI gate fails and names the product, the required declaration and the consequence

#### Scenario: The gate does not overstate what it checked
- **WHEN** the gate runs on a machine with no Swift toolchain
- **THEN** it reports that it read the manifest as text rather than through SwiftPM
