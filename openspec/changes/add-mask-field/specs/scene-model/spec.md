# scene-model — layers may carry a mask

Delta for `add-mask-field`.

## ADDED Requirements

### Requirement: A layer may carry a mask
A layer SHALL optionally carry a mask field, absent by default, stored beside its voxel content and keyed by layer id rather than inside the evaluated document. Its presence SHALL NOT change how the layer evaluates: masking gates where edits are authored, not where the field is sampled, so per-brick culling and blend rigidity are unaffected. Keeping the mask out of the evaluated document makes that structural rather than a property to be maintained.

#### Scenario: Evaluation is unchanged by a mask
- **WHEN** a mask is painted on a layer and the document is evaluated
- **THEN** the field is bit-identical to the same document without the mask

#### Scenario: Freeze protects what comes next
- **WHEN** a region is masked and a further edit is authored across it
- **THEN** the masked region is spared, while items already in the list are unaffected by the mask
