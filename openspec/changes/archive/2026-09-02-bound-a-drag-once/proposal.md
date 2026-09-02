# Bound a drag once

## Why

`layer_move_surface` has been ~1.4x slower since v0.52.2 and nothing said so
(#358). Measured on an M2 Max through the C ABI, over a 1,000-item document:

| build | per drag |
|---|---:|
| `bc0b788` — last good | 0.092 ms |
| after #314 | 0.093 ms — innocent |
| **after #316** | **0.123 ms** |
| after #322 | 0.123 ms — innocent |
| main today | 0.134 ms |

`#316` gave `apply_edit` a region-limited invalidation, which was a large win
for the case it was written for: an edit whose reach is known no longer drops
every stored brick seed. The cost is two `scene::command_influence_bound` calls
and one seed-store walk PER COMMAND.

A Move drag issues **one command per node it reaches** — 257 of them over a
1,000-item document — so it pays that 257 times for one gesture. Isolated: the
bounds are about three quarters of the loss, the seed-store walk the rest.

## What changes

A gesture that can state the region it reaches now does so once, for all of its
commands, instead of having one derived per command.

`clay_layer_move_surface` can state it exactly: the warp's region weight is zero
outside `radius` of the drag centre, and a point whose weight is zero is not
moved, so a sample there evaluates the same nodes at the same place. The
reachable region is the drag's own ball, dilated by the displacement as margin
— O(1), whatever the drag touched.

That is **cheaper and tighter**. Per command `apply_edit` unions the whole
influence bound of each moved node, so a drag catching the edge of 257 items
spread through the volume invalidated the union of 257 whole items, far more
than the ball the drag actually reached.

Result: 0.134 -> 0.097 ms, within ~5% of the pre-#316 figure.

## The benchmark is half the change

No CPU benchmark covered a Move drag. That is why this survived four releases:
the only gate that saw it was the device suite, which cannot run in CI, and the
device gate's own noise floor then suppressed the failure (over the 1.4x
tolerance, under the 0.05 ms floor).

`BM_MoveDrag1000` / `BM_MoveDrag10000` close that. The ceiling is deliberately
generous and cannot catch a 1.25x move — that is inside the spread between a
developer machine and a shared runner, and the file says so rather than
implying a gate it cannot be. What it buys is a one-command local A/B for
anyone touching `apply_edit`, and a row in CI output where there was silence.

## Also reverted here

`move_drags` goes back to one Move application per stroke. It was delivered in
four sub-steps for one release to lift it over the gate's floor (#337); at
thirty-two stacked warps the layer reports a Lipschitz bound of 1931 and a safe
step scale of 0.0005, the gallery bundle went from 6.2 s to 10.7 s, and
`testBrushSessionsAndGallery` was killed by jetsam. The case is REPORTED rather
than GATED again, and the coverage check says so.

## Impact

- Affected specs: `c-abi`
- Closes #358. `sdf_move` returns to its committed baseline, so the held
  exemption in `tools/check_doc_latency.py` is removed and `docs/09` quotes the
  baseline again.
