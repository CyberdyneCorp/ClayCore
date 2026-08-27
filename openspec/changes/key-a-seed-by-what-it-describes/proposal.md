# Proposal: key a seed by what it describes, not by where it sits

## Why

`resume-a-brick-at-a-time` gathered the seed store's admission test into
`shaped_entry` and `seed_for`. Between them they check the sample count, the
voxel spacing, the dims, colour, the layers beneath, the cull pad and whether
the prefix produced an accumulator. Two fields that also decide what a seed
describes were left out.

**The band was stored and read by no gate.** `ResumeEntry` recorded it,
`store_seed` set it, and nothing compared it. A brick's tape is culled against
`request_brick_box(req).dilated(req.band)`, so the band is not a display
setting — it decides which items the evaluation contains. A seed taken under a
smaller band was continued from a tape that had already dropped items a larger
band keeps, and folding a suffix onto it leaves them out for good.

#349 filed this as hardening rather than a bug: a sweep of 45 configurations
found zero differing samples. The sweep varied the band with appends that also
moved the **cull pad**, which is a document-global maximum and which `seed_for`
already gates, so the seed was refused for the pad before the band could
matter. Holding the pad still — plain spheres under a hard union have no
feather pad at all — the difference is plain. Brick (0,0,0) of a dim-8, 0.05
cache, seeded at a 0.15 band and then asked for at 0.6:

| | |
|---|---|
| samples wrong | 9 of 512 |
| worst error | 0.105 — two voxels |
| true distance there | 0.354 |
| band asked for | 0.60 |

The wrong samples are inside the band the caller asked for, which is exactly
where `clay_brick_cache_submit` stores the true distance rather than a clamp.
So this is a wrong field, not a difference below the contract: **it is a bug**.
The reverse direction — a wide seed serving a narrow request — also differs,
there outside the narrow band where a submit would clamp it away, but the
resumed and full paths are equal bit-for-bit by contract and a caller reading
`clay_brick_cache_eval_requests`'s floats is entitled to that.

**The key was the brick coordinate alone.** A brick coordinate is only unique
within a lattice: a coarse cache and a fine one over one document — a viewport
and a mesher — share brick (2, -1, -1) while holding different numbers of
samples in it. `shaped_entry`'s spacing and dims checks made that *correct*,
refusing the mismatched entry, but `store_seed` then overwrote it. Asked in
turn the two evicted each other on every call and NEITHER ever resumed:
`entries` sat at 1 for two caches and `resumed_bricks` at 0 for every dab.

## What

**The seed key becomes (brick coordinate, dims, spacing, band).** All four are
properties of the CACHE that asked, fixed for its lifetime, so keying on them
both refuses a seed that describes a different field and lets two caches hold
their own seeds side by side instead of thrashing. The floats are compared and
hashed by their bits, because an `unordered_map` requires equal keys to hash
equal and `-0.0 == 0.0` while their bits differ.

The cull **pad** is deliberately not in the key. It is the other term of the
same dilation, but it is a property of the DOCUMENT — a global maximum an
append can raise — so it moves under a single cache, and keying on it would
strand the old entry rather than replace it. It stays a gate in `seed_for`.

`ResumeEntry` loses `spacing`, `dims` and `band`; the key carries them, and
`touch_region` reads them from there to rebuild each seed's cull region.
`shaped_entry`'s spacing and dims comparisons go with them, being tautological
once the lookup enforces the lattice.

No ABI or descriptor surface changes: a refill that used to resume a
differently-banded seed now takes the full walk and returns the field it
already had to return.

## Non-goals

The device-buffer refill still has no resumed path. Eviction is still FIFO by
first insertion rather than LRU, and `touch_region` still leaves erased keys in
the order deque. The resumed loop still holds the document cache lock across
every compile and evaluation. None is made worse here.
