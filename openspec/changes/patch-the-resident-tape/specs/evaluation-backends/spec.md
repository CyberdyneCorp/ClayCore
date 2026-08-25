## ADDED Requirements

### Requirement: A compiled tape may name the tape it grew from
A tape produced by reusing another's prefix SHALL carry, beside its own identity, the identity of the tape it grew from and the offset in each of `instrs`, `params` and `blob` at which the two stop agreeing. Every other compile entry point SHALL report no lineage.

This does NOT change what `Tape::compile_id` means. It stays process-unique per compile and equal only for byte-identical sections, so a backend that ignores lineage keeps working exactly as it does today: it misses, it re-uploads, it is correct and merely no faster.

Lineage is a claim about bytes and SHALL be exact: below the stated offsets the two tapes agree byte for byte, and a tape naming an ancestor it does not actually extend is a defect, not a slower path — a backend that patches on it would evaluate a field that never existed.

#### Scenario: An appended tape names its ancestor
- **WHEN** a tape is compiled by reusing the prefix of another
- **THEN** it names that tape's identity and the three offsets at which it stops agreeing, and below each offset the two tapes' sections are byte-identical

#### Scenario: An ordinary compile claims no lineage
- **WHEN** a document, a layer, or a per-brick culled region is compiled normally
- **THEN** the resulting tape names no ancestor, and a backend treats it exactly as it does today

### Requirement: A backend may patch a resident tape instead of re-uploading it
A backend holding an uploaded tape resident MAY, when a tape names that resident tape as its ancestor, transfer only the sections above the stated offsets rather than the whole tape.

Patching SHALL change only speed. The values a patched tape evaluates to SHALL be identical to those of the same tape uploaded whole, an appended dab SHALL NOT be served the pre-append field, and a tape whose ancestor is not resident SHALL be uploaded whole.

A backend that grows its device buffers to absorb an append SHALL do so with spare capacity rather than to the exact size required, so that a stroke does not reallocate on every stamp — an exact-fit buffer turns every append back into the allocation the patch exists to avoid.

That capacity is finite, so a long enough stroke SHALL eventually exhaust it and be uploaded whole again. That is not a failure of patching: reserving proportionally means the re-packs over a stroke are geometric, so their cost is amortised. What SHALL NOT happen is re-packing on every append, which is what an exact fit does and which no test of the evaluated field can catch, because every one of those uploads is correct.

Where a backend packs two sections into one buffer, it SHALL NOT let one section's growth displace another that is already resident. Placing the sections with slack between them is one way; the requirement is that an append stays a write to the end of what changed.

#### Scenario: A stroke uploads once and patches after that
- **WHEN** a document is evaluated, then an item is appended and it is evaluated again, for a stroke short enough to fit the reserved capacity
- **THEN** the tape is uploaded whole once and patched for each later append

#### Scenario: A long stroke re-packs occasionally rather than per dab
- **WHEN** a stroke runs long enough to exhaust the reserved capacity several times over
- **THEN** the overwhelming majority of its appends are patched, and the whole uploads number in the handful that geometric growth implies rather than one per append

#### Scenario: A patched tape evaluates identically to an uploaded one
- **WHEN** the same appended document is evaluated through a patched resident tape and through a backend that uploaded it whole
- **THEN** the values are identical

#### Scenario: An unrelated tape is not patched onto a resident one
- **WHEN** a tape naming no ancestor, or naming one that is not resident, is evaluated
- **THEN** it is uploaded whole and evaluates correctly

#### Scenario: A stroke does not reallocate per stamp
- **WHEN** items are appended one at a time and the document is evaluated after each
- **THEN** the device buffers are reallocated far less often than once per append
