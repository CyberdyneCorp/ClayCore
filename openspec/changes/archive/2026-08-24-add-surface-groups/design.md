# Design: name a region of surface, and hide one

## The decision that is the change

Task 1.1 says implementing before deciding this produces a mesh-only feature
wearing a general name. So: **one world-space id lattice per document**, shared
by all three representations, rather than per-representation storage.

### Why not the free answer

The alternative for SDF was "a rule mapping a surface point to the item that
produced it" — no storage at all. The proposal set up the case that decides it,
and that case kills it twice:

| case | a per-item rule |
|---|---|
| an armour panel spanning two items | **cannot express it** — the panel is not an item |
| a face that is part of one sphere | **cannot express it** — the sphere is one item |

An artist's groups do not respect the edit list, because the edit list is how
the shape was *built* and a group is about what it *is*. A mechanism that can
only name whole items is not the feature.

### Why one lattice rather than three stores

The obvious shape is per-representation: a per-face id on a mesh, a second
palette channel on a voxel grid, something else for SDF. Three mechanisms, and
the proposal warns that "pretending one abstraction covers all three is how this
ends up shipped wrong". The inverse is also true: three storages is three sets
of semantics for hide, isolate, grow and border, and they will disagree.

A world-space lattice answers *"which group is this surface point in"* the same
way for all three, because it never asks what the surface is made of.

**What it costs**, stated rather than discovered:

- **Resolution.** A group boundary is quantized to the group lattice, not to
  the representation. A mesh could have carried an exact per-face boundary and
  will not. That is a visible edge at the group border, and it is the price.
- **Memory.** The same sparse chunked storage a `MaskField` uses. A mask is
  already accepted at that cost, and a group field is the same shape.

**What it buys, and this is the part that decides it:**

- **Groups survive a representation bridge by construction** — task 1.3. They
  were never *in* the SDF, the voxels or the mesh, so rasterizing, meshing or
  converting cannot lose them. The alternative answer the task allows is "they
  are gone", and that is a much worse feature.
- **Voxel memory is untouched.** Task 1.2 worried about a second palette channel
  doubling a grid, against a `≥256³` guarantee that is 16.7 M cells. There is no
  second channel.
- **It inherits three things that already work**: world addressing (so a
  resolution change cannot misalign it, the property `add-mask-field` was built
  for), serialization, and — since `masks-in-the-history` — undo.

### Per document, not per layer

Task 1.4. "Isolate the head" when the head spans two layers is a real case and
per-layer storage makes it impossible. Masks are per layer because a mask gates
*edits to that layer*; a group names a region of the *model*.

## What a group is

An id per lattice cell, `0` meaning "no group" so an empty field costs nothing
and a document without groups behaves exactly as before. Ids are opaque to the
engine: the host names them.

Visibility is a property of the id, not of the cell — hiding group 3 is one
flag, not a rewrite of every cell carrying 3. That is also what makes "isolate"
cheap: show one, hide the rest.

## What this change does NOT do

Extract, split, per-group material and per-group resolution are downstream of
the id existing and are out, per the proposal. So is making the *meshers* skip
hidden groups — that is the visible half and it is a second change, because it
touches four meshers and the brick cache, and landing the id first is what makes
it separable rather than speculative.

## Open, and honest about it

- **Whether a group survives an edit that moves the surface.** The ids stay
  where they were, so a surface that moves out from under them is no longer in
  the group. A mask has exactly this property and is accepted with it, but a
  group is longer-lived than a freeze and the case deserves its own decision.
- **The lattice's resolution**, which is a per-document choice a host makes and
  which nothing yet advises them on.
