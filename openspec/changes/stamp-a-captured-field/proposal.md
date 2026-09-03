## Why

An artist can sculpt a shape and cannot reuse it. Everything needed to make a
captured region of the field into a placeable, instanced, editable asset is
already in the tree — and an audit of the implementation guide against `main`
found that most of what it asks for is shipped:

| The guide asks for | Actually, on `main` |
|---|---|
| a sampled-field representation, not a captured subtree (§3.3) | `PrimType::Volume` is exactly that, and it compiles through the tape |
| the payload shared across instances, so "a thousand uses of one 4 MB asset must not consume ~4 GB" (§3.16, §3.17) | **TRUE IN MEMORY, FALSE ON DISK — see below.** The guide is right and this proposal's first draft was wrong |
| `capture_field_stamp(doc, world_region, settings)` (§3.7) | **`clay_item_volume_from_document(doc, params, region_min, region_max, out_item)`** captures a finite world region of the document's field, banded and redistanced |
| "do not add a stamp-specific distance transform" (§3.9) | there is none to add; the bake's redistance is what capture already uses |
| a new tape opcode, avoided if volume semantics suffice (§3.10) | not needed — a placed stamp IS a volume item |

### The one the audit got wrong, corrected by measuring it

`scene/types.h` says "A sampled volume. Held by shared reference on the Node, so
instancing one costs a pointer rather than a copy of its samples", and that is
true — of a document in memory. The writer serializes a node's volume INSIDE the
node record (`src/scene/commands.cpp`: `volume_bytes = n.volume->serialize(...)`,
per node) and the reader rebuilds one per node with
`std::make_shared<field::FieldVolume>(...)`. **The sharing does not survive a
save.**

Measured through the C ABI, one captured volume placed N times:

| placements | saved bytes |
|---:|---:|
| 1 | 187,531 |
| 8 | **1,499,457** |

Exactly eight copies. So the guide's "a thousand uses of one 4 MB asset must not
consume ~4 GB" is a LIVE DEFECT rather than a hypothetical, and the asset table
it asks for is needed for the reason that is easiest to miss: not to make
instancing cheap in memory, where it already is, but to keep it cheap across the
one operation that currently undoes it.

Both halves have to land together. A writer that deduplicates and a reader that
still calls `make_shared` per node loads N unrelated volumes, and the next save
writes N payloads again.

So this change is **not** a capture pipeline. It is the four things capture does
not give you:

1. **An oriented capture frame.** Today's region is a world-axis-aligned box. An
   artist stamping a detail onto a curved surface needs the capture taken about
   the SURFACE — `+Z` outward, `X/Y` the tangent plane — so the asset is
   reusable at another orientation rather than only where it was taken.
2. **An asset identity, and the deduplication that needs it.** Two placements of
   "the same stamp" are two items that share a payload in memory and two full
   copies on disk (measured above). Nothing names the asset, so nothing can
   write it once; nothing can list what a document uses; and there is no
   standalone form a host library can keep.
3. **A placement helper.** Turning a surface hit, a normal and a stylus azimuth
   into a stable frame is logic every host would otherwise write, and get
   subtly different. `kernel::calpha_frame` already resolves exactly this for the
   scalar alpha and is the single source it should come from.
4. **Stroke integration.** Spacing, pressure, jitter, taper and azimuth already
   resolve a stroke into stamps; nothing turns those stamps into placements.

## What Changes

- `field::FieldStamp` — a captured volume, its local bounds and the frame it was
  captured in, with a content id. It OWNS nothing new: the samples are the
  `FieldVolume` capture already produces.
- **Oriented capture**: a caller-supplied frame, and a surface-oriented helper
  that builds one from a hit, a normal and an azimuth through `calpha_frame`.
  **No PCA and no inferred orientation** — the guide asks for that and it is
  right: an inferred frame is an asset that rotates when you re-capture it.
- **Placement** as an ordinary `Volume` item under a transform, so a placed stamp
  is editable, undoable and instanced exactly as any item is. The transform
  order is pinned by a test, not by a comment.
- **Uniform scale only in v1**, stated rather than discovered: a volume's field
  is a bound rather than an exact distance already (`prim_is_bound_field`), and
  non-uniform scale makes the Lipschitz bound wrong in a way a marcher shows as
  missing surface. The guide says the same and the reason is worth keeping.
- **No `strength` multiplier.** `d *= strength` changes the distance metric, not
  the intensity. Scale, op and blend are the controls.
- Stroke integration: one gesture is one undo step, as every other stroke is.
- C ABI, pyclay, a numbered example, and capture/placement benchmarks.

## Capabilities

### Modified Capabilities
- `scene-model`: a captured region of the field is a nameable asset that can be
  placed many times without duplicating its samples.
- `c-abi`: capture, placement and the asset table across the boundary.

## Impact

- `include/clay/field/`, `include/clay/scene/` — the asset and its table.
- `bindings/c/`, `bindings/python/`, `tests/`, `examples/`, `docs/07`.
- ABI grows.

**Not in v1**: non-uniform scale, an inferred capture frame, a dedicated strength
parameter, and a stamp-specific distance transform — each because the guide
argues against it and the tree agrees.
