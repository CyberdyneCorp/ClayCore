# c-abi

## ADDED Requirements

### Requirement: A layer's placement is settable per axis

The API SHALL expose a per-axis form of the whole-layer placement beside the
existing single-factor one, taking three factors where that one takes a scalar,
and a reader answering the three. The existing entry point SHALL keep its
signature and its meaning.

The per-axis READER SHALL answer three equal factors for a layer placed through
the single-factor call, so a host driving one manipulator needs no branch.

The single-factor READER SHALL refuse a layer carrying a non-uniform scale with
an invalid-argument error rather than reporting one of the three, exactly as the
node-level reader refuses a squashed node: answering any single factor would
describe a placement the layer does not have.

Every factor SHALL be required non-zero, on the same terms the single-factor
call requires its scale positive.

#### Scenario: One manipulator, no branch
- **WHEN** a consumer reads the per-axis placement of a layer set through the single-factor call
- **THEN** it receives three equal factors

#### Scenario: The narrow reader refuses what it cannot describe
- **WHEN** a consumer reads the single-factor placement of a layer carrying three different factors
- **THEN** the call returns an invalid-argument error rather than one of them

#### Scenario: A squashed layer picks and bounds correctly
- **WHEN** a consumer raycasts and reads the bounds of a layer carrying a per-axis scale
- **THEN** the hit and the box describe the squashed shape, not the unsquashed one
