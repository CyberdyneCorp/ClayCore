## Context

`field::flatten` already has both overloads (`include/clay/field/flatten.h`):
one taking a `std::function<float(cfloat3)>` source plus a region, cell size
and band, and one taking a `FieldVolume`. `pyclay` exposes both;
`bindings/c/clay_c.cpp` exposes only the second. So this is ABI surface over
an engine function that exists, not new math.

## Goals / Non-Goals

**Goals:** make the document-sourced flatten reachable from C; stop the parity
table concealing the asymmetry.

**Non-Goals:** changing `clay_item_volume_flatten` (it is correct for a volume
source, which is what an imported mesh gives); changing `field::flatten`;
adding a relax equivalent (relax has no `relaxed_from` — it is not asymmetric).

## Decisions

**Two descriptors rather than one.** The call takes both a
`clay_flatten_params` and a `clay_volume_params`. Folding the sampling fields
into `clay_flatten_params` would grow a struct that `clay_item_volume_flatten`
also takes, so a caller of the in-place form would see fields that do nothing
for it — and the versioned-descriptor pattern makes a grown struct silently
readable, which is exactly when a dead field gets set and ignored.

**The region is the same optional pair `clay_item_volume_from_document`
takes**, with the same rule: both NULL means the document's own bounds padded
by the band, and passing one without the other is refused. A third region
convention in the same ABI would be a trap.

**Validation is shared, not restated.** The flatten half reuses the parameter
reading and refusals of `clay_item_volume_flatten` (zero-length normal,
`region_radius > 0`, mode range, mask resolution) and the sampling half those
of `clay_item_volume_from_document` (cell size, region, empty result). Two
copies of "region_radius must be > 0" would drift.

**The test asserts the DIFFERENCE, not success.** A test that both calls
return `CLAY_OK` would have passed before this change too. The test builds a
case where the band is the limit and holds that the document-sourced facet
lands on the plane while the volume-sourced one does not.

## Risks / Trade-offs

- **A second flatten entry point is a choice a caller now has to make.** →
  The header says which to use and why: a document if you have one, a volume
  if that is all there is. The in-place form's existing accuracy note already
  points at the new call.
- **Sampling a whole document is more expensive than flattening a volume in
  place**, because it evaluates the tape rather than reading stored samples. →
  That is the cost of correctness and it is the caller's to weigh; the cheap
  path stays available and stays documented.
