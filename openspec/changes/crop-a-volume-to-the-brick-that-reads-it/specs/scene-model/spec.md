## ADDED Requirements

### Requirement: A culled compile narrows a sampled volume

A tape compiled against a cull region SHALL carry only the part of a sampled
volume that region can read. A volume's influence bound is its whole box, so the
item cull cannot drop one and nothing else narrowed it: a tape for a single
brick carried the entire payload, which made every operation that compiles per
brick — meshing with gradient normals above all — cost the size of the volume
once per brick rather than the size of the brick.

The narrowing SHALL be exact inside the region, on the same terms the cull
already promises. A cropped volume necessarily answers differently OUTSIDE its
crop, because the evaluator clamps a query onto the sampled box; that is what a
culled tape already does with the items it drops, and it is why a caller may
only evaluate a culled tape inside the region it was culled to.

**The volume's sample lattice SHALL NOT move.** A narrowed volume keeps the
origin and the brick grid of the volume it came from, because the evaluator
locates a sample by arithmetic on that origin and moving it changes the
interpolation weights — which makes the narrowed field agree with the whole one
to within rounding rather than exactly, and the difference is invisible until
something compares bytes.

Where a region reaches every stored brick, or none, the whole volume SHALL be
emitted rather than a narrowed copy: narrowing to nothing is a different field,
and narrowing to everything is a copy for no gain.

#### Scenario: A brick's tape carries a brick's worth of samples
- **WHEN** a tape is compiled against a region the size of one brick, over a document holding a large sampled volume
- **THEN** the payload it carries is a small multiple of one brick's samples rather than the whole volume's

#### Scenario: The narrowed tape answers exactly
- **WHEN** a culled tape over a sampled volume is evaluated inside its region, within the band
- **THEN** every value equals what the whole document's tape returns, exactly rather than approximately, including under a placement that rotates, moves and scales the item

#### Scenario: Colour survives the narrowing
- **WHEN** the volume carries a colour lattice
- **THEN** the narrowed tape returns the same colours as the whole one inside the region, because a narrowing that compacted samples without compacting colours would shade correct geometry from the wrong brick

#### Scenario: A whole-document compile is untouched
- **WHEN** a document holding a sampled volume is compiled with no cull region
- **THEN** the whole volume is emitted, since there is no region to narrow to
