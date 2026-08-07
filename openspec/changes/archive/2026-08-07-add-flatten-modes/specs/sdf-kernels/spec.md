# sdf-kernels — one-sided flatten

Delta for `add-flatten-modes`.

## ADDED Requirements

### Requirement: Flatten can act on one side of its plane
Flattening SHALL offer three modes: blending the surface toward the plane from both sides, removing material only, and depositing material only. The default SHALL be the two-sided behaviour, so a caller that does not ask for a mode gets what it got before.

Removing only is the hard-surface case — ZBrush's hPolish, Planar and the Trim family. Cutting without filling is what leaves a crisp facet against untouched surface; filling the hollows beside a facet is what a polish must not do.

The mode SHALL be a parameter of the existing operation rather than a separate entry point, since the three differ by one clamp on the blend term.

#### Scenario: Cutting only leaves a hollow alone
- **WHEN** a surface carrying both a bump above the plane and a hollow below it is flattened in cut-only mode
- **THEN** the bump is planed onto the plane and the hollow is unchanged

#### Scenario: Depositing only leaves a bump alone
- **WHEN** the same surface is flattened in fill-only mode
- **THEN** the hollow is filled to the plane and the bump is unchanged

#### Scenario: Two-sided is what it was
- **WHEN** a surface is flattened without asking for a mode
- **THEN** the result is identical to the same flatten before modes existed

#### Scenario: Whichever side it acts on lands on the plane
- **WHEN** any mode is applied at full strength within its region
- **THEN** the material it acted on ends on the plane, not short of it

#### Scenario: A one-sided flatten is no steeper than a two-sided one
- **WHEN** the Lipschitz of a one-sided result is measured
- **THEN** it does not exceed what the same two-sided flatten declares
