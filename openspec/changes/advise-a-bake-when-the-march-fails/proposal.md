# Proposal: advise a bake when the march has already failed

## Why

A sculptor doing three or four ordinary Move dabs on a plain sphere waits
almost a second per dab. Measured in the live ClaySpaceDesktop application,
driven through its own MCP — five dabs on the user's own document, each a real
gesture, every one reporting `stalled: true`:

| dab | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| `stroke.end` | 255.6 ms | 275.1 ms | 355.8 ms | 522.2 ms | **739.1 ms** |

The application's own phase split puts `engine edit` at a median of 42 ms with
a **p95 of 446 ms and a worst of 2339 ms**, and `engine mesh` at a p95 of
567 ms. Nothing in the edit explains that spread. What explains it is the
field the edit leaves behind.

**Every Move dab adds a grab to the layer's deformer chain, and the safe step
scale is a product over that chain.** So it decays geometrically, and every
sphere-trace afterwards — the viewport, each pick on each cursor move, each
re-mesh — pays proportionally more iterations. The stall lands *after* the edit
returns, which is why it reads as lag rather than as a slow tool.

**Symmetry doubles the rate.** The engine emits one grab per mirror image, the
starting document has X on, and so the default document degrades twice as fast
as any measurement taken without it. That is why the report says "3 or 4 dabs".

### The engine already knows, and already declines to help

`clay_layer_field_report` sets `degradation = CLAY_DEGRADATION_DEFORMERS` on
exactly this shape. But `advises_consolidation` is
`degraded && volumes` (`src/scene/consolidate.cpp:214`), and a single drawable
carrying a brush chain has no volumes — so the advisory is withheld, **by
design**, on the authority of a measurement recorded in the enum itself:

> Consolidation is NOT the cure and measured 6x WORSE on a real gesture: it
> swaps a cheap analytic item for a dense volume, and the marching win — a 29x
> better step scale — is swamped by what the volume costs per sample.

That measurement is sound and this proposal does not dispute it. It names a
**regime**: 29x. A real session reaches far past it.

### The crossover, measured

`benchmarks/move_collapse_crossover_probe.cpp`, this machine, Release, CPU.
One sphere, N Move dabs at fresh centres, then the same 4096 rays cast two
ways — against the parametric layer, and against a consolidated copy:

| dabs | chain | step scale | ray parametric | ray baked | ratio |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 0.727273 | 4.67 ms | 7.30 ms | 0.64x |
| 2 | 2 | 0.528926 | 3.92 ms | 7.18 ms | 0.55x |
| 4 | 4 | 0.279762 | 7.80 ms | 8.35 ms | 0.93x |
| **6** | **6** | **0.147973** | **17.05 ms** | **9.01 ms** | **1.89x** |
| 8 | 8 | 0.078267 | 37.47 ms | 9.03 ms | 4.15x |
| 12 | 12 | 0.021896 | 130.25 ms | 9.63 ms | 13.52x |
| 16 | 16 | 0.006126 | 202.06 ms | 10.24 ms | 19.73x |

**The curves cross at a step scale of about 0.15.** Below it the bake wins, and
the margin grows without bound while the baked arm stays flat — a redistanced
volume reports 0.577 at every depth, so its march does not care how many dabs
preceded it.

**And at 16 dabs the parametric arm finds NO SURFACE AT ALL.** 0 hits of 1844.
The march exhausts its iteration budget before reaching the form. Past some
depth the field does not merely become expensive to render; it renders
**wrong**, and no amount of waiting produces the right picture.

### What the bake produces is a constant, and that is why a fixed floor works

The obvious objection is that 0.577 is this sphere's number: if a denser form or
a finer cell baked to something lower, the crossover would move and a fixed
floor would not follow it. Measured across an 8x range of cell sizes, at 8 dabs:

| cell | baked step scale | ray (baked) | bake ms | bytes |
|---:|---:|---:|---:|---:|
| 0.0400 | 0.577350 | 8.97 ms | 137.8 | 840,476 |
| 0.0200 | 0.577350 | 9.90 ms | 902.1 | 3,348,596 |
| 0.0100 | 0.577341 | 9.13 ms | 6,247.7 | 13,478,328 |
| 0.0050 | 0.577332 | 12.34 ms | 48,227.4 | 55,553,252 |

**A 1.00x spread.** And the value is 1/sqrt(3) to six figures, which is not a
coincidence: redistancing produces a unit-gradient field, and the safe step
scale for a unit-Lipschitz field on a three-dimensional lattice is 1/sqrt(3).
The baked arm's step scale is a property of *redistancing*, not of the fixture.
That is what makes a constant floor defensible rather than a fitted number.

**Stated as reasoning where it is reasoning:** only the cell size was varied.
Form complexity was not. If the mechanism is redistancing then complexity
cannot move it either, but that is an argument and not a measurement, and the
task list treats it as one.

**What does move, sharply, is the price of the cure.** Bake time goes 138 ms to
48 seconds and memory 0.8 MB to 55 MB across those same four rows, for no
marching benefit at all. The advisory must therefore keep reporting a cost the
host can refuse — `out_cost.bytes` is the number to refuse on, and nothing here
changes that.

### The threshold belongs on the step scale, not on the chain

Chain depth is the wrong variable and this proposal does not use it. Two runs
with identical chain lengths reached 0.296 and 0.000837 depending on how much
the grabs **overlapped** — dabs spread around an equator barely share items,
dabs worked into one area share all of them. `safe_step_scale` already absorbs
that, it is already computed, and it is already what the ray cost depends on.
A threshold on it transfers across fixtures; one on depth does not.

## What Changes

**One condition, and the sentence that justifies it.**

```c
/* was:  advises_consolidation = degraded && volumes;  */
out.advises_consolidation =
    degraded && (volumes || out.safe_step_scale < kMarchFailsBelow);
```

`kMarchFailsBelow` is derived from the table above, not chosen: the crossover
sits at ~0.148, and the floor is set at **0.125** so that a layer is advised
only once the bake is a clear win rather than a coin flip. It is a *scene-model*
constant beside the other bounds, not a caller's tolerance —
`advise_below_step_scale` stays what it is, the caller's own "tell me when the
field is degraded", and this is the separate question of whether the cure
applies.

**`CLAY_DEGRADATION_DEFORMERS`'s documentation gains the regime it was missing.**
Its present text says consolidation is not the cure, flatly. It becomes: not
the cure *above* the floor, which is where it was measured, and the cure below
it, with this table as the reason and the no-hit row named.

**`clay_layer_consolidation_advice` follows automatically.** It is defined
against `report_layer(...).advises_consolidation`
(`src/scene/consolidate.cpp:754`), so a layer that becomes advised starts
receiving parameters and a cost with no change of its own.

## What is NOT changed, and why

**Nothing bakes on its own.** This changes an advisory. The engine does not
consolidate a layer because it has become expensive; a bake discards every
parameter of every item it absorbs and that is a decision for the person
sculpting. ROADMAP #15 settled this — "a recommendation the host can act on
with one call, not an autonomous action" — and it stays settled.

**The 6x-per-sample cost is not disputed or reduced.** A baked volume really is
dearer per sample than an analytic sphere. The claim here is only that at a low
enough step scale it is asked for far fewer samples, and the table is where
that stops being a loss.

**Not a collapse that keeps the layer parametric.** That would be the better
cure — N grabs resolved into fewer without going to a volume — and it stays
unbuilt. `price-the-warps-a-layer-carries` declined both available forms as
unsound: warps do not compose in general, and baking a displacement into an
item is exact only where the warp is rigid over that item's support. This
proposal is the cheap thing that helps now, not the right thing that needs
research. **It should not be read as closing that question**; the 16-dab
no-hit row is the strongest argument yet for funding it.

**Not a change to how grabs accumulate.** A separate defect — coalescing keys
on bit-exact centre and radius, so a pressure-driven radius or a
following centre stacks one warp per frame — is real, measured at 101x, and
belongs to its own change.

## Capabilities

### Modified Capabilities
- `scene-model`: a layer degraded past the point where sphere-tracing fails is
  advised to consolidate even when it has no volumes to absorb.
- `c-abi`: the deformer degradation names the regime its advice holds in.

## Impact

- `src/scene/consolidate.cpp` — the condition and the constant.
- `include/clay/scene/consolidate.h` — the constant's declaration and its rationale.
- `bindings/c/clay.h` — `CLAY_DEGRADATION_DEFORMERS`'s text.
- `tests/unit/` — the threshold's behaviour either side of the floor, and that
  an advised bake actually cures what was named.
- `benchmarks/move_collapse_crossover_probe.cpp` — already written; this is the
  table's source and it stays so the number can be re-derived.
- **No ABI change.** No new entry point, no struct growth, no signature change.
  A host that reads `advises_consolidation` starts getting `true` where it
  previously got `false`, which is the point.
