## ADDED Requirements

### Requirement: A live drag can lag the cursor, and says what it ignores

A live surface drag SHALL accept a lazy-mouse lag, with the same meaning and
units the stroke preset gives it.

A drag is ONE deformation per gesture rather than one per dab: its region is
fixed when the gesture opens and only its displacement follows the pointer.
Most stroke controls therefore have nothing to act on, and the drag SHALL NOT
accept them. Where a control is declined, the interface SHALL say so and say
why, because a control that is accepted and does nothing is worse than one that
is absent.

In particular the interface SHALL state that the accumulation setting does not
govern how successive drags compose, since a caller assuming otherwise will
produce a drag that silently accumulates deformations.

**The lag SHALL be refused where it cannot act.** A form of the drag that
applies a whole gesture in one call retains no previous position to lag from,
and SHALL reject a non-zero lag rather than ignore it.

A lag outside the half-open range from none to total SHALL be refused: a total
lag never reaches the pointer at all.

**The lag SHALL be understood to suspend the drag's path-independence.** A drag
without lag ends where a single equivalent drag would, whatever intermediate
positions it passed through; a lagging drag trails its pointer by construction,
so that property cannot hold and the interface SHALL say so rather than let a
caller discover it.

A caller that does not set the lag SHALL be unaffected in every respect.

#### Scenario: A lagging drag trails, then arrives
- **WHEN** a drag with a lag is updated once toward a target, and then held there
- **THEN** the first update falls short of it, and repeated updates converge on where an unlagged drag would already be

#### Scenario: No lag is unchanged
- **WHEN** a drag sets no lag
- **THEN** it behaves exactly as it did before the setting existed

#### Scenario: A lag that cannot act is refused
- **WHEN** a non-zero lag is given to the form that applies a whole drag in one call
- **THEN** the call is refused rather than proceeding without it

#### Scenario: A lag outside the range is refused
- **WHEN** a lag of total or more, or less than none, is given
- **THEN** the drag is refused
