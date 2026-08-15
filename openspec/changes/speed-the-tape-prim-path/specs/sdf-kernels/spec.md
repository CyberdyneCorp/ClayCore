# sdf-kernels — forty prims are paying for one prim's colour

Delta for `speed-the-tape-prim-path`.

## MODIFIED Requirements

### Requirement: A sampled volume may supply colour per sample
The tape's volume evaluation SHALL be able to report a colour read from the volume's own samples, rather than only the distance. Colour is known where the distance is computed — inside the opcode, from the blob — and the existing split, where a prim returns a distance and the caller applies the item's constant colour, cannot express a field whose colour varies.

The colour SHALL be optional per volume. A volume with no colour section SHALL evaluate exactly as it does today: the same distance, and the item's constant colour. A blob whose colour offset is zero SHALL be indistinguishable in behaviour from a volume authored before this existed.

Colour SHALL be interpolated at the same samples and by the same rule as the distance. A boundary between two colours gradates across a cell; reading the nearest sample instead would put a facet on a surface that does not have one.

Prims other than the volume SHALL NOT pay for this, and SHALL NOT CARRY THE PARAMETER. The colour output SHALL be an out-parameter of the volume opcode's OWN entry point, reached by dispatching on the opcode, rather than a parameter on the shared primitive-distance function that every prim is compiled with — and rather than a wider return type every prim constructs.

The first wording of this requirement said only that other prims "ignore" the out-parameter, which the implementation satisfied in behaviour and broke in cost: the parameter went on the shared if-chain over every opcode, and a document of a thousand spheres — which never enters the volume branch — paid 1.14x for it (`mask_extrude`, 3281 ms to 3752 ms at `ac7460a`). "Does not pay" is a statement about cost and SHALL be read as one.

The dialect SHALL remain single-source. The same header SHALL compile for CPU, CUDA, Metal, OpenCL and Vulkan, and the parity suite SHALL compare COLOUR as well as distance for a coloured volume, on every backend registered in the build.

<!-- The four below are the shipping requirement's own scenarios, carried
     across verbatim. A MODIFIED block REPLACES the whole requirement, so any
     scenario it does not restate is dropped on archive — renaming one has the
     same effect as deleting it. Only the last scenario is this change's. -->

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

#### Scenario: A document with no volume in it pays nothing
- **GIVEN** a tape holding only analytic prims — spheres, boxes, strokes
- **WHEN** it is evaluated over a lattice
- **THEN** no evaluation passes or writes a colour out-parameter, and the cost is what it was before per-sample colour existed
