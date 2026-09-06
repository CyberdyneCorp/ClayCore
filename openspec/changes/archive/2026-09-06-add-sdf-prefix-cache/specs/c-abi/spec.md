# c-abi — drain a Smooth preview instead of copying it

Delta for `add-sdf-prefix-cache`.

## ADDED Requirements

### Requirement: A host can take a Smooth preview one brick at a time
The existing preview call copies the WHOLE working volume, which is right for a host joining a gesture mid-stroke or rebuilding a lost preview and wrong for a per-frame loop: a dab moves a ball of bricks and the host re-uploads the model. The ABI SHALL additionally hand over exactly the bricks whose bytes are new — the ones a dab brought in, and the ones its relax moved.

It SHALL be the two-call size-query shape the ABI already uses for draining dirty work: one call reports what is waiting without taking it, and one takes it into buffers the CALLER owns. The first SHALL take nothing and change nothing, so a host may call it every frame to decide whether to bother. The whole-volume copy SHALL keep working, because a simple host must not have to implement patching to draw anything at all.

Each brick SHALL be described by its lattice key, the world position of its first sample, the spacing between samples, the number of samples per axis including the halo, and where its samples begin in the sample buffer. Samples SHALL be laid out back to back in the order the ABI's other volume calls use. The per-brick record SHALL be an ARRAY ELEMENT and SHALL therefore carry no size field, exactly as the ABI's other array elements do: a caller receives hundreds of them and reads them rather than filling them in.

#### Scenario: A dab's bricks can be drained into caller buffers
- **WHEN** a dab is applied and the delta is asked about and then taken into buffers of the reported size
- **THEN** the counts match what was reported, every brick names its key, origin, spacing and sample count, and the samples are laid out at the offsets the records give

#### Scenario: Nothing waiting is reported as nothing
- **WHEN** the delta is asked about before any update
- **THEN** it reports no bricks, no generation and no bounds

### Requirement: A short delta buffer takes nothing
Taking the delta is what CLEARS it, so a partial drain would strand bricks that nothing reports a second time.

When either buffer is too small, the call SHALL take NOTHING, SHALL report the buffer-too-small error, and SHALL write into the out-counts what it needs — so a caller grows its buffers and asks again from a state it has not damaged. The delta SHALL still be waiting afterwards, whole.

#### Scenario: A short buffer leaves the delta intact
- **WHEN** a take is attempted with buffers smaller than what was reported
- **THEN** it fails with the buffer-too-small error, the out-counts say what is needed, and asking again reports exactly the same bricks still waiting

#### Scenario: The grown buffer takes all of it
- **WHEN** the buffers are grown to the reported size and the take is repeated
- **THEN** it succeeds, and nothing is waiting afterwards

### Requirement: The preview delta accumulates, deduplicates and carries a generation
The delta SHALL accumulate across updates until it is taken, so that a host which skips a frame loses nothing, and a host which reads twice is told the same thing twice.

It SHALL be deduplicated by BRICK. A dab brings a brick in and then relaxes it, so the same coordinate arrives twice by construction, and a host uploading it twice would be paying for the bookkeeping this exists to save.

A generation SHALL be reported that advances on every update which CHANGED the preview and on nothing else — not on an update that moved nothing, and not on taking. Taking clears the delta and not the generation, because the generation names the state the caller now HOLDS rather than what is waiting. That is how a host tells a duplicate read from a skipped frame and drops an upload it began against an older state.

The payload SHALL follow the BRUSH and not the model: the same dab on a layer holding far more items SHALL hand over the same number of bricks.

#### Scenario: The same dab twice is reported once
- **WHEN** the same dab is applied twice without the delta being taken
- **THEN** the brick count is what one dab produced, and the generation has advanced twice

#### Scenario: The payload does not grow with the document
- **GIVEN** two layers alike where the brush is, one of them with hundreds of items far out of its reach
- **WHEN** the same dab is applied to each
- **THEN** the delta hands over the same number of bricks for both

### Requirement: A spent Smooth transaction refuses its delta rather than dangling
After a commit or a cancel the working field is released, so a delta read would describe something the host can no longer draw.

Both delta calls SHALL refuse a transaction that is no longer live, with the ABI's invalid-argument error, on the same terms every other call on that handle refuses it. The handle SHALL still be destroyable, and destroying it SHALL still be safe.

#### Scenario: The delta is refused after a cancel
- **WHEN** a transaction is updated, cancelled, and then asked for its delta
- **THEN** both delta calls fail with the invalid-argument error, and destroying the handle still succeeds
