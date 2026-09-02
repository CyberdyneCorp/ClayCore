# scene-model — the cull plan's survivors do not depend on how it scans

Delta for `pack-the-cull-scan`.

## ADDED Requirements

### Requirement: A coarse cull plan may precompute its test but not change its answer
The coarse cull MAY fold per-entry constants — whether an item's influence is local, and whether its bound is infinite — into a form decided when the entry is cached rather than re-derived per plan, and MAY hold that form apart from the survivor records so the scan reads only what it tests. Both clauses are settled once the entry exists, and re-deriving them dominated the scan: the scan is the plan, at 0.1373 ms against 0.1379 ms for the bare predicate loop over 50,000 entries.

The survivors a plan reports SHALL be exactly those the survive test reports, in chain order, for EVERY region — including the degenerate ones. A folded test that is exact only over ordinary regions is not exact, because the region a plan is asked about comes from the caller.

An entry whose own geometry bound is EMPTY SHALL be culled by every region, as the survive test culls it. This is the case a fold gets wrong: such an entry is local and its bound is not infinite, so a fold that stores bounds and tests them with a bare intersection stores an empty box, and an infinite region passes a bare intersection against ANY box. Empty geometry bounds are reachable — a stroke or armature with no points, a volume with no payload — so this SHALL be tested rather than argued.

An EMPTY region needs no such exception and SHALL NOT be given one, since a bare intersection against it passes only a box that is infinite on every axis, which is what the fold gives the entries that can never be culled and is what the survive test returns there.

#### Scenario: A folded scan answers what the predicate answers
- **GIVEN** a chain holding non-local items, items with infinite bounds, items with empty bounds and ordinary ones
- **WHEN** it is planned against an empty region, an infinite region, a region over part of it and a region over none of it
- **THEN** each plan's survivors are the survive test's survivors, in chain order

#### Scenario: An item with no geometry is culled by a region containing everything
- **WHEN** a document holding a strokeless stroke, a boneless armature and a payload-less volume is planned against an infinite region
- **THEN** none of the three survives, and every item with a real bound does

#### Scenario: An extended index scans what a rebuilt one scans
- **WHEN** an index is extended by an append and another is built fresh over the same document
- **THEN** the two plans report the same survivors for the same region
