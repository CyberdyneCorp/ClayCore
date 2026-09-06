# Proposal: snapshot only what a pass will overwrite

## Why

`make-the-relax-dab-local` made a dab's traversal follow the brush, and
`skip-the-noop-band-shrink` took the far-bound rebuild out of it. One
whole-volume term was left:

```cpp
const FieldVolume previous = current;   // per pass
```

A stencil needs the pass's *input*, not its half-written output, and copying the
volume is the obvious way to get one. It is **six megabytes at a 0.01 cell** to
protect the few hundred kilobytes a brush touches — a term that scales with the
model, inside a dab that had just been made to scale with itself. 0.162 ms of a
1.81 ms dab.

## What

`FieldVolume::snapshot_region(region)` copies only the bricks
`rewrite_region` would select for the same region. Reads outside them are served
from the volume itself, and that is correct for exactly the reason the region
limit is: a brick that does not meet the region is never written, so what it
holds during the rewrite is what it held before it.

Which makes the order a **requirement** rather than a convention — take the
snapshot, then rewrite the *same* region. A snapshot of one region used while
another is rewritten reads half-written bricks, and the header says so.

## Impact

| 24-dab stroke, cell 0.01 | before | after | |
|---|---:|---:|---|
| first dab | 2.96 ms | **2.31 ms** | 1.28× |
| steady dab | 1.81 ms | **1.60 ms** | 1.13× |

At a 0.02 cell the two are level; the copy was already small there.

The gain is more than the copy, and worth naming: a snapshot of one brush's
worth of bricks is a few hundred kilobytes and stays in cache, where a tap
against the whole volume walks a sparse index into six megabytes. The read path
is where this was nearly lost — see `design.md`.

A dab now has nothing left in it that scales with the model.

## Non-goals

**Ping-pong buffers**, which #278 kept as the alternative. They avoid the
allocation and not the copy: the whole sample array still has to be duplicated
per pass. A region snapshot avoids both.

**Threading a pass.** Bricks share face samples, so parallel writes need an
ownership rule; that is its own change and there is no measurement asking for it
yet.
