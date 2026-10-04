## MODIFIED Requirements

### Requirement: Consolidation across the ABI
The C API SHALL expose consolidating a layer and reporting its cost before it is paid, reusing what a volume already reports — bytes, brick count, sample count and sample Lipschitz. The addition SHALL be purely additive.

The cost SHALL also carry what the sample Lipschitz IMPLIES — the declared
Lipschitz and the safe step scale — because those are the numbers a host budgets
a frame against, and deriving them from `sqrt(3) x max(l, 1)` in every binding
would be re-implementing a kernel combinator outside the kernel.

Consolidation applies to SDF layers. The calls that bake, price a bake or plan a region merge — `clay_layer_consolidation_cost`, `clay_layer_consolidate`, `clay_layer_consolidate_cancellable`, `clay_layer_plan_region_merge` and `clay_layer_consolidate_region` — SHALL refuse a voxel or a mesh layer with `CLAY_ERROR_UNSUPPORTED`, and `clay_last_error` SHALL name the representation it found. The refusal SHALL come before the protection check and before anything is sampled, and SHALL leave the document unchanged. An SDF layer with nothing to bake SHALL keep answering `CLAY_ERROR_INVALID_ARGUMENT`, so a host can tell "wrong kind of layer" from "nothing there yet" by the code alone. `clay_layer_consolidation_advice` and `clay_layer_consolidation_state` are queries and SHALL keep answering a voxel or mesh layer: not advised, and not consolidated.

#### Scenario: The cost is knowable before consolidating
- **WHEN** a host asks what consolidating a layer would cost
- **THEN** it gets the memory and resolution it would spend, without the document changing

#### Scenario: The quote is the bill
- **WHEN** a host quotes a consolidation and then performs it with the same parameters
- **THEN** the brick count and byte count it was quoted are the ones it pays

#### Scenario: A voxel or mesh layer is refused as unsupported
- **WHEN** a host asks to consolidate a voxel or a mesh layer, to price that consolidation, or to plan or perform a region merge on it
- **THEN** the call returns `CLAY_ERROR_UNSUPPORTED`, `clay_last_error` names the layer as a voxel or a mesh layer, and the document and its undo history are unchanged

#### Scenario: An empty SDF layer still has nothing to consolidate
- **WHEN** a host asks to consolidate an SDF layer that holds no items
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` with "nothing to consolidate", not `CLAY_ERROR_UNSUPPORTED`
