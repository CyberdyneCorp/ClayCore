# Design

## 1. The decision this change turns on

Not the fold — that is a small change to `run()`. It is what happens to the
**resumable multi-layer split**, which exists because layers hard-union.

`compile_document_part(doc, active, below)` compiles the layers beneath the
active one and the active one separately, and a brick refill holds the two
VALUES and folds them itself with a hard Add. That is sound only because the
document's own inter-layer combine is a hard Add: `min()` is exact, associative,
and adds no extent, so splitting there and rejoining costs nothing.

Under a per-layer operator none of the three holds. A smooth union is not
associative, a subtract is not commutative, and both change the bounds.

### The options

**(a) Refuse the split when the fold is not a hard union.** The refill falls back
to the whole-document path for that document, which is the path it already takes
whenever a seed is not exact. Costs the multi-layer fast path on documents that
use the feature; costs nothing on documents that do not.

**(b) Teach the split the operator.** The refill folds with the layer's own
combine instead of a hard Add. Correct for a hard operator; for a SMOOTH one it
is wrong in a way nothing detects, because the accumulated value part-way down a
chain is not the value the whole-document compile would have folded — this is
the same drag the cull pad exists for (`scene::cull_pad`), and it is why a smooth
chain needs a pad at all.

**(c) Split at a different place.** Fold up to the last hard boundary and treat
everything above it as one part.

**Leaning (a) for v1, with (c) as the follow-up**, because (a) is the only one of
the three that cannot be silently wrong, and because "a document that uses layer
booleans loses one cache's fast path" is a performance statement a benchmark can
carry, where (b) is a correctness statement no test would fail. This must be
settled before implementation and the benchmark in §5.25 is what prices it.

## 2. First-visible-layer semantics

The first visible SDF layer initialises the accumulator and its own operator is
NOT applied. Two failures this avoids, both of which produce an empty screen and
no error:

- `Subtract(empty, A)` — the layer removes itself from nothing.
- `Intersect(empty, A)` — likewise.

An artist reordering their base layer to the top hits one of them, and "the model
vanished and nothing said why" is the worst available outcome.

The compiler already has this shape: `compile_list` tracks `have_acc` and reads
an item's op only once something is beneath it (`tape_build.cpp:876`,
`if (!have_acc && n->op != Op::Add && !op_creates_material(n->op)) continue;`).
The layer fold SHALL follow the item rule rather than invent a second one.

## 3. Bounds, which is the correctness half

A wrong bound is not a slow frame — it is a missing ray hit, an incomplete brick
plan and a preview with holes in it. Per operator, conservatively:

| Op | Bound |
|---|---|
| Union / smooth union | union of both, dilated by the blend's support |
| Subtract | **the left operand's alone.** A subtraction cannot create material outside what it is cutting |
| Intersect | the intersection |
| Extended (groove, tongue, the morphs, paint) | whatever the item-level equivalent already computes |

The last row is the rule the other three are instances of: **the item-level
bound logic is the single source**, and a layer fold that computed its own would
be a second answer to a question already answered.

## 4. Symmetry order

A layer's mirror and radial copies are compiled and combined WITHIN the layer,
and the layer's result is combined once with what is below. Subtracting each
mirrored copy separately from the accumulator is a different field wherever the
blend is smooth, because a smooth combine is not associative — and it is the
kind of difference that looks like a rendering artefact rather than a bug.

## 5. Cache invalidation

Changing an op, a blend, a `blend_k`, a rounding, the layer order or a
visibility can change the fold from that layer upward. Every derived cache keyed
on "the document" must include the composition in what it is keyed on:

- the compiled tape and its lineage,
- the cull index and its pad,
- the SDF prefix cache (`layer_prefix_fingerprint` digests the layer's own
  properties, and composition becomes one of them),
- the brick seed store.

Conservative first: invalidate, measure, then narrow. A missed invalidation here
is wrong geometry that renders happily.
