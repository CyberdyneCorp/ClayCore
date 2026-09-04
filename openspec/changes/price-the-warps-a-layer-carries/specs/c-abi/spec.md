## ADDED Requirements

### Requirement: A layer reports what its accumulated warps cost

A host SHALL be able to ask what the warps a layer has accumulated are charging
it. A drag records a warp on every item it reaches and each is evaluated per
sample for the life of the edit list, so a session of drags gets steadily dearer
to evaluate with nothing in the document that looks like a cost — and the only
remedy is a consolidation, which is expensive and destructive enough that a host
needs a reason before spending one.

The report SHALL distinguish what a WHOLE-DOCUMENT evaluation pays for from how
much of that a culled compile can drop, because those are different numbers and
a host acts on them differently: the first is what the accumulation costs a
viewport, and the difference is what working in bricks already saves.

A layer that cannot carry a warp SHALL report none rather than be refused, so
that a host walking a stack of mixed kinds does not have to special-case them. A
layer that does not exist SHALL be a typed not-found, because "no such layer" and
"no warps" are different answers.

The operation that records a warp SHALL say in its own documentation that the
warp is not free once it lands, since it otherwise reads as a bounded local edit
whose cost ends with the call.

#### Scenario: A host asks before and after a drag
- **WHEN** a layer is queried, dragged, and queried again
- **THEN** the second answer reports the warps the drag recorded, and a further drag adds its own rather than composing with them

#### Scenario: A layer that cannot carry a warp
- **WHEN** the layer queried is not one that can hold a warp
- **THEN** the report is zeroes rather than a refusal

#### Scenario: A layer that is not there
- **WHEN** the layer queried does not exist
- **THEN** the call reports not-found rather than zeroes
