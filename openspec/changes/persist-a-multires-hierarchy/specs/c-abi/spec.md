# c-abi — a host can ask what a row is, and take what a load produced

Delta for `persist-a-multires-hierarchy`.

## ADDED Requirements

### Requirement: A host can ask whether a layer carries a hierarchy
The C ABI SHALL let a host ask whether a given layer carries a multiresolution hierarchy, and answer it from the document rather than from anything the host has to keep. Today the engine reports a hierarchy's layer as an ordinary mesh layer, so the only record that a row was ever a hierarchy is a file the host writes beside the document; losing that file loses the levels with no error anywhere.

The query SHALL be answerable on a freshly loaded document with no prior calls, SHALL NOT build, decode or materialise the hierarchy to answer, and SHALL distinguish "this layer is not a mesh layer" from "this mesh layer carries no hierarchy" — a host draws different rows for those two.

#### Scenario: A loaded document reports its hierarchy rows
- **WHEN** a document holding one plain mesh layer and one mesh layer with a hierarchy is loaded, and each layer is queried
- **THEN** the first reports no hierarchy, the second reports one, and neither call decodes a surface

#### Scenario: A non-mesh layer is distinguished from an empty one
- **WHEN** an SDF layer and a plain mesh layer are queried
- **THEN** the two answers differ, and neither is reported as carrying a hierarchy

### Requirement: A loaded hierarchy is reachable as a handle
A host SHALL be able to obtain a `clay_multires` for a hierarchy a load produced, on the same terms as one it built itself: the handle's lifetime is the host's, destroying it SHALL NOT remove the hierarchy from the document, and saving the document afterwards SHALL write what the document holds.

Asking for a handle on a layer carrying no hierarchy SHALL be refused with the code that means "no such thing here" rather than one meaning the call was malformed, because a host walking every layer will ask this question about layers that legitimately have no answer, and a retryable code and a programming-error code mean opposite things to it.

#### Scenario: A loaded hierarchy is sculptable
- **WHEN** a host takes the handle for a hierarchy produced by a load and sculpts a level through it
- **THEN** the edit applies to that hierarchy, and saving the document writes the edited surface

#### Scenario: Destroying the handle keeps the document's copy
- **WHEN** a host takes a handle, destroys it, and saves the document
- **THEN** the hierarchy is still written, and reloading yields it unchanged

#### Scenario: A layer with no hierarchy is refused, not faulted
- **WHEN** a handle is requested for a mesh layer carrying no hierarchy
- **THEN** the call is refused with the not-found code and nothing is allocated
