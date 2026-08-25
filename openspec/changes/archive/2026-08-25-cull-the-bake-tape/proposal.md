# Proposal: cull the bake's tape per brick, exactly

## Why

The bake evaluates the **whole** tape at **every** sample. A brick needs only
the items whose influence reaches it: a 600-dab sphere hard-unioned compiles to
1,199 instructions and any one brick needs **5.4**. `BrickCache` has evaluated
this way since it was written; the bake never has.

After #270 and #271 the bake is essentially pure tape evaluation, and it scales
linearly with tape length — 5.2× the instructions gives 4.1–4.9× the bake. That
is the signature of every instruction running at every sample.

## What

`eval::document_block_fill` compiles a tape per brick against the brick dilated
by the band, fanned out over the window's bricks with one `CullPlan` per window.

## Why it is exact, not approximate

Two facts, neither of them a measurement:

1. **`culled >= true`.** Culling drops items from a minimum, and dropping from a
   minimum can only raise it.
2. **`culled <= band` implies the two are equal.** An item is only dropped when
   its influence bound is more than a band from the brick, so inside the brick
   it cannot be the nearest thing while the nearest thing is within the band.

From those:

- a sample the culled tape puts **inside** the band already *is* the truth;
- a brick with no such sample **stores nothing** — only "not near the surface",
  which is right, and its **sign**, which (1) gives, since an item that could
  make a point inside is within a band of it and therefore kept;
- what remains is the samples a **kept** brick stores beyond the band. Those are
  stored raw and must be the whole tape's.

That last set is 27% of the samples. Evaluated as scattered scalar calls it
measured 1.78×; collected across the window and done in **one batch**, 3.48×.

## Why the remainder cannot be skipped

Storing culled values beyond the band would make the volume **overstate its own
distance**: measured 0.033 against the plain bake's 0.002, which is 1.65 cells
where the interpolation overshoot is 0.1. A field that overstates is one a
sphere tracer steps through. This was measured, not assumed, and it is the whole
reason the refinement exists.

## Culling is not always worth it

A smooth union's cull pad grows with `k` (#282), so a wide enough blend keeps
every item in every brick's tape and the per-brick compile is pure overhead.
On a 0.02 cell, whose bricks are 0.16 across:

| `k` | brick tape / whole | bake |
|---|---:|---:|
| 0.00 | 0.5% | **3.4×** |
| 0.04 | 7% | **2.2×** |
| 0.06 | 16% | 1.5× |
| 0.10 | 49% | 0.80× — refused |
| 0.16 | 100% | 0.54× — refused |

So the decision is **measured** from a sample of the lattice rather than guessed
from `k`, and the threshold — a third of the document's tape — was set by
measuring both ends of the crossover.

## Impact

The C ABI bake, 193 items at cell 0.05: **22.3 ms → 15.2 ms**. On the larger
fixtures the ratio reaches 3.4×. Output byte-identical; the volume oversteps
exactly as the plain bake does.

## Non-goals

**`clay_item_volume_flatten_from`.** Flatten transforms the block *after* the
fill produces it, so a brick the fill classified as empty can come back holding
the surface — and the values it would then store were never refined. It keeps
the whole tape. `test_c_volume.cpp`'s march-cost assertion caught this on the
first run and is what guards it.

**Metal and the other backends.** This is the CPU bake path.
