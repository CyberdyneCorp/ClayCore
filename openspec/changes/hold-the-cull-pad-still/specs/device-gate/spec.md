# device-gate

## ADDED Requirements

### Requirement: The SDF cases are measured with a smooth blend

The SDF stamp and stroke cases SHALL be measured on documents whose items carry
a SMOOTH blend, not only a hard one.

A hard blend contributes nothing to the chain pad — the pad resolves to zero and
holds there — so a hard-blended fixture cannot exercise the cull pad at all, and
every effect that depends on it is invisible to the suite however many cases it
carries. The clay and build brushes are smooth by default, so a hard-blended
fixture is also not the document a sculptor makes.

Where both are worth measuring they SHALL be separate cases rather than one
fixture changed, so neither stands for the other.

#### Scenario: A pad effect is visible to the suite
- **WHEN** a change makes the cull pad move on every append
- **THEN** at least one SDF case reports it, rather than every SDF case being blind to the pad because its fixture is hard-blended

#### Scenario: The smooth and hard shapes are separate rows
- **WHEN** the run record is read
- **THEN** a smooth-blended SDF stroke case and a hard-blended one appear under different case names
