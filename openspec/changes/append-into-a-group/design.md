# Design

Everything below was measured on this tree before any of it was written. Three
of the four findings correct something the proposal assumed.

## What the reusable prefix is

A tail-in-tail group append leaves the two full compiles byte-identical up to
**where the group's children end**. At 1000 base items: 2009 instrs before,
2011 after, agreeing on 2008.

| shape | agree | trailing to re-emit |
|---|---:|---:|
| group last in the root list | 2008 | **1** |
| group NOT last | 2008 | 3 (the group's combine, then the sibling and its own) |

The reuse point is the SAME either way. So the tail-in-tail restriction is not
about where the prefix ends — it is about bounding what has to be re-emitted
after it. A non-tail group means recompiling everything that follows, which is
unbounded; a tail-in-tail one means the ancestor combines and nothing else.

## Correction 1: it is not one instruction per enclosing group

The proposal says the resume re-emits one combine per enclosing group. It does
not. Nesting all-`Add` groups at 1000 base items gives `trailing = 1` at depth
1, 2, 3 and 4 — the inner groups emit no combine at all.

`compile_group` emits its combine only under `if (have_acc || seeded)`, where
`seeded = !have_acc && op != Op::Add`. An inner `Add` group entered with no
accumulator beneath it passes its children's value straight through.

So the checkpoint must carry, per ancestor, whether that ancestor EMITS —
which is decided by the rule the compiler already applies, and which is
exactly what `TapeCheckpoint` records today for the layer case
(`layer_have_acc`, `doc_have_acc`) generalised to one entry per level.

## Correction 2: the rollback path IS reachable

The proposal treats `compile_group`'s roll-back-on-empty-subtree as
unreachable for an append, because the group is non-empty by construction.
That is wrong under a CULL: a group whose children all miss the brick's cull
region compiles to nothing even though it holds items.

    full compile                              9 instrs
    culled to a brick far from the group      1
    after appending another far item into it  1

A resumed append must reproduce that decision rather than re-emit the group's
combine, or the culled tape gains a combine the full compile never emitted.

**Which path this binds.** `compile_document_append` passes no cull and must
not — "it copies a prefix that was compiled without one" — so the rollback is
genuinely unreachable there. `compile_layer_suffix` DOES take a cull, and that
is the one this constrains. The two append paths need different reasoning
about the same function.

## Correction 3: the proposal's tasks do not reach the measured win

This is the load-bearing one.

The per-brick resume splits the document at a ROOT-LIST ORDINAL.
`compile_layer_prefix(doc, count, ...)` compiles `roots.begin() ..
roots.begin() + count`; `compile_layer_suffix` checks the appended ids are the
tail of `roots`; `root_ordinal_of`, `prefix_boundary` and `dirty_from` are all
root ordinals.

A node appended inside a group is not at the root list, so "everything up to
the group's children so far" is not expressible as an index. The split point
has to become a PATH.

And that is the path the 90x runs through. `sdf_stroke_in_group_bricks` drives
the brick cache: `tail_append` refuses the group append, the edit is
classified structural, seeds are dropped, and every brick pays a full culled
compile. Making that fast needs the SEED/RESUME path to accept a split inside
a group.

The proposal's tasks 1.1-1.4 are written against `TapeCheckpoint` and
`compile_document_append` — the whole-document tape. They are a prerequisite
and they are **not sufficient**: landing only those would leave
`sdf_stroke_in_group_bricks` where it is, and the change would measure as
having done nothing. That is the trap `bound-an-edit-by-the-node-it-names`
fell into, where the win existed but was masked until the cull pad was fixed.

## What this change therefore does, in two phases

**Phase 1 — the whole-document append.** `TapeCheckpoint` gains the ancestor
stack; `run()` records a checkpoint after the deepest tail chain; `resume()`
re-emits the pending combines that a full compile would have emitted;
`tail_append` accepts a tail-in-tail group append. Verifiable on its own: the
resulting tape is byte-identical to a full compile, and the counters say the
append path fired. **It does not move the device gate**, and the tasks say so
rather than letting a flat measurement read as failure.

**Phase 2 — the per-brick resume.** The split point becomes a path rather than
a root ordinal, through `compile_layer_prefix`, `compile_layer_suffix`,
`root_ordinal_of` and the seed's `prefix_boundary`. This is what moves
`sdf_stroke_in_group_bricks`, and it is the larger half.

Splitting them this way is deliberate: phase 1 is a prerequisite for phase 2
and is small enough to review on its own, and stating up front which phase
buys the number stops the first one being judged by a gate it cannot move.

## Rejected

**Making an insert anywhere cheap.** Only a TAIL append leaves the compiled
prefix a prefix. An insert in the middle moves everything after it.

**Widening to a group that is not in tail position.** Its reuse point is the
same, but everything after it must be recompiled — unbounded, and measured
above as 3 trailing instructions in the smallest case rather than 1.
