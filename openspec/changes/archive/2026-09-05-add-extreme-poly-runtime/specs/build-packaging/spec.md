# build-packaging — the module the profile lives in

Delta for `add-extreme-poly-runtime`.

## ADDED Requirements

### Requirement: A new core module is declared in the layering rule
If the memory profile and the scratch arena land in a new core module, that module SHALL be declared in the layering rule with its permitted dependencies, and the layering check SHALL enforce it as it does every other module.

A module that exists in the tree and not in the rule is unconstrained by construction, which is how the thread pool ended up private to a backend and out of reach of the core library that needed it.

The module SHALL depend on as little as the type allows. A budget descriptor and a scratch allocator need nothing above the standard library and the shared vector types.

#### Scenario: The layering check covers the new module
- **WHEN** `tools/check_layering.py` runs after this change
- **THEN** the new module is declared, its edges are validated, and an include that violates them fails the check
