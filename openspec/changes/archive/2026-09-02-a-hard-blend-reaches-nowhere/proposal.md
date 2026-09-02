# Proposal: a `k` a hard profile never reads should drag no chain

## Why

Issue #335: ClaySpaceDesktop's move brush costs 1.76x on v0.52.2 what it cost
on v0.39.0, on the frame path the release notes record at 1.34x *faster*. The
report attributes it to #292's straddler pass — the change that made
`clay_brick_cache_mesh` march the cells a brick with no stored lattice owns.

**It is not the meshing.** Reproduced with a 300-node smooth-union ball, a
12-segment move drag accumulating grab deformers, and a per-brick refill timed
the way a host pays it. On byte-identical cache contents producing byte-
identical output (1,993,615 triangles either way), `mesh_bricks` is **1.24x
faster** on `main` than at `v0.39.0`. The frame-path claim holds.

**It is #282's chain pad** (`a02c874`), and that part is the accepted cost of a
correctness fix rather than a defect. Same document, same bricks, same regions:

| | tape instrs | compile | eval | mesh | total |
|---|---:|---:|---:|---:|---:|
| v0.39.0 | 773,192 | 1183 ms | 2711 ms | 1524 ms | 5440 ms |
| main, blend pad zeroed | **773,192** | 1170 ms | 966 ms | 1195 ms | 3357 ms |
| main | **1,917,099** | 2594 ms | 1830 ms | 1930 ms | 6373 ms |

Zeroing only the blend term restores `v0.39.0`'s tape count exactly, and the
whole regression with it.

**What IS a defect turned up beside it.** The pad is the largest single-item
reach in the LAYER, and reach is spelled `max(support, k)` because a hard
profile has zero support — so a `k` left on a hard node set the pad for every
brick in the layer out of a blend that drags nothing. `ctape_smin_m` hands a
hard profile back a step: the running value is a plain `min()` and moves by no
`k` at all, and `emit_combine` says the same with its own `smooth` test. That
`k` is what a UI which keeps its blend slider when the artist picks "hard"
writes.

On a 200-node chain alternating hard `k = 0.5` with smooth `k = 0.05`, where
the hard node is therefore the layer's maximum: **35,068 tape instructions
against 19,724 over 200 bricks, a factor of 1.78.**

## What Changes

`scene::chain_drag_reach(const Node&)` is how far a node can drag a chain's
running value, and it answers 0 for a hard profile that is neither `Paint` nor
an extended mode. `blend_cull_pad` and `cull_pad` are its only callers.

**A node's own bound is NOT changed, and that is the whole of what was learned
here.** The first version of this proposal took `max(support, k)` off the
node's bound as well, on the reasoning that a hard combine reads no `k`. That
reasoning is right about the blend and wrong about the bound: in a MIXED chain
the dilation is doing a second job — margin for the drag a node's *smooth*
neighbours apply to a running value it contributed to. Measured on a 12-node
hard/smooth document, band-clamped disagreements against the full tape went
**540 to 10,105** (worst 0.0065 to 0.0088), where narrowing the pad alone
measures **540 to 540, exactly**. x86-64 passed the thin sampling in the
regression test; an arm64 CI runner did not. The bound keeps its dilation and
`bounds.h` now says why, so the next reader does not repeat it.

`BM_DeepDocCullPlanned2000K06` measures the existing cull document at twice its
blend radius, gated as a ratio against the `k = 0.03` row. The pad is `4k`
against a region that is a fixed brick plus band, so its cost is superlinear in
`k` and every cull row we had sat at one value of it:

| k | pad | tape growth | wall-clock |
|---:|---:|---:|---:|
| 0.015 | 0.06 | 1.41x | 1.20x |
| 0.03 | 0.12 | 1.92x | **1.37x** |
| 0.06 | 0.24 | 2.50x | **1.87x** |
| 0.12 | 0.48 | 3.08x | 2.73x |

The `k = 0.03` row is the "20-35%" #282 recorded. The gate measured the cheap
end of a curve, which is why a 1.76x regression on a sculpt blending at 0.06
was invisible to it.

## Impact

**The pad gets smaller, and only where a hard node was setting it.** A layer
whose largest reach comes from a smooth node — which is every document that
blends at all — sees no change, and no existing test moved. A layer of hard
nodes carrying a stale `k` pads nothing at all, which is what it always should
have done: #282 is explicit that hard unions have no chain drag, since `min()`
is exact and associative at any length.

**No bound moves and no reported value changes.** `clay_layer_node_influence_bound`,
`clay_tape_info` and every geometry bound answer exactly what they answered
before; only the per-brick cull region narrows.

**The field does not move.** Held over a 200-node mixed hard/smooth chain, 200
bricks and 8,000 band-clamped samples, with the culled tape under half the full
tape per brick so the sweep is not passing vacuously.

## Non-goals

**Tightening #282's pad itself.** Its own sweep found `band + 0.12` sufficient
at `k = 0.06` where the landed pad is `0.24`, and a per-batch local maximum
would beat the layer-wide one — but that commit also says the pad "is not a
proof for an arbitrary chain", and this change's own first attempt is the
argument for taking that seriously. #335 stays open for it.

**Making the pad cover every document.** It already does not: a 12-node chain
blending at `k = 0.5` disagrees with the full tape 540 times in 800,000
in-band samples on `main`, and identically after this change. That is the
heuristic's known limit, not a regression, and it is why the regression test
sweeps a document the contract holds on rather than one that merely looks
adversarial.
