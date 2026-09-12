## Why

**`clay_set_layer_mirror` and `clay_set_layer_radial` were write-only**, and
that absence was load-bearing in a real silent-wrong-geometry bug rather than a
tidiness complaint.

Symmetry was the odd one out. Every other piece of layer state has a reader
beside its writer — `clay_document_layer_info`, `_transform`, `_composition`,
`_protection`. Symmetry had none, so a host could not ask; it had to REMEMBER.

A remembered value is one an undo can invalidate behind its back. A host caches
"this layer's mirror is off" and short-circuits a redundant set, which is
correct and is the only thing it can do without a reader. Undo then reverts the
engine's `SetLayerMirrorCmd` and nothing tells the cache, so the host takes its
early return and sculpts through a mirror the artist turned off. Reproduced by
ClaySpaceDesktop (their #109):

```text
starting form, symmetry OFF, one dab, undo, the same dab again
  -x radius before anything:     1.00030
  -x after an unmirrored dab:    1.00030   correct
  -x after undoing the dab:      1.00030   correct
  -x after a SECOND dab:         1.28029   MIRRORED
```

The far side grew 0.28 world units on a stroke asked to be unmirrored.

**With a reader that class of desync is unreachable rather than fixed.** The
host reconciles instead of guessing: read, compare, set only if it differs. No
cache, nothing to go stale, and no need to reason about which undo ranges
contained a symmetry edit.

## Shape

Each reader takes what its writer takes, so what comes out goes straight back
in — the convention `clay_document_layer_composition` already sets. A mirror
axis reads 0 or 1 rather than the internal bitmask, for the same reason.

Every out-pointer is optional, so a call passing none still validates the
layer.

## Two answers that are NOT refusals

**A layer carrying no symmetry answers with it off**, rather than refusing.
"No mirror" is the true answer, and a host walking a stack should not have to
special-case it.

**Reading is not editing.** A ghosted, locked or hidden layer answers normally,
while SETTING one stays refused exactly as before. A non-SDF layer IS refused,
for the reason the composition reader already gives: zeroes there would read as
a real answer to a question the layer cannot express.

## What changes for a caller

Purely additive. Two new entry points; nothing that existed answers
differently.
