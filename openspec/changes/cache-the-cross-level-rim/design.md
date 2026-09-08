# Design: cache the cross-level rim

## 0. What runs today, measured before anything was designed

`bind()` on a still-good binding calls `set_cross_level(&cross_level_at(level))`,
and `cross_level_at` called `refresh_cross_level` unconditionally. Instrumenting
the two accessors with a read and a refresh counter, over 10 interior dabs on the
4x4-at-level-3 fixture (1,089 stored vertices, 136 outside, 132 derived faces):

    reads = 48   refreshes = 48   full = 0   partial = 0   vertices_evaluated = 0

48 rather than 10 because a crossing stamp asks at every level it can write:
`bind()` asks at the bound level, and `stamp_coarse` asks at each coarse level
plus once more through the evaluation each of those triggers. So the walk ran
4.8 times per dab while the hierarchy evaluated nothing at all.

## 1. Every writer of a level cache's `mesh.positions`, enumerated

The signal needed is "did the parent level's positions change since this
neighbourhood was last refreshed". The enumeration is the work; the fix is four
lines.

| # | writer | file | queues `pending`? |
|--:| ------ | ---- | ----------------- |
| 1 | `apply_detail` | multires_eval.cpp | at its callers |
| 2 | `apply_detail_all` | multires_eval.cpp | caller sets `pending_all` |
| 3 | `apply_base_layers` | multires_eval.cpp | at its callers |
| 4 | `gather_class_positions` in `evaluate_level0` | multires_eval.cpp | yes |
| 5 | `restore_positions` (level 0 branch, from the cage) | multires_eval.cpp | NO, deliberately |
| 6 | the read-back loop in `absorb_level_edit` | multires_eval.cpp | yes |
| 7 | `set_detail` | multires_eval.cpp | yes |
| 8 | `set_base_position` | multires_eval.cpp | yes |
| 9 | `MeshSculptor` through `level_mesh(level)` — the bound level | multires_sculpt.cpp :184 | through `absorb_level_edit` |
| 10 | `MeshSculptor` through `level_mesh(c.level)` — a coarse level | multires_sculpt.cpp :419 | through `absorb_level_edit` / `restore_level_positions` |
| 11 | the direct write of `fine_targets_` | multires_sculpt.cpp :511 | through `absorb_level_edit` |

Rows 9-11 are the ones a grep of `multires_eval.cpp` misses, and row 10 is the
one that decides the design. `MultiresSurface::level_mesh` is the only door that
hands out a mutable `Mesh&`, and outside the tests it has exactly those three
callers; the C ABI's `clay_multires_copy_level_mesh` copies.

### 1.1 Why the enumerable sites are not enough — the level-0 hole

At level 0 the brush writes the cache's `Mesh` directly and `absorb_base_edit`
READS those positions into `State::base` rather than writing them back. So a
revision bumped at the assignment sites never moves for a cage stroke, while
level 1's outside positions — which are `subdivide_positions` of level 0's —
have all moved. Nothing crashes; the boundary normals and frames above are
quietly built against a surface that moved.

This is not a hypothesis. Implemented and run: gate A (no walk on an interior
dab) passes, gate B (a walk after a stroke at level 2) passes, and
"regional: a stroke on the CAGE moves the level above's outside positions"
fails with 0 of 13 outside positions moved and values that differ from a cold
build.

### 1.2 Why `level_mesh` is not the bump site either

The airtight-looking answer — bump when the mutable `Mesh&` is handed out — was
implemented on paper and rejected on the numbers. `stamp_coarse` calls
`level_mesh(k)` for every coarse level on every dab, so on the fixture above it
would bump levels 0, 1 and 2 per dab and the bound level's neighbourhood would
refresh every dab anyway. The win would be zero.

## 2. The signal: the queue that was already there

`MultiresLevel::pending` already means exactly "vertices at THIS level that
changed and have not yet been pushed to the level above", and `pending_all` the
same about every vertex. The outside positions of level N are
`subdivide_positions(parent.topology, parent.conn, parent.positions,
outside_layout)` — the same call, on the same input, that produces level N's own
`subdivided` from a different index set. **So the neighbourhood goes stale
exactly when the parent has something to push up.**

That makes the pairing structural rather than a convention. `pending` and
`pending_all` become private; `note_moved`, `note_moved_all` and `clear_pending`
are the only doors, and the first two move `positions_revision`. A writer that
moves a level's positions and skips them has also skipped the queue, which leaves
the level above with stale `subdivided`, stale frames and stale detail — visible
to every existing multires gate. The revision cannot be forgotten on its own.

The thirteen sites that mutate the queue are in `multires_eval.cpp`,
`multires.cpp`, `project.cpp` and `sculpt_layer_eval.cpp`, and every one of them
now goes through a door.

## 3. Why `restore_positions` does NOT bump, and why that is not a hole

`restore_positions` writes back what the STORED coefficients reconstruct to.
Every acknowledged write to a level's positions ends by reading those same
coefficients back through the frame — the read-back in `absorb_level_edit` says
why, and `apply_detail` is the same expression — so "what the coefficients
reconstruct to" IS what the array held at the current revision. The raw brush
write it undoes never moved the number. The pair is therefore a no-op on the
content and a no-op on the revision, together.

It is also the whole win. On an interior dab the coarse sculptor stamps, every
vertex it moved turns out to belong to the level above, `partition_coarse_write`
sends all of them to `restore_level_positions`, and `c.written` is empty. A
revision that bumped on any write would bump there and the bound level would
refresh every dab.

## 4. What a stale answer would have looked like

The failure mode is silent: a cached neighbourhood served after the parent moved
gives a reader stale outside positions, which are used to complete boundary
normals and frames. That is the defect #481 spent three review rounds removing.
So the gates assert the MECHANISM — `cross_level_refreshes` ran or did not —
beside the values, because a correctness test passes happily over a cache that
has silently stopped caching, and a value test alone cannot tell "did not need
to refresh" from "no longer refreshes".

`cross_level_reads` is there so the mechanism gate cannot pass vacuously: zero
refreshes means something only beside a non-zero count of asks.

And an ask counts only where there is a neighbourhood to ask for. Both accessors
test `level_is_self_contained` — from the TOPOLOGY, and ahead of the counters —
so a uniform hierarchy, whose every level stores every patch, reports zero
however hard it is sculpted. Nesting that test inside "the parent is not
resident", which is where `cross_level_at` first had it, counted one read per dab
through `MultiresSculptor::bind` and none through `cross_level_of`, from the same
surface: the parent is evaluated on the normal path, so the early return was
never reached.

## 5. Ordering, and the two places the revision is read

`evaluate_up_to` walks levels 1..target in order and clears level `l-1`'s queue
after level `l` has consumed it — `clear_pending`, which does NOT move the
revision, because being pushed up is not a change. So the revision a
neighbourhood records is read while the parent is evaluated and current, from
`cross_level_of` inside the evaluation and from `cross_level_at` at the public
door, and both write it back on the same line they refresh.

A cache drop is conservative in the safe direction: `positions_revision` lives on
`MultiresLevel` and survives, `cross_parent_revision` lives in `LevelCache` and
does not, and a released parent that is rebuilt goes through `full_evaluate` and
`note_moved_all` — so it refreshes once more than it strictly needs to rather
than once less.

## Where this change does NOT help, said here because it was mis-said once

**A uniform hierarchy sees no improvement, because it saw no cost.**
`level_is_self_contained(child, keep)` is `keep.empty() || child.dense()`, so
every level of a whole-surface hierarchy is self-contained, and `cross_level_at`
returns the empty neighbourhood on that test BEFORE it counts a read and before
it builds anything. The rim work was already zero there and a cache has nothing
to save.

The measured wins — 1,319 and 1,759 rim walks per 200 dabs going to 0 — are on
REGIONAL fixtures, where levels genuinely have vertices outside themselves.

**The phrase that caused the confusion is worth recording, because it is this
repository's recurring defect in a sentence rather than in an API.** Describing
the fix as reaching "the shared path" was told to the consuming host, and
"shared" carried two scopes:

- shared across the LEVELS of a regional hierarchy — the bound level and the
  coarse levels `stamp_coarse` asks for. **True**, and the reason the win is
  larger than #493 estimated.
- shared across ALL hierarchies, regional or not. **False**, and the reading a
  host with a uniform fixture took.

One form of words, two scopes, only one of them true — the same shape as a
constraint judged against the wrong entry point of the same library, and as
"we validate the header" describing a correct and an incorrect guard
identically. The host had already written the correct caveat about its own
fixture and preferred our sentence over it, which is the cost: a claim that
covers two scopes is not merely vague, it overrides a reader's accurate
knowledge of the narrower one.

**What would exercise this change from a host: a regional hierarchy**, which
needs `clay_multires_add_level_region`. Adopting an unused ABI surface to score
a benchmark measures a code path the application has never had, so a host that
builds whole levels only is right to report no figure rather than bind one.
