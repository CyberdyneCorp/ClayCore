# Bend along a curve

## Why

Stage 2 of #116. `bend` and `bend_range` turn material about a fixed axis at a
constant rate — every bend they can express is a circular arc. An artist bending
a horn, a tail or a branch does not want an arc; they want to **draw the shape
the axis takes** and have the material follow it.

ZBrush's Gizmo 3D calls this Bend Curve. It is the third of the four gizmo
deformers and the last one that fits the existing machinery.

## What Changes

A `bend_curve` deformer: the item's local span along an axis is mapped onto a
**guide polyline's arc length**, and the material is carried on the guide's
parallel-transported frames.

The map is the **inverse** of `Prim::swept`. A sweep asks "given an arc length,
where does the profile sit"; a deformer is an inverse point map and asks the
opposite — "given a point, what arc length is it at, and where in that frame".
Both questions are answered by the same nearest-point-on-guide query, the same
transported frames, and the same blob layout. This change does not invent that
machinery; it **factors it out of the sweep and reuses it**, so the two agree
about what a guide is by construction rather than by inspection.

### The structural part

A deformer record is a fixed 12 floats. A guide is not a fixed size, so
`bend_curve` is the first deformer to need the **blob** — which means
`ctape_deform_point` gains the blob pointer it did not take, across all five
backends. That signature is the actual cost of this change, and it is paid once:
the Lattice (stage 3) needs exactly the same thing for its cage.

### What does not change

The `.clayspace` deformer codec stays forward-compatible in the way it already
is — the type is on the wire and the reader dispatches on it, so old files
decode exactly as before and a file containing a `bend_curve` carries its guide
as a length-prefixed point list after the extension floats.

## Impact

- `sdf-kernels` — new deformer, new field-info bound, a changed kernel signature
- `scene-model` — the deformer carries a guide; the codec learns to write one
- `bindings` — `pyclay` and the C ABI both reach it, and the parity corpus gains
  a scene so the four backends are held to the same frames

## Out of scope

Lattice / FFD (stage 3 of #116). It needs this blob plumbing and then an answer
to a question this change does not touch: forward FFD has no closed-form
inverse, and which inverse a claycore lattice uses is a design decision the
issue leaves open.
