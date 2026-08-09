# c-abi — sculpt at more than one resolution

Delta for `add-multi-resolution`.

## ADDED Requirements

### Requirement: Levels across the ABI
The C API SHALL expose adding, dropping, counting and selecting levels, and every verb SHALL take the level it acts on. The addition SHALL be purely additive: an existing caller that never mentions a level SHALL get today's behaviour.

#### Scenario: An existing caller is unaffected
- **WHEN** a program compiled before this change runs against the new library
- **THEN** it links, and its grids behave as single-level grids exactly as before
