# sdf-kernels — a volume carries its own colour

Delta for `volume-color-channel`.

## ADDED Requirements

### Requirement: A sampled volume may supply colour per sample
The tape's volume evaluation SHALL be able to report a colour read from the volume's own samples, rather than only the distance. Colour is known where the distance is computed — inside the opcode, from the blob — and the existing split, where a prim returns a distance and the caller applies the item's constant colour, cannot express a field whose colour varies.

The colour SHALL be optional per volume. A volume with no colour section SHALL evaluate exactly as it does today: the same distance, and the item's constant colour. A blob whose colour offset is zero SHALL be indistinguishable in behaviour from a volume authored before this existed.

Colour SHALL be interpolated at the same samples and by the same rule as the distance. A boundary between two colours gradates across a cell; reading the nearest sample instead would put a facet on a surface that does not have one.

Prims other than the volume SHALL NOT pay for this. The colour output SHALL be an out-parameter the volume opcode may write and every other prim ignores, rather than a wider return type every prim constructs.

The dialect SHALL remain single-source. The same header SHALL compile for CPU, CUDA, Metal, OpenCL and Vulkan, and the parity suite SHALL compare COLOUR as well as distance for a coloured volume, on every backend registered in the build.

#### Scenario: A coloured volume evaluates its own colour
- **WHEN** a volume carrying per-sample colour is evaluated at a point inside its sampled box
- **THEN** the reported colour is the volume's own, interpolated between samples, and not the item's constant colour

#### Scenario: An uncoloured volume is unchanged
- **WHEN** a volume with no colour section is evaluated
- **THEN** the distance and the colour are what they were before this change, bit for bit on the CPU reference

#### Scenario: Every backend agrees about colour
- **WHEN** a document containing a coloured volume is evaluated on each registered backend
- **THEN** the colours agree within the parity tolerance, as the distances already must

#### Scenario: Outside the box the item's colour still applies
- **WHEN** a coloured volume is evaluated outside its sampled box
- **THEN** the item's constant colour is reported, since there is no sample to read
