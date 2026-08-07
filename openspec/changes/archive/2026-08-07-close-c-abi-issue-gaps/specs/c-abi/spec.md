# c-abi — baking a document, and a settable import budget

Delta for `close-c-abi-issue-gaps`.

## ADDED Requirements

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
