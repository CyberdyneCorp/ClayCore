# device-gate

## ADDED Requirements

### Requirement: The append path is measured in both shapes

The stroke case that exercises the compiled-prefix reuse SHALL be recorded for
dabs added at the LAYER ROOT and for dabs added inside a GROUP, under different
case names. One SHALL NOT stand for the other.

They exercise the same verb and take different paths: the append fast path
requires the added node's parent to be the root list, so the grouped shape
misses it and recompiles the whole tape per dab. A single case would report
whichever shape its fixture happened to build, and the shape that reaches the
fast path is the one a case named for that path would naturally use — so the
expensive shape is the one that goes unmeasured by default.

Both cases SHALL record how many dabs one timed body contained, so a per-dab
cost is derivable and the two are comparable at the same unit.

#### Scenario: Both shapes are in the record
- **WHEN** the run record is read
- **THEN** it carries a stroke case whose dabs are added at the layer root and another whose dabs are added inside a group, under different case names, each recording its dab count

#### Scenario: The grouped shape is not hidden by the flat one
- **WHEN** the append fast path stops firing for dabs at the layer root
- **THEN** the root case fails, and the grouped case — which never reached that path — does not mask it
