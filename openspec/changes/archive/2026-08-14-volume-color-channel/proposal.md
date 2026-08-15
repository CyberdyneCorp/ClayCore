# Proposal: a sampled volume should carry its own colour

## Why

Two things in the tree lose colour for the same reason, and it is not a bug in
either of them: **a `FieldVolume` has nowhere to put one.**

- **The voxel round trip** (#90) converts once per palette entry and places one
  volume item each, because a single field cannot express two colours. That
  works and it ships, but a 40-entry palette becomes 40 items and 40 volumes.
- **Consolidation** bakes a whole layer into one volume, and the result carries
  one `Node::color`. Every per-item colour in that layer is gone — a
  consolidated character loses the distinction between skin and armour, and
  the operation is advertised as changing cost rather than appearance.

A `Node` carries exactly one colour, and the tape's prim evaluation returns a
**distance**, with colour supplied from the item's constant parameter block.
So the absence is structural rather than an oversight, and closing it is a
change to the kernel dialect rather than to a container.

## What

`FieldVolume` gains an OPTIONAL per-sample colour array, and the tape learns to
read it. Optional is load-bearing: a volume that has no colour stores none and
costs nothing, which is what keeps every existing bake and every mesh import
exactly as expensive as it is today.

Four things move together, and the order matters:

1. **Storage.** A parallel array of packed RGB8, one word per sample, present
   or absent as a whole. Colour is authored through a 256-entry palette or a
   float colour field; 8 bits a channel is more than either resolves, and the
   alternative — three floats — quadruples a volume rather than doubling it.
2. **The tape.** `ctape_volume` gains a colour output, which is the part that
   reaches every backend and every host compiling our headers.
3. **The format.** The volume's serialised layout grows a section, so
   `kSceneMinor` / `kClaySpaceMinor` move 8 → 9: backward-open (a minor-8
   volume reads as uncoloured), forward-refuse (a minor-9 document is refused
   by an older reader rather than misread).
4. **The producers.** `consolidate` writes the colour it is currently
   discarding, and `VoxelGrid::to_field` writes the palette, so #90's
   per-entry loop becomes one volume rather than N.

## The part that needs deciding, not just building

**This is a host-visible change.** `docs/06-host-gpu-previews.md` ships the
kernel headers as an artifact so a host can sphere-trace our fields in its own
shading language without reimplementing the maths — the whole point being that
there is no second implementation to drift. A host that has compiled
`kernel/tape.h` and evaluates a document containing a volume will need to
recompile against the new headers, because the volume blob grows a section and
the opcode reads it.

Nothing SILENTLY breaks: a host on the old headers reading a new blob sees the
header slots it knows and the same distances, and simply does not see colour.
But "recompile to keep up" is a cost paid by someone outside this repository,
and it is the reason this proposal exists as a decision rather than a patch.

## What this is not

**Not a change to what an uncoloured volume costs or evaluates.** A volume
with no colour array stores nothing extra, serialises to the same bytes it does
today apart from an absent-section marker, and evaluates to the same distance
and the same node colour.

**Not a general per-sample attribute system.** One colour, because colour is
what two shipped features are losing. A volume that wants a material id or a
mask per sample is a different proposal with a different storage argument.

**Not a reason to remove #90's per-entry conversion.** Converting one palette
entry stays useful — it is how a caller assembles a sculpt by hand, and how a
host gets one part of a sculpt as its own operand. What changes is that it
stops being the ONLY way to keep colour.
