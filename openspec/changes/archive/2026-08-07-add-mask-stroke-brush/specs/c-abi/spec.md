# c-abi — the mask brush

Delta for `add-mask-stroke-brush`.

## ADDED Requirements

### Requirement: Masking through the C ABI is a stroke
The ABI SHALL expose the stroke engine's mask consumer beside the ones that write voxels and emit items, so a host paints a mask with the drag it already resolved rather than by looping single stamps and re-deriving spacing itself.

It SHALL also expose the bounded fill and invert, and SHALL accept an optional mask on the relax and flatten parameter blocks — added at the END of those structs, with `struct_size` deciding whether the field is present, so a caller compiled against the older layout keeps working unchanged.

#### Scenario: A host paints a mask along a drag
- **WHEN** a host resolves a stroke and applies it to a mask
- **THEN** the mask is painted along the path and the call reports how many stamps ran

#### Scenario: An older caller is unaffected
- **WHEN** relax or flatten is called with a `struct_size` from before the mask field existed
- **THEN** the call succeeds and behaves exactly as it did, with no mask

#### Scenario: Inverting within a box
- **WHEN** a host inverts a mask within a world-space box
- **THEN** the complement is taken over that box and nothing outside it changes
