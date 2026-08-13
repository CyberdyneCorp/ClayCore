# Design: a colour channel on FieldVolume

The decisions, and what was rejected. Written before the code because three of
these are visible outside this repository once they ship.

## 1. Storage: packed RGB8, optional as a whole

One `std::uint32_t` per stored sample, parallel to `data_`, present or absent
for the whole volume rather than per brick.

| option | cost per sample | verdict |
|---|---|---|
| three floats | +12 bytes (4x a volume) | rejected — a volume is already the heaviest thing a document holds |
| **packed RGB8** | **+4 bytes (2x)** | **taken** |
| fp16 x3 | +6 bytes, unaligned | rejected — no better than RGB8 for authored colour, worse to address |
| per-brick optional | +4 bytes on coloured bricks only | rejected for v1 — it makes every reader branch per brick, including the tape, for a saving that only appears in volumes that are mostly uncoloured, which is not a case that exists |

8 bits a channel is more than either producer resolves: the voxel path's colour
comes from a 256-entry palette, and consolidation's comes from a float field
that a display quantises anyway.

**Optional as a whole** is what keeps this free where it is not used. A volume
built by `mesh::to_field` has no colour to store and stores none, so an
imported model costs exactly what it costs today.

## 2. The tape: a colour OUT-PARAMETER on prim evaluation

This is the load-bearing decision and the one that reaches other people.

Today `ctape_eval` computes a distance per prim and the caller applies the
item's constant colour from the parameter block. A volume with per-sample
colour breaks that split: the colour is known only where the distance is
computed, inside the opcode, from the blob.

| option | verdict |
|---|---|
| return `CTapeValue` from prim evaluation | rejected — every prim pays a colour it does not have, on every backend, to serve one opcode |
| **an out-parameter the volume opcode may write, ignored by every other prim** | **taken** — one branch at the call site, no cost in the prims that do not use it |
| a second opcode, `ctape_volume_colored` | rejected — doubles the volume paths through every backend and every cull, and the two would drift |

The blob header has a spare slot; the colour section is addressed by a new
offset there. A header whose colour offset is zero has no colour, which is how
an old volume and a new uncoloured one read identically.

**Interpolation matches the distance's.** Colour is read at the same eight
samples the distance is and blended trilinearly, so a boundary between two
colours gradates across a cell rather than snapping at a sample. Reading the
nearest sample instead would put a visible facet on a surface that has none.

## 3. Precedence: the volume's colour wins where it has one

A coloured volume's sample colour replaces the node colour. An uncoloured
volume, or a coloured volume outside its sampled box, uses the node colour as
it does today. `Op::Paint` continues to override both — it is the operator
whose entire job is to set colour, and a volume that ignored it would make
painting over a consolidated layer impossible.

## 4. The format: minor 9, backward-open, forward-refuse

Volumes persist through the scene payload (`scene/commands.cpp`), not through
the container directly, so this moves `kSceneMinor` and `kClaySpaceMinor`
together — the static assertion in `io/clayspace.cpp` already requires that.

- A **minor-8 document** opens in a minor-9 reader: its volumes have no colour
  section and read as uncoloured, which is what they are.
- A **minor-9 document** is refused by a minor-8 reader, by the existing
  forward-refuse rule, rather than being misread as a volume with a corrupt
  tail.

The serialised colour section is an absent-or-present block with its own
length, so a volume with no colour costs one marker rather than an empty array.

## 5. What the producers do

- **`consolidate`** writes the colour it currently discards. This is the change
  users will notice first: a consolidated layer stops turning one colour. It
  also means consolidation's output is no longer byte-identical to before, so
  the bit-identity gate that guards it has to be re-baselined deliberately, in
  the same commit, with the reason.
- **`VoxelGrid::to_field`** writes the palette colour per sample, so #90's
  per-entry loop collapses to one volume. `clay_voxel_to_layer` keeps its
  signature and produces one item instead of N — a behaviour change worth
  stating, since a host counting the items it gets back will count differently.

## 6. What is NOT in this change

- **The brick cache.** It caches the lattice it was configured with and never
  sees a `FieldVolume`; colour there is `add-brick-colour`'s business.
- **Per-sample anything else.** One channel, for the reason two shipped
  features are losing exactly one thing.
- **Removing the per-entry conversion.** It stays; it is how a caller gets one
  part of a sculpt as its own operand.
