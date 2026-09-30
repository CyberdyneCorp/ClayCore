## Where the reach comes from
`apply_edit` already takes `command_influence_bound` on both sides of every command before deciding it was a `TailAppend`. That box goes into `touch_appended` and is stored index for index with the log. Nothing is recomputed under `cache_mutex_`, and an intersect's box is the layer extent, which is exactly the region its append changes.

## Why not simply drop a lagging seed
Dropping is correct, and it was measured as the mutant `return rev + 1 == next`. After a stroke most seeds on the model sit at the revision before the stroke, because a refill re-stamps only the bricks it filled. Dropping them sends every later edit there down the full walk. On the regression fixture the resumed count for a re-dirtied underside went from 108 bricks to 0 (all 144 refilled). The per-append check keeps those seeds.

## Cost
The check runs once per untouched clean seed that lags. The first version scanned the log from each seed's revision, which made the region edit cost seeds x appends. On a filled unit sphere (dim 8, voxel 0.02) with N dabs on its cap, each refilled on its own, the first far move afterwards took:

| dabs | main (unsound) | linear scan | range-union tree |
|---|---|---|---|
| 200 | 0.21 ms | 3.98 ms | 0.52 ms |
| 1000 | 0.21 ms | 14.5 ms | 0.89 ms |
| 3000 | 0.30 ms | 42.9 ms | 1.27 ms |

The scan grew with the length of the stroke, under `cache_mutex_`. So the region edit now builds a binary tree of unions over the log's index ranges (`ReachTree`, O(n)) once, and each seed asks it whether any reach from its own index on meets its cull region. A node's union only prunes; the answer is decided at a leaf, so the result is exactly the scan's. Consecutive dabs lie close together, which keeps a range's union tight. An infinite reach is widened to the whole of space when it is logged, so no union can prune it.
