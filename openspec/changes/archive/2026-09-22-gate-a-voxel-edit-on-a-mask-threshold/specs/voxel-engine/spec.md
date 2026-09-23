## MODIFIED Requirements

### Requirement: Masked voxel edits
Voxel edits SHALL accept an optional mask, and where one is given the effective edit strength at a cell SHALL be scaled by one minus the mask value there. A fully masked cell SHALL be left untouched by any edit.

Voxel edits SHALL also accept a mask THRESHOLD. At zero — the default — the mask scales the strength exactly as above, bit for bit. Above zero the mask SHALL be read as a stencil instead: a cell whose mask value is at or above the threshold SHALL be refused outright, and a cell below it SHALL be edited as though no mask were present. A threshold outside [0, 1] SHALL be refused rather than clamped, because a clamp would silently answer a different question than the caller asked.

The two readings are exclusive on purpose. Scaling below the threshold would leave a dithered band the threshold does not cover, which is the defect a stencil exists to remove.

#### Scenario: A frozen region survives an edit
- **WHEN** a region is fully masked and a brush is stamped across it
- **THEN** cells inside the masked region are unchanged and cells outside it are edited

#### Scenario: Partial masking attenuates
- **WHEN** a region is half masked and a brush is stamped across it
- **THEN** fewer cells change there than in the unmasked region, and more than in the fully masked one

#### Scenario: The default changes nothing
- **WHEN** an edit is made without setting a threshold
- **THEN** every written cell is the one the same edit wrote before the threshold existed

#### Scenario: A threshold refuses rather than attenuates
- **GIVEN** a mask whose value ramps across a skirt
- **WHEN** a solid brush is stamped across it with a threshold inside that ramp
- **THEN** no cell at or above the threshold is written, and cells below it are written as though unmasked

#### Scenario: The boundary is inclusive
- **WHEN** a cell's mask value equals the threshold exactly
- **THEN** the cell is refused
