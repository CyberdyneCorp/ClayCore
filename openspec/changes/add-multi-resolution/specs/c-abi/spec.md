# c-abi — sculpt at more than one resolution

Delta for `add-multi-resolution`.

## ADDED Requirements

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
