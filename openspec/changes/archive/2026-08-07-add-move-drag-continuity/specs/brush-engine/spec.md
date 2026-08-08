# brush-engine — move drag continuity

Delta for `add-move-drag-continuity`.

## ADDED Requirements

### Requirement: A drag coalesces rather than accumulating
A Move applied repeatedly as one gesture SHALL replace that gesture's warp rather than adding another beside it. A drag holds its centre and radius fixed and grows only its displacement, so those two identify the gesture without a caller having to thread an identifier through.

Otherwise a drag adds one warp per frame: the chain grows without bound, and because each warp multiplies into the declared Lipschitz, the safe step scale collapses over the length of the gesture.

A drag whose centre or radius differs is a different gesture and SHALL be kept beside the first. A leading deformer that is not a grab from a drag SHALL NOT be replaced.

#### Scenario: Frames of one drag do not stack
- **WHEN** a Move is applied repeatedly with a growing displacement and a fixed centre and radius
- **THEN** each item carries exactly one warp from that drag, and the field equals a single drag of the final displacement

#### Scenario: The marcher does not pay for the frame count
- **WHEN** the same drag is applied in many frames rather than one
- **THEN** the resulting safe step scale is the same

#### Scenario: A different gesture is kept
- **WHEN** a Move with a different centre follows one already applied
- **THEN** the earlier warp is kept and the new one is added in front of it

#### Scenario: An unrelated chain is left alone
- **WHEN** a Move is applied to an item whose chain already begins with a deformer that is not this drag
- **THEN** that deformer is kept, and the move goes in front of it

### Requirement: A move can be previewed without applying it
The library SHALL expose which items a drag would warp without modifying the document, so a host can preview a Move, or show what it is about to affect, before committing it.

#### Scenario: Previewing changes nothing
- **WHEN** a drag is previewed
- **THEN** the document evaluates identically to before, and the preview names the items the move would warp

#### Scenario: The preview agrees with the move
- **WHEN** a drag is previewed and then applied
- **THEN** the items reported by each are the same
