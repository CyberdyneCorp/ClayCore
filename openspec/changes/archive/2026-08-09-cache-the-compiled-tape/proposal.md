# Proposal: stop recompiling the document on every look

## Why

Every read of the field recompiled the whole document: `clay_eval_points`,
`clay_eval_gradients`, `clay_safe_step_scale`, `clay_document_mesh` and
`clay_raycast` each called `compile_document` (or `pick::pickable_tape`) from
scratch. Compiling is O(document).

That is the wrong shape for sculpting, because a stroke ADDS a node per stamp:
the document grows monotonically all session, so the cost of LOOKING grew with
everything the artist had already drawn. Measured over 240 short strokes, the
edits themselves stayed at 0.002 ms while a single-point evaluation went from
0.089 ms to 0.533 ms and a pick from 0.189 ms to 1.080 ms — the work was not the
sculpting, it was re-deriving what had already been derived.

## What changes

The compiled tape is cached on the document and invalidated by a revision that
the mutating entry points bump. Picking keeps its own slot, because it excludes
ghosted layers and is therefore a different tape that would otherwise thrash
against the main one.

Measured at 2 680 nodes:

| pattern | before | after |
|---|---|---|
| repeated reads — `clay_eval_points`, 1 point | 0.417 ms | 0.074 ms (−82%) |
| repeated reads — `clay_raycast` | 0.954 ms | 0.622 ms (−35%) |
| ten reads per edit (a preview frame) | 0.961 ms | 0.597 ms (−38%) |
| ONE read per edit (a continuous stroke) | 0.944 ms | unchanged |

The last row is the honest limit and is stated rather than buried: when every
event mutates the document, the compile happens once either way and a cache
cannot help. What it helps is everything else — preview frames, camera moves,
hover picks, marching — which is most of what an app does between edits.

## The risk, and how it is held

Under-bumping is the failure that matters. The document changes, the cache does
not, and every later read answers with the field as it was: no error, no crash,
no wrong return code, just a stale answer. So the rule is to bump on anything
that could possibly matter, because an unnecessary bump costs one recompile —
exactly the old behaviour — while a missing one is silent corruption.

`apply_edit` is the funnel for the whole command vocabulary, so the invalidation
sits there once for all of them. Undo and redo replay commands straight onto the
document rather than through it, so they bump too, and so does adding a layer
outside the vocabulary.

The guarantee is the test: the suite walks the mutating surface of the ABI and
requires that what the field ANSWERS afterwards has changed. Disabling
invalidation fails 19 assertions across every path.

## Concurrency

Two threads could read one document at once before this change, because
`compile_document` takes a `const Document&` and returned a fresh tape — no
shared mutable state. A cache introduces some, and it would have been easy to
take that property away silently.

Readers therefore receive a `shared_ptr` snapshot built under a lock rather than
a reference into a slot another thread may rebuild. A reader holding a snapshot
is unaffected by an invalidation, which is the same reason the CPU thread pool
holds its `Job` by `shared_ptr`.

## What it is not

Not incremental compilation. Adding one stamp still rebuilds the whole tape, so
a continuous stroke pays the same as before. Making an append cost the append
is the larger change and needs the tape layout to permit it.
