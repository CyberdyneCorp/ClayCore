# Proposal: cache the cross-level rim

## Why

`MultiresSculptor::stamp` calls `bind()` on every dab, and a binding that is
still good — same level, same cache generation — still ends by handing the level
sculptor `&surface_.cross_level_at(level)`. `cross_level_at` built the
neighbourhood once and then called `refresh_cross_level` on EVERY access: a walk
over the region rim re-subdividing every outside vertex from the parent's
positions, whether or not the parent had moved.

The comment beside it declined to track them, and the objection was real:
"tracking them would be a fourth revision counter guarding a walk over the region
rim", and handing back what was last read would be an answer a reader cannot
distinguish from a current one. #493 prices that trade rather than overruling it.

**The refresh scales with the RIM; the dab scales with the FOOTPRINT.** So its
share grows with the region at a fixed brush size — with the thing regional
refinement exists to make affordable. Measured on this branch, min of 200
interior dabs, two builds differing in one condition, load average 0.74 before
and after every run:

| region | level | stored | outside | rim walks / 200 dabs | min dab | ratio |
| ------ | ----: | -----: | ------: | -------------------: | ------: | ----: |
| 4x4    | 3     |  1,089 |     136 | 1,319 -> 0           | 0.02962 -> 0.02440 ms | 0.82 |
| 8x8    | 4     | 16,641 |     520 | 1,759 -> 0           | 0.14273 -> 0.10763 ms | 0.75 |

Both builds moved the same vertices — 5,960 and 27,579 over the 200 dabs — so
the difference is the rim walk and nothing else.

## What changes

- **`MultiresLevel::positions_revision`, and it rides on the queue that was
  already load-bearing.** A level's `pending` list — the vertices it owes the
  level above — becomes private behind `note_moved`, `note_moved_all` and
  `clear_pending`, and the revision moves with the queue. The outside positions
  of level N are `subdivide_positions` of level N-1's positions, so they go stale
  exactly when N-1 has something to push up: a writer that skips the door has
  already left N with stale `subdivided` and stale frames, which existing gates
  read. There is no way to make the neighbourhood stale on its own.
- **`LevelCache::cross_parent_revision`**, beside the neighbourhood it describes,
  so a cache drop or `release_cross_levels` takes both.
- **`cross_level_at` and `cross_level_of` refresh only on a mismatch.** Same
  values, same order, same bits.
- **`MultiresEvalStats` gains `cross_level_reads` and `cross_level_refreshes`**,
  so the caching is gateable as a MECHANISM. A correctness test passes happily
  over a cache that has silently stopped caching.

## Approach

The engineering guide's rule — the mutation owns its invalidation signal — picks
the shape, as it did for `own-the-mesh-invalidation-signal`. The rejected
alternative is spelled out in the design and gated against: a counter bumped
where the positions are ASSIGNED (`apply_detail_all`, `gather_class_positions`,
`subdivide_positions`, `subdivide_positions_partial`, the absorb read-back).
It passes every other gate and silently serves a stale rim after a stroke on the
CAGE, because at level 0 the brush writes the cache's `Mesh` directly and
`absorb_base_edit` READS those positions into the cage rather than writing them
back. Measured under that alternative: 0 of 13 outside positions moved where 13
should have.

## Non-goals

- Changing what the neighbourhood contains, its order, or when it is built. The
  existing gate "regional: the outside positions follow a stroke on the level
  below" passes unchanged.
- Skipping the refresh when the parent moved somewhere the rim does not read.
  The signal is per level, not per vertex; a finer one costs a walk over the
  thing it is trying to avoid walking.

## Impact

`mesh-multires` gains the freshness rule the neighbourhood now keeps. No ABI
surface changes and no format minor: `MultiresEvalStats` is a C++ struct with no
C mirror, and pyclay reports it as a dict.
