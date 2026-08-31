## ADDED Requirements

### Requirement: A region of a layer can be merged from C
The C ABI SHALL expose the region merge as `clay_layer_consolidate_region`, with `clay_layer_plan_region_merge` reporting what it would absorb without baking, and a versioned `clay_region_merge` carrying the sampled box, the number of roots absorbed and whether the closure took the whole layer.

A missing or empty region SHALL be refused rather than treated as the whole layer: a region merge without a region is a whole-layer consolidation, and a caller should ask for that by name.

#### Scenario: The merge leaves the rest parametric
- **WHEN** a region over one of four well-separated items is merged
- **THEN** one item is absorbed, a volume takes its place, the other three remain, and the layer does not report itself consolidated

#### Scenario: Repeated gestures keep one baked item
- **WHEN** the same region is merged once per gesture over several gestures
- **THEN** the layer's node count does not grow

#### Scenario: A region it cannot make sense of is refused
- **WHEN** the call is made with a null or inverted region, an unknown layer, null params, a region over empty space, or a stale struct_size
- **THEN** it is refused and the document is unchanged
