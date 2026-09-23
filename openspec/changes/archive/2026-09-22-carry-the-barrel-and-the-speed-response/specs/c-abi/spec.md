## ADDED Requirements

### Requirement: The stroke preset carries every control the engine has

The C stroke preset descriptor SHALL carry every control `brush::StrokePreset`
carries. A control the engine honours and the descriptor cannot name is a
capability the C ABI does not have, however complete the engine is.

The descriptor SHALL name the stylus BARREL rotation and the SPEED response —
a signed size channel, a signed strength channel, and the reference speed they
are measured against. All SHALL be APPENDED behind the existing `struct_size`,
and their zero SHALL be the behaviour that shipped before them, so a caller
compiled against the shorter layout resolves exactly the strokes it resolved
before.

The reference speed's own default SHALL be zero — which switches the response
off — and the descriptor's defaults call SHALL supply the engine's preferred
value instead. A field's own value SHALL NOT be read as "the caller did not
declare this".

**The barrel and the path SHALL NOT both be refused.** They are two answers to
one question, they are mutually exclusive by construction rather than by
validation, and where a caller sets both THE BARREL SHALL WIN, because a caller
that asked for the barrel meant the barrel. Refusing the pair would make a
preset library unloadable the moment one preset in it set both.

**A speed response with no reference speed SHALL be refused.** A non-positive
reference with a non-zero channel describes no stroke and would otherwise run
with both channels silently inert, which a host cannot see. Both channels at
zero SHALL leave the reference unread, so an older caller's zeroes stay legal.

The reference brush library SHALL cross with these controls intact: a preset
whose only distinguishing property is the barrel rotation SHALL arrive as that
brush and not as the brush it is derived from.

Where these controls act SHALL be stated in the header. They read sample
channels only the wider sample struct carries, so on the flat sample packing
they are inert, and a host SHALL be told that beside the fields rather than
discovering it.

#### Scenario: The barrel follows the stylus, not the path
- **WHEN** a stroke is resolved along one axis from samples whose azimuth points elsewhere, with the barrel rotation set
- **THEN** each stamp faces the azimuth, and not the direction of travel

#### Scenario: Both rotations set
- **WHEN** a preset sets both the barrel rotation and the along-stroke rotation
- **THEN** the stamps are identical to the barrel rotation alone

#### Scenario: The speed response is signed
- **WHEN** a stroke at the reference speed is resolved with a positive size channel, and again with the negative of it
- **THEN** the first widens each stamp by that fraction and the second narrows it by the same

#### Scenario: A speed response with no reference speed
- **WHEN** either speed channel is non-zero and the reference speed is not greater than zero
- **THEN** the call is refused rather than run with the response inert

#### Scenario: A caller compiled against the shorter layout
- **WHEN** a caller declares the `struct_size` the preset had before these controls, with both controls set in the memory past what it declared
- **THEN** the stroke resolves exactly as it did before the controls existed

#### Scenario: A named rake crosses as a rake
- **WHEN** a host asks the reference library for the preset whose only distinguishing property is the barrel rotation
- **THEN** that rotation is set on the preset it receives, and on the preset a byte round trip returns

## MODIFIED Requirements

### Requirement: A host can carry a brush preset across the ABI
The C ABI SHALL expose the brush preset — the stroke preset it contains and the model axes the mesh path already honours — through versioned descriptors following the established `struct_size` pattern, with bounded output fills.

Serialization SHALL cross as bytes rather than as a path, matching every other format the library writes, so a host holding a preset library in its own container never writes a temporary file.

Image content SHALL remain borrowed for the duration of a call. The ABI SHALL NOT take ownership of alpha or displacement samples, and SHALL NOT copy them into a preset.

Existing mesh brush entry points SHALL keep their semantics unchanged.

**THE BRUSH PRESET EMBEDS THE STROKE PRESET BY VALUE, WITH FIELDS AFTER IT**, so
appending to the stroke preset MOVES them. `struct_size` negotiates a tail, not a
shift, and there is nothing it can do about the middle. Where that happens:

- The brush preset's own original layout SHALL be DERIVED from the offsets it
  declares rather than written down, so it grows with the embedded descriptor
  and a caller compiled against the older layout falls below it.
- Every entry point taking one SHALL therefore REFUSE such a caller rather than
  read the fields after the embedded descriptor at the wrong offsets. A silent
  misread is the failure this ABI works hardest to avoid, and a refusal is the
  whole of the mitigation.
- The refusal SHALL name the change that caused it. The generic prefix-rule
  message tells a caller to declare the size it compiled against, which is
  exactly what such a caller did.
- The header SHALL say that a host compiled against the previous minor must
  recompile.

#### Scenario: A preset crosses and comes back
- **WHEN** a preset is serialized through the ABI, deserialized, and used to resolve a stroke
- **THEN** the resolved stamps equal those from the original preset

#### Scenario: An older descriptor is honoured
- **WHEN** a host passes a descriptor whose `struct_size` predates a field added later
- **THEN** the call succeeds using defaults for the fields it does not carry, and writes no byte past the size the caller declared

#### Scenario: A layout from before the embedded stroke preset grew
- **WHEN** a host declares the brush preset's `struct_size` from before the stroke preset it embeds was appended to
- **THEN** the call is refused, nothing is written into the host's buffer, and the error names the change that moved the fields
