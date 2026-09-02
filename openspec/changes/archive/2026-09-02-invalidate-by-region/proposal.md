# Proposal: an edit should only invalidate what it can reach

## Why

A brick refill resumes from its own previous output, so a dab costs what the dab
adds. But only an APPEND could resume: any other edit — moving an item, deleting
one, an undo — dropped every stored seed, and the next refill paid the whole
surviving edit list over every sample again.

Adjusting a placed item is not a rare thing to do, and at 50,000 items it cost
**20.8 ms** against a 4.17 ms budget.

## What

A seed is the value of that brick's CULLED tape. An item whose influence misses
the brick's cull region is dropped from that tape — so editing it cannot change
what the brick evaluates to, and the seed is still the answer, now at the new
revision.

`Doc::touch_region(changed)` keeps those seeds and drops only the ones the change
reaches. `scene::command_influence_bound` supplies the region, taken on BOTH
sides of the apply and unioned, because one side is not an answer: an add's node
is not there before, a removal's is not there after, and a move has two ends.
That is the contract the function states and the undo stack already follows.

Undo and redo hand over the bound they already compute and return to the caller.

A seed at the CURRENT revision is served directly — there is nothing to fold into
it. That is what a region-limited invalidation leaves behind, and it also makes a
refill asked for twice with no edit between free.

## Impact

12 bricks, a non-append edit (undo/redo of an item no brick read can reach):

| edit-list length | before | after | |
|---:|---:|---:|---:|
| 5,000 | 2.522 ms | **0.246 ms** | 10x |
| 20,000 | 8.003 ms | **0.995 ms** | 8x |
| 50,000 | 20.794 ms | **2.636 ms** | 7.9x |

Inside the interactive budget where it was five times over it.

## What is left in that 2.6 ms

Almost all of it is the CULL INDEX rebuild — 2.42 ms at 50,000 items. A
non-append clears the append log, so `CullIndex::append` cannot extend it and the
index is built from scratch. The same reach argument would apply to the index's
own entries, and is the obvious next step if this term starts to matter.

## Non-goals

**Edits whose reach is not known.** Everything that does not pass the command
funnel or the undo stack keeps the general invalidation, which drops everything.
That stays the DEFAULT: an entry point that does not positively know what it
changed must land there, exactly as it must for appends.

**Making the cull index region-aware**, per above.
