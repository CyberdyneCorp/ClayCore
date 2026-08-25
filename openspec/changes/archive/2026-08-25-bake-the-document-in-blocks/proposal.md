# Proposal: bake a document through the pool, not one point at a time

## Why

`scene::bake_layer` has gone through `sample_blocks` with a pooled window fill
since it was written, and a benchmark pair gates it. The three document-sourced
entry points in the C ABI never did:

| | |
|---|---|
| `clay_item_volume_from_document` | `clay_c.cpp:5005` |
| `clay_item_volume_relax_from` | `clay_c.cpp:5099` |
| `clay_item_volume_flatten_from` | `clay_c.cpp:5192` |

All three handed `FieldVolume::sample` a `std::function` and asked the tape for
**one point at a time**, as did two of the `pyclay` equivalents. That is where a
bake's time went: a tape instruction costs about ten nanoseconds and its
arithmetic costs one (#207), so the interpreter is most of a bake and the
interpreter was per point.

Measured, same document, twelve-core machine, cell 0.05, 193-node layer:

| | |
|---|---:|
| `BM_VolumeBakeDoc` — batched block fill | **23.6 ms** |
| `BM_VolumeBakeSerialDoc` — the per-point walk | 386 ms |

**16.4×.** Wider than the consolidate pair's 7.5× because this measures the
bake alone, with no serial redistance floor on either side.

The mechanism already existed, was already wired through the bindings for
`bake_layer`, and is byte-identical by contract. These three entry points were
simply never moved onto it, and nothing gated them, which is how they kept the
serial walk long after `bake_layer` stopped using it.

## What

`eval::tape_block_fill(tape)` returns the window fill as a value the other bake
paths can pass. It lives in `eval/` for the reason `pooled_bake_eval` does: the
layering runs eval → scene, so a fill naming both a tape and a backend belongs
above both. It falls back to the tape's own scalar walk when no CPU backend is
registered, so a build without one bakes slower rather than not at all.

`field::relax` and `field::flatten` gain overloads taking a
`FieldVolume::BrickBlockFill` source. Relax's is trivial — its document form was
already exactly sample-then-relax. Flatten's is not, and `design.md` has why.

## Impact

No ABI change and no behaviour change. A host that already calls these gets the
speed without recompiling against anything new. Output is byte-identical, held
by a test rather than asserted.

## Non-goals

**`move_topological`'s document form.** It samples the source at a *displaced*
point, so a batched fill must build its own query positions rather than
post-process the lattice, and `solve()` is a second consumer with its own access
pattern. Different shape; #275.

**The duplicate Lipschitz measurement.** `flatten`, `move_topological` and
`mask_extrude` each call `measure_sample_lipschitz()` on a volume
`sample_blocks` has already measured. Real, free to fix, and not this change —
noted here so it is not lost.

**Whole-band relax traversal** (#272), which is the next one and the only
remaining O(field-size) term on this path.
