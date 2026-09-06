# Design

## 1. The three questions 17.3 deferred, and where the answers come from

> "a cache is a session's policy and a device's memory ceiling, and the C shape
> for that (who owns it, whether it is per-document or per-host, how a budget is
> expressed) is a design question this change does not need to answer to ship"

**None of the three needs inventing.** `clay_brick_cache` answered all three in
2026-08 and its header states the rule outright:

> "There is no borrowed form: a cache belongs to whoever made it, never to a
> document, so this takes no document and destroy returns void."

So: the HOST owns it, it is PER-HOST (it takes a document per call and is keyed
by layer and fingerprint, so one cache serves any number of documents), and the
budget is BYTES, set at create and adjustable. Adopting that rule rather than
choosing again is the point — a host that has learned one cache's lifetime should
not have to learn a second.

## 2. The one thing that DOES need deciding: where the resolution comes from

`SdfPrefixCache::key_of` mixes `cell_size`, `band` and `padding` into the key. A
prefix built at one resolution and asked for at another is a MISS — not an
error, not a wrong answer, just silence and no acceleration. That is the worst
failure shape available here, because it is indistinguishable from the feature
not working.

The C++ API has already designed this out, and it is worth seeing how:
`SdfSculptPolicy` **nests** an `SdfPrefixPolicy`, and `SdfSmoothTransaction::begin`
overwrites the nested one's sampling from the outer policy's own
(`src/session/sdf_sculpt.cpp`):

```cpp
SdfPrefixPolicy prefix_policy = policy.prefix;
prefix_policy.cell_size = policy.cell_size;
prefix_policy.band      = policy.band;
prefix_policy.padding   = policy.padding;
```

A caller cannot express two resolutions because there is only one place to put
one. **The C ABI mirrors that**: `clay_sculpt_policy` grows the three CACHE knobs
— `min_history_roots`, `keep_live_suffix_roots`, `max_bytes` — and keeps the one
`cell_size`/`band`/`padding` it already has.

Rejected: a standalone `clay_sdf_prefix_policy` descriptor passed to both the
build and the begin. It reads cleaner and it is the shape that lets a host build
a cache nothing will ever hit.

Growing a descriptor is compatible by `struct_size`; that mechanism exists for
exactly this, and `tools/check_c_abi.py` already walks every entry point taking
one and requires a bounded fill.

## 3. Passing the handle: a second entry point, not document state

`clay_sdf_smooth_begin` cannot grow an argument. Two shapes were available:

**(a) `clay_sdf_smooth_begin_cached(doc, layer, policy, cache, token)`** — the
`clay_eval_grid` / `clay_eval_grid_device` shape, which is the precedent for "the
same call, plus a handle".

**(b) `clay_document_set_prefix_cache(doc, cache)`** — one call, and every future
consumer picks the cache up without its own variant.

**(a) ships.** (b) is tidier until it is not: it makes the document hold a
pointer to memory the host can free, which is precisely the hazard the tape
export was designed around ("no exported pointer may be silently invalidated"),
and it puts an optimisation into hidden state where a reader of a `begin` call
cannot see whether it is accelerated. The cost of (a) is one entry point per
future consumer, and there is currently exactly one consumer.

## 4. What is NOT exposed, and why

**`SdfSourceField`.** It is the composition machinery — open, fill windows,
compose a suffix onto a seed — and a host has no call for it: the only thing that
opens one is a Smooth transaction, and that is already a C surface. Exporting it
would publish an object whose whole contract is "correct whatever the cache
holds", which is not a decision a host makes.

**A synchronous "build if missing" flag on begin.** That is the one thing
`SdfSourceField::open` deliberately does not do, and re-adding it at the boundary
would reintroduce the whole-layer bake at the moment the header promises it was
removed from.

## 5. The scheduling note (17.2)

17.2 asked for "a measured recommendation, not a guess". The measurement, from
`BM_SdfPrefixBuild*` and `BM_SdfHistory*` (build/release, load 1.26 -> 5.70):

| | 5,000 items | 20,000 items |
|---|---:|---:|
| build | 576 ms | 2,170 ms |
| dab, full walk | 89.8 ms | 242 ms |
| dab, with prefix | 2.32 ms | 2.32 ms |
| stored bricks | 268 | 268 |
| bytes | 1.4266 MiB | 1.4266 MiB |

Two facts the pair gives that neither gives alone:

1. **The build follows the history** — 576 ms to 2,170 ms is 3.77x for 4x the
   items, near-linear.
2. **The volume does not.** 268 bricks and 1.4266 MiB at BOTH sizes (250 and
   0.7147 MiB for the piled fixture, likewise at both). The cache's SIZE is a
   property of the shape; only the time to fill it follows the history.

So a host sizes its budget from the MODEL and schedules the build from the
HISTORY, and those are two different questions with two different inputs.

Break-even, `build / (full dab - accelerated dab)`: **9.1 cold windows at 20,000
items**, 6.6 at 5,000, 8.4 and 6.8 for the piled fixture. The recommendation
follows and is stated as a number rather than as "when convenient": build when
the artist is likely to take more than ~10 cold dabs on that layer, between
gestures, never on the pointer-down path.
