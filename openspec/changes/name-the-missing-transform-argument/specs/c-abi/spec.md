# c-abi — the arguments a transform requires

Delta for `name-the-missing-transform-argument`.

## ADDED Requirements

### Requirement: A transform's arrays are required, and a missing one is named
Every entry point taking a transform as position, rotation axis, angle and scale SHALL require BOTH arrays and SHALL refuse a null one with `CLAY_ERROR_INVALID_ARGUMENT`, leaving the document unchanged. A null rotation axis SHALL NOT be read as "no rotation": these calls take the whole transform rather than a partial update, so a null that meant identity would also silently decide the fate of the position beside it, and the caller who passed it wanted the position applied.

The refusal SHALL name WHICH argument was missing. A message covering both equally tells a caller that one of two was null and leaves them to guess, and the refusal is the only thing standing between a host and an edit it believes landed.

The axis refusal SHALL name what to pass instead, since a caller reaches it wanting no rotation and the signature already expresses that as any non-zero axis with an angle of 0 — which is what the readback answers for an unrotated node, so the round trip closes.

#### Scenario: A null rotation axis is refused and nothing moves
- **WHEN** a node's transform is set with a position and a null rotation axis
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the node's placement reads back exactly as it did before, position included

#### Scenario: The message names the missing argument
- **WHEN** the same call is made once with a null axis and once with a null position
- **THEN** the two diagnostics differ, and each names the argument that was missing

#### Scenario: No rotation is said with an axis and a zero angle
- **WHEN** the refused call is repeated with any non-zero axis and `rotation_angle` 0
- **THEN** it succeeds, the node moves to the position given, and the rotation reads back as 0

#### Scenario: One rule across every transform entry point
- **WHEN** a null array is passed to the node transform, the per-axis node transform, the layer transform or either mesh transform
- **THEN** each refuses on the same terms, so a host cannot learn the rule from one call and be caught by the next
