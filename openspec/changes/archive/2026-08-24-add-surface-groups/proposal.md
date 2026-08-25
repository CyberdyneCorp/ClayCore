# Proposal: name a region of surface, and hide one

## Why

Every tool this library is measured against has a way to name a piece of a
model and then act on that name. ZBrush calls them PolyGroups, Blender calls
them Face Sets, and an artist uses them constantly and without ceremony:
isolate the head, hide the armour, mask everything but the hand, grow the
selection to the panel border.

**This library has no such concept, on any representation.** The nearest things
are three, and each falls short in a different way:

- **Visibility is per LAYER.** A layer can be hidden and a region cannot, so
  "isolate the head" means the head was authored as its own layer — a decision
  taken before the artist knew they would want it.
- **A layer holds ONE mask.** `clay_document_add_mask` replaces the mask a
  layer already had, so a host cannot even emulate N named regions with N
  masks. There is one channel, and it is the one the brushes gate on.
- **Scene groups group EDITS, not surface.** A group is a sub-expression in the
  edit list. It says how three items combine; it says nothing about which part
  of the resulting surface belongs to the head.

So the operations an artist expects — hide, isolate, grow, shrink, border,
extract, split, merge — have nowhere to attach, and every host that wants them
must invent its own semantics on top of an engine that does not have any. That
is the failure mode this library exists to prevent: it is why the C ABI carries
brushes rather than leaving hosts to write their own.

The second half is **partial visibility**, and it is the same primitive. The
ROADMAP has carried "hidden is not deleted, and hidden state survives
resampling" as a requirement taken from a competitor's bug since the list was
collected. It is satisfied for a LAYER (see `correct-the-undo-scope`) and
unreachable for a region, which is the case the competitor's users hit.

## What this is worth, honestly

**It is not one feature, it is a substrate.** A surface group id is what
procedural masks, extract-by-region, per-region resolution and per-region
material assignment all attach to later. Scoping it as "polygroups" undersells
it, and building it as "polygroups" would produce a mesh-only answer that the
SDF and voxel layers cannot use.

**The representation question is the whole design.** A mesh can carry a per-face
id and that is easy. A voxel grid can carry a per-cell id, which is a second
palette-indexed channel and mostly a memory question. An SDF layer has no
elements to label at all — its surface is a function, and the honest answer is
either a spatial field of ids (like the mask lattice) or a rule that maps a
surface point to the item that produced it. **These are different mechanisms,
and pretending one abstraction covers all three is how this ends up shipped
wrong.** The first task is to decide that, not to implement it.

## What changes

A group id addressable per representation, with a common query — *which group
is this surface point in* — and the set operations an artist expects. Partial
visibility is defined in terms of it, so "hide" and "isolate" mean the same
thing on all three representations even where the storage does not.

## What this does NOT include

Extract, split, and per-group material are downstream of the id existing and
are deliberately out of this change. So is per-group resolution. Landing the id
and visibility first is what makes those separable rather than speculative.
