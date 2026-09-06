# c-abi — capturing, keeping and placing a stamp

Delta for `stamp-a-captured-field`.

## ADDED Requirements

### Requirement: A capture can be taken about a surface

The ABI SHALL let a caller capture a region of a document's field in a frame it
supplies, with the region named in THAT frame's coordinates rather than the
world's — so a box about the origin is a patch centred on a surface hit and its
depth is how far above and below the surface the capture reaches.

It SHALL offer a helper building such a frame from what a host actually has: a
surface hit, the normal there, and the stylus azimuth about that normal. The
azimuth SHALL rotate the tangent, and SHALL be measured from a fixed reference
rather than from whichever axis the normal happens to lean on least, so that one
azimuth means one direction wherever the hit is.

The frame SHALL NOT be inferred from the captured content.

What comes back SHALL be an ordinary item whose transform is that frame, so
adding it changes nothing about the document's field and moving it afterwards is
an ordinary edit rather than a second placement mechanism.

#### Scenario: A capture placed back is what was captured
- **WHEN** a region is captured about a frame that is not axis-aligned and the item is placed into an empty document unchanged
- **THEN** its field agrees with the source's over the captured region, within the sampling tolerance the capture declares

#### Scenario: The azimuth turns the asset
- **WHEN** two frames are built at one hit a quarter turn apart in azimuth
- **THEN** their tangents are perpendicular, and a full turn returns the tangent it started from

#### Scenario: A malformed frame is refused
- **WHEN** a capture names a zero normal, or a region with no surface in it
- **THEN** the call is refused and no item is produced

### Requirement: A captured asset can be kept outside a document

The ABI SHALL be able to write a captured asset to a self-contained form and
read it back, carrying the payload, the frame and the asset's identity.

Reading SHALL refuse a buffer that is truncated or is not one of these, rather
than reading past its end.

An asset SHALL carry an id derived from its CONTENT, so two captures that sample
identically are recognisably the same asset. It SHALL NOT be a unique
per-capture identifier: a host that captured the same detail twice is better
told so than left to accumulate duplicates it cannot recognise. Nothing SHALL be
dispatched on the id.

#### Scenario: A round trip preserves the placement
- **WHEN** a captured asset is saved on its own and loaded back
- **THEN** the loaded item places identically to the original, and its id is the same

#### Scenario: A truncated buffer is refused
- **WHEN** a buffer that is truncated, or is not an asset at all, is read
- **THEN** the call is refused and no item is produced

### Requirement: What the assets cost is reportable apart from the placements

A host SHALL be able to ask what a document's captured payloads cost, counted
once per ASSET rather than once per placement. Summing per placement reports the
multiplied cost that sharing exists to avoid paying, which is the wrong number
for the decision a host makes with it.

#### Scenario: Many placements of one asset cost one payload
- **WHEN** one captured asset is placed many times in a document
- **THEN** the report names one asset, that many placements, and the bytes of a single payload

### Requirement: A resolved stroke can be placed as stamps

The ABI SHALL turn the stamps a stroke resolves to — spacing, pressure, jitter,
taper and azimuth already applied — into placements of one captured asset, as
ONE undo step and ONE invalidation for the whole stroke.

A resolved stamp's RADIUS SHALL become a uniform scale against the asset's own
size, which is where the stroke's pressure lands. Its STRENGTH SHALL NOT
multiply the captured distance: that scales the metric rather than the sculpt,
moving the zero set and costing the gradient its unit length.

Every placement SHALL share one payload.

#### Scenario: A stroke is one undo step
- **WHEN** a resolved stroke of many dabs is placed
- **THEN** one undo returns the document to before the stroke, and the document holds one payload for all of them
