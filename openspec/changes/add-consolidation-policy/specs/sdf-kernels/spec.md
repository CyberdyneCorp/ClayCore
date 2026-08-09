# sdf-kernels — a consolidation policy

Delta for `add-consolidation-policy`.

## ADDED Requirements

### Requirement: A degrading chain is detectable
The engine SHALL make the degradation of a chained region-verb edit observable, from the declared Lipschitz and the safe step scale it already tracks, so that a host can act before the field becomes unusable rather than discovering it by eye.

The cause SHALL be stated where it is felt: each region verb samples a document and hands back a volume, and outside its band a volume reports a lower bound rather than a distance — so a verb applied to a previous verb's volume blends from the wrong value.

#### Scenario: A chain reports its own degradation
- **WHEN** a region verb is applied to the result of a previous region verb
- **THEN** the declared Lipschitz and the safe step scale reflect the chained cost, and both are readable before the next edit

### Requirement: Consolidation bounds the cost of a stroke
After consolidation, a chain of region-verb edits SHALL hold its declared Lipschitz within a stated bound rather than multiplying per edit.

A stroke is many gestures — a polish is repeated until it looks right, and a move is a sequence of drags — so the per-edit multiplication is the difference between a verb that exists and a verb that is usable.

#### Scenario: Repeated polishing stays usable
- **WHEN** the polish verb is applied several times over the same region with consolidation in effect
- **THEN** the declared Lipschitz stays within the stated bound and the form is not corrupted

#### Scenario: A drag sequence does not decay geometrically
- **WHEN** a move stroke is made as a sequence of drags with consolidation in effect
- **THEN** the safe step scale does not decay by a constant factor per drag
