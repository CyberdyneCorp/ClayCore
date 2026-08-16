# c-abi — alphas on SDF layers

Delta for `add-sdf-alphas`.

## ADDED Requirements

### Requirement: Alphas reach the C ABI through their own entry point
The C API SHALL expose applying an alpha to an item through a dedicated call rather than the flat deformer call, because a variable-length sample array does not fit that signature — the same reason bend curve has its own entry point. The samples SHALL be COPIED, so a caller may free its buffer immediately.

#### Scenario: A malformed stamp is refused rather than applied
- **WHEN** a caller passes a null sample pointer, a non-positive dimension, or a dimension that disagrees with the sample count
- **THEN** the call is refused and the item is unchanged
