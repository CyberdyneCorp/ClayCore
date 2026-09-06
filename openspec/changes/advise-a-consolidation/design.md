# Design

Four decisions, each with what it rejected. Two of them refute the framing this
change was scoped with, and both are written down here rather than quietly
dropped.

## 1. Where `cell_size` comes from

### What it must not come from

The obvious candidate is the Lipschitz, and it is wrong. `clay_field_report`
carries `lipschitz`, `steepest_volume` and `steepest_deformer_chain`, and only
the middle one is a slope. `Tape::lipschitz_bounds_gradient` states the split in
the tree already:

> Whether `info.lipschitz` bounds `|grad f|` EVERYWHERE, and not only the step a
> marcher may take. Those are two different numbers. […] an ellipsoid's bound
> field measures a slope of 1.09 near its tips and 3.6 for a needle-shaped one
> […] Three deformer bounds are exceeded outright: taper's (up to 2.5x),
> wrap_around's (1.05x) and bend_curve's (7-9x).

A sampling rate is a statement about how fast the field varies over one cell. A
stepping bound is not that statement, and a layer holding an ellipsoid or a
bend_curve declares `L = 1` while its field varies several times faster. A
`cell_size` derived from `L` would be principled-looking and unsound, and it
would be *most* unsound exactly on the shapes that motivate a bake.

**This refutes the scoping of this change**, which named "the declared/sample
Lipschitz" as one of the two inputs. Only the *sample* Lipschitz of a volume
item is a slope, and where a volume is present its own `cell_size` is the more
direct number. So the Lipschitz enters the *advice* and not the *resolution*,
and section 3 is where it enters.

### What it does come from

A resolution is right when the smallest thing that must survive it spans enough
cells. Two real inputs: the layer's extent gives the scale, the layer's contents
give the feature.

Everything below is in the **layer's local frame** — the frame
`local_view(layer)` compiles and `bake_tape_with` samples in. `clay_layer_bounds`
is world-space and composes the layer transform; using its box here would advise
a `cell_size` wrong by the layer's scale, and the error would be invisible on
every layer at identity. Derive from the same `tape.bounds` the bake itself
uses, so the advised `padding` lands on the box the bake will actually sample.

Let `B` be that box, and:

```
E = max(B.max[i] - B.min[i])            the longest side

for each drawable node i:
    f_i = K * cell_size_i               if i is a volume carrying samples
    f_i = min axis extent of            otherwise
          item_geometry_bound(i)

F = min f_i                             the smallest feature the layer carries
K = 4                                   cells across that feature

cell_raw = F / K
cell_lo  = E / 512                      the finest grid advised
cell_hi  = E / 32                       the coarsest grid advised
cell     = clamp(cell_raw, cell_lo, cell_hi)

band     = 3 * cell                     the engine's default for band <= 0
padding  = band                         the engine's default for padding <= 0
skip_redistance = 0
```

**The volume arm is the load-bearing one.** `cell_raw = (K * c) / K = c`: a
layer whose finest content is a volume at cell size `c` is advised `c`
unchanged. That is the whole answer to "a number nobody chose" — the only
degradation ever advised is `CLAY_DEGRADATION_VOLUMES`, and such a layer carries
volumes whose resolutions were chosen at earlier bakes. The advice returns the
finest of them, so a re-bake loses no detail that is already stored, and gains
none that was never there.

**What the analytic arm assumes.** That a node's smallest authored dimension is
the smallest feature it contributes. That is true for a primitive and false in
one direction for a blend: two spheres smooth-unioned at a small radius produce
a fillet finer than either sphere's box. The advice is therefore conservative in
the wrong direction on heavy blends, it is not a promise of fidelity, and the
header says so.

**`band` and `padding` are spelled out rather than left as zeros** even though
zero means the same thing today. A host that stores the advice and re-bakes a
week later must get the same box, and a quote whose `out_cost` was measured at
`3 * cell` alongside params that say `0` describes two different bakes.

**`skip_redistance = 0`** is not a default carried through, it is the point:
redistancing is what bounds the Lipschitz, and advising a skip would advise a
bake that does not cure what the flag reported.

### The three constants, and what has to be measured

`K = 4`, `512` and `32` are choices, not derivations, and the implementer owns
measuring them:

- `K = 4` puts the smallest feature across half a brick (`kBrickDim` is 8). Two
  is where a sphere becomes an octahedron; eight quadruples the grid. Sweep 2,
  4, 8 on a fixture whose smallest item is a small dab on a large form, report
  surface agreement against the parametric field, and **if 4 is wrong, change it
  and write down what it measured.**
- `E / 512` and `E / 32` bound the GRID, not the memory. A banded volume stores
  only surface bricks, so the byte count depends on surface area and is not
  predictable from the clamp. That is fine here and only because `out_cost`
  reports `brick_count` and `bytes` at the advised number: **the host refuses on
  the measured bytes, not on the clamp.** The clamp only has to keep the
  sampling pass itself affordable.

## 2. The not-advised case: zeroed, not untouched

`*out_advises` is 0/1. When it is 0, `out_params` and `out_cost` are **zeroed to
the caller's declared `struct_size`, with `struct_size` itself preserved**.

Rejected: leaving them untouched, which is `clay_layer_consolidation_state`'s
convention ("`out_cost` may be NULL and is left untouched when the layer is not
consolidated"). The precedent does not transfer, and the difference is what the
caller does next. `clay_layer_consolidation_state` answers a yes/no about the
present, and a caller that ignores it reads its own uninitialised memory and
gets nonsense it can only misread. Here the caller is being handed something it
will feed into a **destructive** call, so the failure has to be loud on the
next call rather than plausible.

Zeroing makes it loud for free: `cell_size == 0` is exactly the value
`clay_layer_consolidation_cost` and `clay_layer_consolidate` already refuse
(`if (!(params.cell_size > 0.0f)) return std::nullopt;`). A host that never
reads `*out_advises` gets `CLAY_ERROR_INVALID_ARGUMENT` and an unchanged
document. There is no reading of the zeroed struct under which anything bakes.

So `*out_advises == 0` means: **the other two out params carry no information,
and the params are already refusable.** Not "bake at these anyway", not "some
fields are stale".

## 3. What `*out_advises` actually means

It is 1 when **both** hold:

1. `clay_layer_field_report` at the same `advise_below_step_scale` sets
   `advises_consolidation` — degraded, and by a mechanism a bake cures (#387).
2. The projected `safe_step_scale` in `out_cost` is at or above
   `advise_below_step_scale`.

Condition 2 is new and it is the reason this is not just a params helper. A
sampled volume declares `sqrt(3)` times its samples' Lipschitz, so a
consolidated layer's step scale is **at best `1/sqrt(3) = 0.577`** — the number
`clay_layer_consolidate`'s own header gives. A host whose frame budget wants
`0.8` is asking for something no bake can deliver, and handing it params would
trade a parametric layer for a dense volume and still miss the threshold.

Condition 2 also subsumes cases nobody would enumerate by hand: an
already-consolidated layer whose samples are still steep, a layer whose contents
resample no better than they evaluate, a bake whose redistance cannot recover
the shape. All of them come out as "the projection does not clear your
threshold", which is the honest answer and the same answer for all of them.

**This is where the Lipschitz enters** — as `out_cost.sample_lipschitz` and the
`safe_step_scale` derived from it, measured on the samples a real bake produced.
That is a slope, measured, not a declared stepping bound. Section 1 refused the
declared one; this uses the measured one, and the two are not the same number.

**A risk the implementer must close.** Condition 2 compares
`out_cost.safe_step_scale` — a number for the volume in isolation — against a
threshold the host will re-check with `clay_layer_field_report` on the
consolidated layer. Those two must be the same number, or the advice promises
something the follow-up report contradicts. The bake works in the local frame,
so a layer transform, a mirror mode or a layer-level modifier could compose
something the projection does not see. The implementation task is to assert
equality on a mirrored and non-uniformly-scaled layer, and where they differ,
key `*out_advises` on the number that actually results, not the one that is
easy to get.

### Rejected: making `out_cost == NULL` a cheap path

Tempting, since deriving the params is a bounds walk and the cost is a full
sampling pass. Rejected: `*out_advises` is *defined* by condition 2, so a call
that skipped the sampling would answer a weaker question under the same name and
a host could not tell which it got. `out_cost` may still be NULL — a host may
not want the numbers — and the call costs the same. The header says this
outright.

## 4. What it does not do

**It does not bake.** It runs `clay_layer_consolidation_cost`'s path internally:
the real bake with the result thrown away. The document is not changed.

**It does not sever an instance.** `clay_layer_consolidate` severs a shared edit
list before it bakes; `clay_layer_consolidation_cost` does not, "because it does
not bake", and this does not for the same reason. Asking whether a bake is
*advisable* must not unlink a subtool any more than asking what it costs does.

**It refuses before it works.**

| input | answer |
|---|---|
| unknown layer | `CLAY_ERROR_NOT_FOUND` — "no such layer" and "not advised" are different answers |
| `advise_below_step_scale <= 0` | `CLAY_ERROR_INVALID_ARGUMENT`, before any sampling |
| null `out_params` or `out_advises` | `CLAY_ERROR_INVALID_ARGUMENT` |
| non-SDF layer | success, not advised, zeroed |
| protected layer | success, not advised, zeroed |
| empty layer, or infinite bounds | success, not advised, zeroed |

The threshold refusal deviates from `clay_layer_field_report`, where a zero
threshold legitimately means "measure without asking for advice". This call has
nothing to return without a threshold: every output it has is defined against
one, so a zero would buy a full sampling pass to be told nothing. Refusing is
cheaper and cannot be misread.

A non-SDF or protected layer is *not* refused, following the rule
`clay_layer_warp_cost_get` set: a host walking a stack of mixed kinds should not
have to special-case them, and on a protected layer the follow-up call is
refused anyway — advising a bake that `clay_layer_consolidate` will reject is
bad advice, not an error condition.

## What the header must say it does NOT promise

- **Not optimal.** It is derived from the layer's extent and what the layer
  already holds. A host that knows its viewport, its zoom, its device's memory
  and what the artist is about to do next can do better, and should override.
  The params are a caller-owned struct precisely so it can.
- **Not stable.** Adding one small item changes `F` and therefore the advised
  cell size. A host that means to re-bake the same box later must **store the
  params**, not re-ask.
- **Not a memory bound.** The clamp bounds the grid; `out_cost.bytes` is the
  memory, and it is the number to refuse on.
- **Not cheap.** It costs a full sampling pass, NULL `out_cost` included. Not a
  per-frame call.
- **Not a fidelity claim.** A bake at the advised cell size still discards every
  parameter of every item absorbed and every colour but the first. The advice is
  about marching cost, not about what the shape will look like.
- **It carries no region and pins none.** `clay_layer_consolidation_cost` warns
  that a repeated bake pads the previous padding, because a volume's geometric
  bound is its whole sampled box. The advice is derived from the layer's current
  bounds, so re-asking after an advised bake advises a slightly larger box each
  time. A host consolidating the same region repeatedly must pin the region
  itself; this call will not do it for them.
