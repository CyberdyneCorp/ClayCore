## ADDED Requirements

### Requirement: A surface drag can report where it reached

A drag applied to a layer's surface SHALL be able to report the regions it
invalidated, not only how many items it moved.

A caller that must invalidate a cache otherwise reconstructs the region from
the brush's size and the distance travelled. That reconstruction is looser than
the one the engine already derives, and under symmetry it is WRONG: a drag acts
at every image the layer's symmetry makes of it, and a caller holding a single
box either misses an image or unions them into a region far larger than either.

The regions reported SHALL be the ones the gesture actually invalidates —
expressed in the document's space and dilated exactly as the invalidation is,
including what a fold above the layer can move and the reach of every other
layer sharing the same edit list. A caller SHALL be able to mark those regions
and nothing else.

**A buffer too small SHALL be refused before anything is applied**, and the
number of regions needed SHALL be reported. The region set is known before the
first edit is recorded, so the refusal costs the caller nothing — the same
order already used to refuse a protected layer before the work rather than
after it. A partial report following an applied edit would leave a caller
unable to invalidate correctly, which is worse than a refusal.

The count SHALL be a property a caller can discover without applying anything.

The existing counting form SHALL be unchanged.

#### Scenario: The reported regions cover what changed
- **WHEN** a drag is applied and its reported regions are compared against where the field actually moved
- **THEN** every point whose field changed lies inside one of them

#### Scenario: A symmetric drag reports more than one region
- **WHEN** a drag is applied to a layer carrying a mirror, away from the mirror plane
- **THEN** it reports a region per image rather than one region spanning both

#### Scenario: A buffer too small changes nothing
- **WHEN** a drag is asked to report its regions into a buffer that cannot hold them
- **THEN** it is refused, the number needed is reported, and the document is unchanged

#### Scenario: Asking the count without applying
- **WHEN** the regions are requested with no room at all
- **THEN** the count is reported and nothing is applied

#### Scenario: The counting form is unaffected
- **WHEN** the same drag is applied through the form that reports only a count
- **THEN** it applies what it always did
