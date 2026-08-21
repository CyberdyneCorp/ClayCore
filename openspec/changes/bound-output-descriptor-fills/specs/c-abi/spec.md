# c-abi

## MODIFIED Requirements

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
