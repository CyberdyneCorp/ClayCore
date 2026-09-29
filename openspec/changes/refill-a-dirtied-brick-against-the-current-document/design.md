## Where the reach comes from
`apply_edit` already takes `command_influence_bound` on both sides of every command before deciding it was a `TailAppend`. That box goes into `touch_appended` and is stored index for index with the log. Nothing is recomputed under `cache_mutex_`, and an intersect's box is the layer extent, which is exactly the region its append changes.

## Why not simply drop a lagging seed
Dropping is correct, and it was measured as the mutant `return rev + 1 == next`. After a stroke most seeds on the model sit at the revision before the stroke, because a refill re-stamps only the bricks it filled. Dropping them sends every later edit there down the full walk. On the regression fixture the resumed count for a re-dirtied underside went from 108 bricks to 0 (all 144 refilled). The per-append check keeps those seeds.

## Cost
The check runs once per untouched clean seed that lags. The log's running union rejects a brick that no append came near without a scan, and after a stroke that is almost every brick. Only bricks inside the stroke's box scan the appends from their own revision onward.
