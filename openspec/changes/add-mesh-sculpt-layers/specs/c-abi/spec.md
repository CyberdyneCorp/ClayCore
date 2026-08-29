# c-abi — sculpt layers and high-frequency stamping

Delta for `add-mesh-sculpt-layers`.

## ADDED Requirements

### Requirement: A host can drive the layer stack across the ABI
The C ABI SHALL expose sculpt layers by stable 64-bit identity, with add, remove, move, merge-down, bake, rename, set-active, set-strength, set-visible and set-locked, plus layer introspection carrying bytes and coverage.

Names SHALL be retrieved into caller-owned buffers. The ABI SHALL NOT return pointers into engine-owned strings whose lifetime a host cannot reason about.

High-detail stamping SHALL cross with a write domain and a stamp mode — alpha, height or vector displacement — with image data borrowed for the duration of the call.

Changed blocks SHALL be readable into caller-owned buffers with a capacity query, alongside revisions for base, detail, layers and evaluated state, so a host updates what changed rather than copying a display-level mesh per stamp.

Descriptors SHALL follow the established `struct_size` pattern with bounded output fills.

#### Scenario: A layer is addressed by identity across a reorder
- **WHEN** a host stores a layer identity, reorders the stack, and sets that layer's strength
- **THEN** the intended layer changes

#### Scenario: A pore stamp copies blocks, not the model
- **WHEN** a host stamps detail on a large surface and drains the changed blocks
- **THEN** the bytes copied follow the changed blocks
