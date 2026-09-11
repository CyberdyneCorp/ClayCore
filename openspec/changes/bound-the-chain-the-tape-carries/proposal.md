## Why

**A per-brick tape declares a Lipschitz bound for deformers it did not emit.**

`Compiler::cull_deformers` (`src/scene/tape_build.cpp`) drops every
finite-support warp whose ball cannot reach the cull region, because over that
region it is the identity — that is issue #452, and it works. `Compiler::fold_info`
is a SEPARATE call on the same item, and it folds `deformer_lipschitz(item)`,
which walks `item.deformers`: the whole chain, including the grabs the cull has
just dropped.

So the bound describes a longer chain than the tape it is attached to. Measured
with `benchmarks/cull_vs_bound_probe.cpp` on a unit sphere under Move dabs
(radius 0.40, drag 0.05, `ease_linear` — what `brush/move.h` actually sets):

```text
 moves   chain  emitted   declared L    safe_step
     1       1        0         1.12   0.88888890
     8       8        3         2.57   0.38974434
    32      32        6        38.52   0.02595804
    48      48        9       253.61   0.00394304
```

`1.125^48 = 253.6`. The tape emitted NINE grabs and declared the product of all
forty-eight. The `moves = 1` row admits no other reading: **zero deformers
emitted, 1.12 declared**, for a region where the tape is an undeformed sphere.

**This is not only slow, and that was the surprise.** `craycast`
(`kernel/field.h`) computes

```c
float h = f(ro + rd * t) * step_scale;   /* accept when |h| < eps * t */
```

so the step scale scales the distance THE HIT TEST SEES. A bound two orders too
large makes the marcher accept hits on rays that miss the shape. A 64x64 fan
into a worked cap at 48 dabs, scored against a dense sign-change truth of 3960
hits:

| | hits | truth | time | steps |
|---|---:|---:|---:|---:|
| bound over the item's chain | **4090** | 3960 | 759.2 ms | 3,511,660 |
| bound over the emitted chain | 3960 | 3960 | **48.0 ms** | 226,458 |

**15.8x faster, and 130 false hits removed.** In a host those are picks landing
on nothing, or at the wrong depth, with no error anywhere.

## What this is NOT

- **Not a new bound, and not a sampled one.** The same
  `deformer_lipschitz` runs, over a different list. It prices the emitted chain
  against the item's own extent (`prim_local_bounds`), so it bounds the tape's
  field EVERYWHERE the tape can be evaluated — including rays passing outside
  the cull region, where the culled tape's field is its own.
- **Not a change to the whole-document compile.** With no cull region every
  deformer is emitted, the lists are the same list, and the bound is unchanged.
  `benchmarks/declared_vs_actual_probe.cpp` reads identically on both sides.
- **Not the ease-slope work (#542)**, which is a different factor on the same
  product and is measured separately. Every figure here was taken with #542
  present on BOTH sides.
- **Not a meshing win.** Tried first and recorded because it measures nothing:
  `mesh_tape` evaluates a dense grid and never marches, so the step scale
  cannot reach it — 7.666 ms against 7.213 ms while the bound moved 43x.

## What building it found

**The first two designs were wrong, and both are recorded so they are not
retried.**

1. *Price the grab against the `local` AABB already in scope at the pricing
   site.* That `local` is `prim_local_bounds(item)` — the PRIMITIVE'S OWN
   EXTENT, not a query region. Every grab in a worked fixture spans it.
2. *Narrow each easing's slope to the sub-interval of its falloff the brick
   spans.* Sound, and worth nothing here: the Move brush sets `ease = 0`
   (`include/clay/brush/move.h`), and linear's derivative is the constant 1.

**A ground truth taken through `craycast` is not a ground truth.** The first
attempt marched at a deliberately tiny step scale and reported 4096 hits of
4096, including corner rays 1.06 from the axis that cannot touch a unit sphere
— because shrinking the step scale is exactly what makes the acceptance test
fire early. The truth had to be taken by walking the ray in fixed world
increments, outside `craycast` altogether. The same mechanism is why the
degraded bound reports MORE hits than the truth rather than fewer.

**The cache is keyed on the source vector's address, not on call order.** Every
`emit_prim` site passes `item.deformers` straight through and `emit_empty`
passes a temporary that can match no item. A stale entry would be read as a
SHORTER chain and would UNDER-declare the bound — which does not cost frames, it
steps the marcher through the surface. So `fold_info` falls back to the full
chain whenever the address does not prove the chain belongs to the item in hand.

## What changes for a caller

Nothing in the ABI, and nothing in any field's VALUE. A culled tape reports a
smaller `lipschitz` and a larger `safe_step_scale` than it did. A caller that
recorded those numbers as fixtures will see them move; a caller that marches by
them gets the same surface, sooner.
