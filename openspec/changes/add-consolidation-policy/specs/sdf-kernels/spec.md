# sdf-kernels — a consolidation policy

Delta for `add-consolidation-policy`.

Two numbers appear below and they are not the same one, so each requirement
names which it means. `sample_lipschitz` is how fast a volume's STORED SAMPLES
vary, measured over the cell size. The DECLARED Lipschitz is what the tape
compiler folds for a field containing that volume, which for a volume item is
`sqrt(3) x max(sample_lipschitz, 1)` — the interpolation factor `cfi_volume`
charges whatever the samples do. The safe step scale is `1 / max(declared, 1)`.
The examples print the first; a raymarcher pays for the second.

## ADDED Requirements

### Requirement: A degrading chain is detectable
The engine SHALL make the degradation of a chained region-verb edit observable, from the declared Lipschitz and the safe step scale it already tracks, so that a host can act before the field becomes unusable rather than discovering it by eye.

TWO causes SHALL be distinguishable, because they are different faults with
different cures and an aggregate cannot tell them apart:

- a verb that samples a document and hands back a volume, so a second
  application samples a VOLUME — and outside its band a volume reports a lower
  bound rather than a distance, so the blend works from the wrong value;
- a verb that appends a domain warp per gesture, so a stroke multiplies the
  chain's Lipschitz factor without any volume being involved at all.

#### Scenario: A chain reports its own degradation
- **WHEN** a region verb is applied to the result of a previous region verb
- **THEN** the declared Lipschitz and the safe step scale reflect the chained cost, and both are readable before the next edit

#### Scenario: The report names which mechanism degraded the chain
- **WHEN** a host asks a layer what its chain costs
- **THEN** it is told the steepest sample Lipschitz among the layer's volumes and the longest deformer chain on any of its items, alongside the aggregate

### Requirement: A sampled volume declares what its samples measure
A volume produced by sampling a field SHALL declare a sample Lipschitz no smaller than the slope its stored samples actually have.

Declaring 1 without measuring is not a conservative default, it is an
overclaim: the marcher steps by `f(p) / L`, so a field whose samples vary at
fourteen times the cell size and reports 1 licenses a step fourteen times too
long — the precise overstep the declared bound exists to prevent.

#### Scenario: Baking a steep chain does not claim to be 1-Lipschitz
- **WHEN** a steep field is sampled into a volume
- **THEN** the volume's declared sample Lipschitz is at least the slope measured between its stored samples

### Requirement: Redistancing bounds a baked field's Lipschitz
The engine SHALL be able to replace a sampled volume's stored samples with the distance to their own zero set, preserving each sample's sign and the surface it describes, and SHALL re-declare the volume's sample Lipschitz from what the result measures.

BAKING ALONE DOES NOT BOUND ANYTHING, and this requirement exists because the
opposite is the intuitive guess. Steepness is a property of the FIELD; sampling
it onto a lattice reproduces the steepness, and a finer cell makes it worse
rather than better because there are then more cells across the same steep
shell. Measured: two polish passes over a sphere bake to samples varying at 14x
the cell at 0.04, 31x at 0.02 and 38x at 0.01.

#### Scenario: A redistanced bake measures about 1
- **WHEN** a field whose samples vary many times the cell size is redistanced
- **THEN** its samples measure about one, and its zero set is where it was to the accuracy of the sampling

### Requirement: Consolidation bounds the cost of a stroke
After consolidation, a chain of region-verb edits SHALL hold its declared Lipschitz within a stated bound rather than multiplying per edit, and the bound SHALL NOT grow with the length of the chain.

The stated bound is `sqrt(3) x 1.10`: `sqrt(3)` is `cfi_volume`'s interpolation
factor, which any volume pays, and the 1.10 is the slack a redistanced bake is
allowed over a perfect 1.

A stroke is many gestures — a polish is repeated until it looks right, and a
move is a sequence of drags — so the per-edit multiplication is the difference
between a verb that exists and a verb that is usable.

#### Scenario: Repeated polishing stays usable
- **WHEN** the polish verb is applied several times over the same region with consolidation in effect
- **THEN** the declared Lipschitz stays within the stated bound and the form is not corrupted

#### Scenario: A drag sequence does not decay geometrically
- **WHEN** a move stroke is made as a sequence of drags with consolidation in effect
- **THEN** the safe step scale does not decay by a constant factor per drag

#### Scenario: Repeated consolidation does not grow the stored field
- **WHEN** a layer is consolidated repeatedly over the same region
- **THEN** the stored sample count does not grow with the number of consolidations
