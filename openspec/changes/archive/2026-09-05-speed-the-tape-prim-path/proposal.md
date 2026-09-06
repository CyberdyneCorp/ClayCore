# Proposal: forty prims are paying for one prim's colour

## Why

`mask_extrude` is over its `operation` budget on the reference iPad — 3787 ms
p95 against 3751 ms, where v0.30.0 measured 2501 ms. It was found by the device
gate, survived `fix-consolidation-colour-cost` untouched, and a bisect put it
somewhere that change does not reach.

Medians of three at 1000 stamps, the device case's fixture through the C ABI:

| commit | | 1000 stamps | step |
|---|---|---|---|
| `50a301a5` | v0.30.0 | 3250.1 ms | — |
| `196d403` | before colour | 3281.3 ms | x1.01 |
| **`ac7460a`** | **the colour out-parameter** | **3751.7 ms** | **x1.14** |
| `85f1679` | the producers write colour | 3785.0 ms | x1.01 |
| `71118c1` | after the colour pass was pooled | 3806.7 ms | x1.01 |
| `b1868d4` | main | 3825.7 ms | x1.00 |

One step, and it is not the colour pass. It is the parameter.

## What the spec already said

`sdf-kernels`, the requirement `ac7460a` was written against:

> Prims other than the volume SHALL NOT pay for this. The colour output SHALL
> be an out-parameter the volume opcode may write and every other prim
> ignores, rather than a wider return type every prim constructs.

The implementation put that out-parameter on `ctape_prim_dist` — the ~40-branch
if-chain over every primitive opcode. So every prim carries a pointer that
exactly one of them writes through. "Ignores" was satisfied in behaviour and
broken in cost: on a document of 1001 spheres the volume branch is never
entered, and the measurement above is all plumbing.

The requirement was right. This change makes the code meet it.

## What

**Lift the volume opcode into `ctape_volume_dist`, and dispatch.**
`ctape_prim_dist` loses the out-parameter and keeps the analytic prims;
`ctape_prim_local` sends `ctape_volume` to the new function and everything else
to the old one. The volume block moves verbatim — it is the only path that ever
wrote through the pointer, so behaviour is identical by construction rather
than by argument.

This is what the requirement's own wording implies. An out-parameter "every
other prim ignores" is a parameter every other prim should not have.

## Results, prototyped before this was written

Mac, medians of three at 1000 stamps:

| | mask_extrude | sdf_consolidate |
|---|---|---|
| main | 3829.1 ms | 370.1 ms |
| **this change** | **3288.6 ms** | **324.4 ms** |
| before colour (`196d403`) | 3348.8 ms | 243.4 ms |

`mask_extrude` returns to **below** its pre-colour cost: the whole `ac7460a`
regression is recovered, not part of it.

`sdf_consolidate` improves too and does not return, because its remaining cost
is the colour PASS rather than the parameter — that is
`fix-consolidation-colour-cost`, and the two are complementary. Neither is
sufficient alone: 370 -> 324 here, 370 -> 278 there, and the pre-colour figure
is 243.

Verified in the prototype: 1008/1008 unit tests, `check_kernel_dialect` passes
for cpu, cuda and metal profiles plus the OpenCL and Vulkan amalgamations.

## Non-goals

- **The colour pass.** Different defect, different change, already open.
- **Widening what colour can do.** No opcode gains or loses the ability to
  report colour; a volume with samples still reports them, and every other prim
  still reports the item's constant. The parity suite is what holds that.
- **Sizing the win on device from the Mac.** The Mac has consistently
  understated this cost — x1.18 here against the device's x1.51 for the same
  range — so the device gate measures the result rather than predicting it.
