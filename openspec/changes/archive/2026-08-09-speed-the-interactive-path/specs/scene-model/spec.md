# scene-model — one definition of what may be culled

Delta for `speed-the-interactive-path`.

## ADDED Requirements

### Requirement: An item's bound is derived once per compile
Compiling a document SHALL compute each item's world bound once. The influence bound of a local item IS its geometry bound, so asking for both means doing the same work twice — and for a stroke or a sweep that work re-tessellates the curve.

#### Scenario: A stroke-heavy document compiles without re-tessellating per item
- **WHEN** a document of curve-based items is compiled
- **THEN** each item's bound is derived once

#### Scenario: The tape is unchanged
- **WHEN** any document is compiled
- **THEN** the resulting tape's instructions, parameters, blob and bounds are exactly what they were before

### Requirement: Whether an item may be culled has a single definition
The test for whether an item's influence is confined to its geometry SHALL exist in exactly one place, and the influence bound SHALL be defined in terms of it. A caller holding the geometry bound already SHALL be able to ask the question without recomputing the bound.

This matters beyond tidiness. A second copy of the test that fell out of step would declare a non-local item cullable, and per-brick tapes would drop it while the whole-document tape kept it — so the field would be wrong only inside bricks that do not touch the item, and no whole-document assertion would notice.

#### Scenario: The predicate and the bound agree
- **WHEN** an item carries a non-local op, an infinite grid repeat, or a primitive with no finite extent
- **THEN** the predicate reports it as not local AND its influence bound is infinite

#### Scenario: An ordinary item is local and finite
- **WHEN** an item carries a local op, no infinite repeat and a bounded primitive
- **THEN** the predicate reports it as local AND its influence bound is finite

#### Scenario: A non-local item survives a distant cull region
- **WHEN** a document is compiled against a cull region far from every item's geometry
- **THEN** items whose influence is not local are still emitted, and only the local ones are dropped

#### Scenario: A cull region covering everything changes nothing
- **WHEN** a document is compiled against a cull region containing all of it, and again with no cull region
- **THEN** the two tapes are identical
