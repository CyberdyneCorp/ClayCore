# Merge a bake into a region of a layer

## Why

Consolidation collapses a whole layer. That is the right scope for a chain that
has genuinely degraded, and the wrong one for the thing a sculptor actually
does, which is work a PATCH.

A host applying a region bake per gesture has two options and both are wrong for
a stroke (issue #390):

1. append an `Op::Replace` volume per gesture — the layer grows one baked volume
   per stroke and every later bake samples all of them;
2. `clay_layer_consolidate` — collapse the whole subtool, discarding every
   parameter of every item outside the region the sculptor touched.

There is no middle. Measured on a real form, twelve gestures on one patch:

| gesture | ms | items | safe_step_scale |
|---:|---:|---:|---:|
| 1st | 22 | 2 | 0.30940 |
| 6th | 93 | 7 | 0.05130 |
| 12th | **244** | 13 | 0.03509 |

`clay_layer_consolidate` takes a region, but that region is only *where to
sample*: the call still collapses the entire edit list, so a small region drops
everything outside it rather than preserving it.

## What Changes

- **ADDED** `scene::plan_region_merge` / `scene::consolidate_region`,
  `clay_layer_plan_region_merge` / `clay_layer_consolidate_region` with
  `clay_region_merge`, and `Layer.plan_region_merge` / `Layer.consolidate_region`.
- A region merge bakes a region into one volume and puts it back where the items
  it absorbed were, leaving everything outside parametric.

## The scope is an INFLUENCE CLOSURE

This is the load-bearing decision, and it is the same argument that made the
existing scope a LAYER: an edit list is ordered and its operators are relative,
so "absorb the items near the stroke" has no well-defined field of its own. What
does is a region no remaining item can reach into.

    B = the caller's region
    repeat:
      S = the items whose influence bound meets B
      B = B union (the influence bounds of S)
    until B stops growing

It terminates because B only grows and the layer's own bound caps it. At the
fixed point every item that can change the field inside B is in S, and every
item in S can change the field only inside B. So nothing outside B moves, and
inside B the bake is the whole answer because no remaining item contributes
there at all.

**Plain containment is not enough, and the difference is not cosmetic.** Absorb
only the items overlapping the region and a Subtract straddling its edge stays
behind: the material it had carved comes back, and the volume cannot take it
away again, because `op_replace(a, b) = min(max(a, -b), b)` still reads `a`
wherever `b > 0`.

**One pass is not enough either.** An item early in the list may only be reached
after a later item widens the box, and a single forward pass has already gone
past it. The fixed point is what catches it — pinned by a test whose fixture is
built so a single pass takes one item where the closure takes two.

The closure may swallow the layer, and then this IS `consolidate_layer`. That is
the honest fallback rather than a failure, and the report says when it happened.

## Impact

- Affected specs: `scene-model`, `c-abi`, `python-bindings`
- Affected code: `include/clay/scene/consolidate.h`, `src/scene/consolidate.cpp`,
  `bindings/c/clay.h`, `bindings/c/clay_c.cpp`,
  `bindings/python/pyclay_module.cpp`
- Additive: `consolidate_layer` is unchanged, and the installer is shared, so
  everything the whole-layer form guarantees holds here for the same reason.
