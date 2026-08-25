# Design: why flatten could not simply bake first

## The easy two

`FieldVolume::sample_blocks` already takes a `BrickBlockFill`: a callable handed
consecutive slot windows that writes `count * kBrickSamples` values, sample
index x-fastest, exactly the values a per-point callable would produce at
`grid.sample_position`. Blocks land in slot order whatever order they were
computed in, so the result does not depend on the evaluator's scheduling. That
contract is what makes any of this byte-identical rather than merely close.

The **plain bake** is a straight substitution: the fill produces what
`tape.eval(p).d` produced.

**Relax** is nearly as easy, because its document-sourced form was already
literally sample-then-relax:

```cpp
return relax(FieldVolume::sample(source, region, cell_size, band), settings);
```

The batched overload is the same line through `sample_blocks`. Nothing about the
smoothing changes; only who evaluates the source.

## Flatten is not a post-process

The tempting version — bake the source, then flatten the volume — is wrong, and
quietly so.

`sample_blocks` decides **which bricks to keep** from the values it is handed: a
brick whose every sample is further than the band from the surface stores
nothing and records only which side it is on. Flatten moves the surface, and the
brushes doc is explicit that it moves it *many band widths*, which is the whole
reason the document-sourced overload is preferred over the volume one.

So:

- bake the source → the kept bricks are the ones around the **source's**
  surface;
- flatten those samples → the surface moves to the plane, which may be several
  bands away;
- the facet now sits in a region where no brick was kept.

The band no longer tracks the surface. That is the exact failure the volume
overload's own doc comment warns about, reintroduced by the back door.

The per-point overload avoids it by blending **inside** the sampled callable, so
the values `scan_block` tests are already the flattened ones and the kept bricks
are the ones around the flattened surface. The batched overload has to preserve
that, so it applies the blend to the block the source filled, in place, before
returning:

```cpp
source(grid, first, count, block);
for each sample:
    block[at] = flatten_at(settings, pl, grid.sample_position(first + s, i), block[at]);
```

Same values, same order, same brick decisions.

## One blend, not two

Having two source overloads means the blend could be written twice, and a blend
written twice drifts. It is now one function, `flatten_at`, taking the settings,
the resolved plane, the position and what the source said there. Both overloads
call it; the guards that decide "these settings describe no flatten at all" are
shared the same way, in `resolve_plane`.

This is also what makes the byte-identity test meaningful rather than
tautological: the two overloads differ in *who evaluates the source and in what
batch*, and in nothing else.

## Lifetime

`tape_block_fill` captures the tape by reference and returns a lambda. That is a
hazard worth naming: the tape must outlive the `sample_blocks` call the fill is
handed to. Every call site holds it as a local or a `shared_ptr` on the stack
across that call, and the fill is passed directly into the sampling call in the
same expression, which is the only shape this is for. The header says so.

## The fallback

If no CPU backend is registered, `eval_points` cannot be reached and the fill
walks the tape scalar-wise itself, writing the same values. A build without a
backend bakes at the old speed rather than failing — the same choice
`consolidate.cpp`'s `fill_window` already made with its optional
`BakePointEval`.

## Verification

- Byte-identity against the per-point overload for all four paths — plain bake,
  relax, flatten, and flatten's no-plane guard branch — over a **polished**
  sphere. A steep field is the right fixture: kept bricks hold values far past
  the band, so a fill that disagreed about a sample near a brick's edge would
  change which bricks survive rather than only what they hold, and
  `serialize()` compares both.
- `BM_VolumeBakeDoc` against `BM_VolumeBakeSerialDoc` in `FASTER_THAN`, so the
  serial walk cannot come back unnoticed on this path the way it did on the
  last one.
- Full unit suite, the C ABI smoke test, and `examples/20_relax.py`,
  `21_flatten.py`, `38_consolidation.py`, which drive the pyclay side.
