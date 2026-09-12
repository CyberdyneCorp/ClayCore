## ADDED Requirements

### Requirement: Every pointer-driven gesture can lag, and they all share one ceiling

A gesture driven by a pointer position SHALL be able to lag that pointer, with
the same meaning and limits as the lag applied during stroke resolution.

Most brushes receive the lag before they are reached: resolution smooths the
samples and the brush consumes stamps that are already smoothed. A gesture takes
a total displacement from an anchor and never passes through resolution, so it
SHALL carry its own — and every pointer-driven gesture SHALL carry it, not only
some.

A gesture that is NOT driven by a pointer position SHALL NOT accept one. A
control that is accepted and cannot act is worse than one that is absent.

**The lag SHALL NOT be placed on a structure shared with entry points that
cannot use it.** Where the natural home would be shared with stamp-based calls
that receive an already-smoothed position, the lag SHALL be scoped to the
gesture instead.

**One ceiling SHALL govern every path.** A value a caller supplies SHALL NOT be
honoured by one path and silently reduced by another; where a path cannot accept
a value, it SHALL refuse rather than quietly alter it.

#### Scenario: A lagging gesture trails, then arrives
- **WHEN** a pointer-driven gesture with a lag is updated once toward a target, and then held there
- **THEN** the first update falls short of it, and repeated updates converge on where an unlagged gesture would already be

#### Scenario: One ceiling
- **WHEN** a lag above the ceiling is given to a gesture
- **THEN** it is refused, rather than accepted and reduced

#### Scenario: A gesture with no pointer does not accept a lag
- **WHEN** a gesture that takes no pointer position is examined
- **THEN** it exposes no lag to set
