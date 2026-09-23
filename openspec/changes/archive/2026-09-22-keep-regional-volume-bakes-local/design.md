## Context

An analytic operand cannot be cut out of an ordered fold merely by clipping
its bounds. A sampled volume can retain its untouched samples while a local
patch is replaced. Splitting it into roots would introduce artificial box
surfaces and make root count depend on the number of maintenance operations.

## Decisions

1. Prefer a retained volume plus a rebuilt patch, assembled into one ordinary
   volume. Keep the lattice origin and spacing aligned when extending storage.
2. Partial replacement requires a proof: a plain additive volume, no global
   modifiers or active replication/gating, identity node and layer transforms,
   unit scale axes, a grab-only deformer chain with positive radii and exactly
   zero easing at zero, and no other visible operand reaching the patch halo.
   Source spacing and effective band must match the requested settings. Unsupported cases keep the existing
   whole-root closure. Expand the patch to include every deformer removed,
   even if the caller supplies only the last stroke's dirty region.
3. Resample/redistance only the patch, with a halo. Transition to retained
   samples outside the edited support and copy unaffected stored samples and
   colours exactly. Recompute the resulting volume's conservative slope bound.
4. Preserve the existing build-before-publish transaction and command-based
   undo, including shared-content detachment. Cancellation publishes nothing.
5. Retained storage can require a linear copy and sparse-index rebuild. This
   is a maintenance operation, not a per-dab operation; do not claim O(patch)
   total time. Require local field evaluation/redistancing, bounded storage at
   fixed geometry, and measured improvement over repeated whole-root baking.

## Verification

Reproduce the stationary 8-subtool case before changing the implementation.
Gate closure extent, root count, evaluated samples, and retained lattice size
across repeated Move/bake cycles. Check field signs and surface positions, not
only costs; check gradient continuity across the patch transition. Exercise
coloured volumes, edits outside the last requested box, unsupported modifiers,
different sampling settings, undo/redo, serialization, and cancellation.

## Storage and boundary details

The patch is dense during redistancing and stitching. A missing source brick
returns a sparse lower bound, which must never become a stored distance sample:
stitching uses a retained sample (including a neighbouring brick's halo) when
one exists, and otherwise the dense patch distance. Colour uses the same retained
sample index. Bricks outside the transition are copied exactly. Steep blocks
with opposite signs are retained even if the caller skips redistancing and no
sample falls inside the narrow band.

The blend is smoothstep over a box-distance transition of at least four cells
and at least the band width, outside every removed grab's support. An additional
two-cell sampling halo is rounded out to whole source bricks. The installed
volume is ordinary serialized FieldVolume storage; no file format or C struct
layout changes. Python additionally reports the execution's actual `box`.

Nonidentity placements, squash, symmetry, repetition, gates, smooth blends,
other deformer types, overlapping operands and changed sampling settings retain
the whole-root behavior. Expanding support or sculpting a new area can legitimately
expand the patch; the bounded-work guarantee concerns stationary local edits.
The total operation still copies/indexes retained storage and measures its slope;
it is not independent of total retained volume size.


Reproduction, measurements and verification commands are recorded in
[validation.md](validation.md).
