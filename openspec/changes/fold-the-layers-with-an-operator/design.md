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

## Decision — task 0.1, settled 2026-09-06

Read against the tree, not against the proposal. Every line number below was
re-checked; the ones design.md and proposal.md carry have drifted and are
corrected in §4 of this section.

### 1. The split: SPLIT AT THE LAST HARD BOUNDARY, which is the only boundary

**Not (a), and not (b). (c) — and in this tree (c) costs exactly what (a) costs,
because the split has exactly one seam.**

§1 above argues against (b) on the grounds that "the accumulated value part-way
down a chain is not the value the whole-document compile would have folded".
That sentence does not describe this split. There is no part-way-down. The
split point is fixed, in five independent places, as the LAST visible SDF layer:

- `plan_resume` (`bindings/c/clay_c.cpp:1498`), loop at `:1505-1512`, keeps the
  last visible SDF layer and sets `has_below = visible > 1` at `:1517`.
- `plan_frontier` (`:1558`), same loop at `:1563-1568`, same `:1570`.
- `eval_requests_impl` (`:14243-14250`), a third copy of the same loop.
- `compile_document_part(doc, active, below=true)` is `run_part` with
  `Part::Before` (`src/scene/tape_build.cpp:1133`), which BREAKS at `active`
  (`:1141`) — so the `below` half is the complete accumulator `run()` holds at
  that same point, folded by the same loop, under the same document pad.
- `scene::last_visible_sdf_layer` (`tape_build.cpp:1262`) is the fifth copy.

So the value the refill holds as `below` is not a partial accumulator. It is
THE accumulator, at a layer boundary, and the join between it and the active
half is one combine: the ACTIVE layer's own composition. Under a per-layer
operator the split is therefore available exactly when that one composition is a
hard Add — regardless of what every layer beneath it does, because `run_part`
already folded those with their own compositions.

**The gate is one predicate on one layer:**

```cpp
// scene/tape.h, beside last_visible_sdf_layer.
// The join a caller holding two halves apart has to re-apply itself. True when
// there is nothing beneath the active layer, or when the active layer folds
// with a plain hard Add — the only fold `fold_layers_below` can spell.
bool layer_join_is_hard_union(const Document& doc);
```
(implementation: find the last visible SDF layer and count them; true if
`visible <= 1`, else `c.op == Op::Add && c.blend.profile == BlendProfile::Hard &&
c.blend.k == 0.0f && c.rounding == 0.0f` for that layer alone.)

A SECOND, stricter predicate is needed for one site only — `compile_document_except`,
whose promise is about the whole stack, not about one seam:

```cpp
// True when every visible SDF layer AFTER the first folds with a plain hard
// Add. Only Except/Only's min-composition identity needs this.
bool document_fold_is_hard_union(const Document& doc);
```

**Why this and not (a).** (a) as §1 states it refuses the split for any document
that uses the feature anywhere. That would cost the fast path for the most
ordinary shape the feature creates — a cutter layer beneath a unioning layer the
artist is sculpting into — for no correctness gain whatsoever, because the fold
at the seam in that document is still a hard Add. (c) refuses only where the
seam itself is composed.

**Why this and not (b).** (b) would teach `fold_layers_below`
(`bindings/c/clay_c.cpp:13263`) the four arguments. It is one line, and it is
correct at a layer boundary for any pointwise op — but it buys three new silent
failure modes and this change already has enough:

- **The empty half.** An empty tape evaluates to `CLAY_TAPE_FAR`
  (`include/clay/kernel/tape.h:1175`). `run()` at `tape_build.cpp:1199` does
  `if (!layer_val) continue;` and skips the combine entirely. With a hard Add,
  `min(below, FAR) == below` and the two agree. With a layer-level Intersect,
  `max(below, FAR) == FAR` and they do not — two different wrong answers from
  one document, differing only in bricks the composed layer does not reach.
  Under (c) that question never arises in the refill: the fold that runs is the
  one that is already there and already tested.
- **Transitions and feathered replace cannot be folded at all.** The interpreter
  branches on `ctape_mode_is_transition` and `ccombine_replace_feather` BEFORE
  calling `ctape_combine_values` (`kernel/tape.h:1184-1194`), because both need
  the sample point. `fold_layers_below` has six floats and no `p`, and
  `ctape_combine_dist`'s forward-compatibility arm returns `a` for an unknown
  mode (`kernel/tape.h:1063`) — the active layer discarded, silently. (Both ops
  are refused at the setter anyway; under (b) that refusal becomes load-bearing
  rather than merely tidy.)
- **Two implementations of one rule.** Under (c) the refill contains no fold
  arithmetic that the compiler does not also contain; whatever §2 decides about
  `!layer_val` is inherited by both halves for free, because both go through
  `run_part`/`run`. Under (b) the rule has to be written twice and can only be
  compared by sampling.

`fold_layers_below` stays byte-for-byte as it is. That is the point.

**What it costs, concretely.**

Unaffected — the split stays available:
- every document with one visible SDF layer (its composition is never applied,
  by the first-visible rule, so the predicate is trivially true);
- every document that exists today;
- every multi-layer document whose TOP visible SDF layer unions hard, whatever
  the layers beneath it are set to. `A − B + C` sculpted on `C` keeps the fast
  path.

Loses the split — top visible SDF layer composed, i.e. sculpting into the cutter:
- the append resume (#348) — `plan_resume` returns `usable = false`;
- the frontier drag resume (#360/#362) — `plan_frontier` likewise;
- the full path's Active/Below split — one whole-document batch instead of two
  halves, and no seed stored.

It does NOT lose anything else, because two consumers already refuse every
multi-layer document and this change does not touch them: the #306 cold-brick
prefix path (`prefix_source`, `clay_c.cpp:1734`, `if (has_below) return src;`)
and the device refill's seed keep (`resume_batch_into_host`, `:12564`,
`*keep_seeds = !doc->plan_resume(1).has_below;`).

**What a benchmark shows.** Register a composed-top-layer arm beside the
existing pair `BM_BrickRefillResumed` / `BM_BrickRefillFull`
(`benchmarks/bench_main.cpp:1385`, `:1388`) and gate the `resumed_frac` counter
(`:1849`) through `MAX_COUNTER` in `tools/check_bench.py:794` — the claim is a
COUNT, so it is asserted as one. Expected: `resumed_frac` 0.0 on the composed
arm against ~1.0 on the union arm, and per-dab cost equal to `BM_BrickRefillFull`
— the pre-#348 cost of a stroke on a multi-layer document, restored for that one
shape and no other. A wall-clock floor is the wrong gate here and
`MAX_COUNTER` skips silently on absence, so the union arm needs a `FASTER_THAN`
pair as well or the row can pass by not running.

### 2. Policy at each site

Detection is cheap wherever the `Document` is in hand; the table says so per row.

| # | Site (verified) | Policy | Detection | Test |
|---|---|---|---|---|
| 1 | `include/clay/scene/tape.h:147` — `compile_document` | CHANGES. This is the definition of the fold, so it does not detect anything; it folds. Comment rewritten: visible SDF layers FOLD left to right, the first initialising and its op not applied. | none needed | the §3.3 parity fixture, plus the order gate (`A−B+C` vs `A+C−B`) |
| 2 | `tape.h:198-206` + `tape_build.cpp:1255` — the resumable checkpoint's trailing union | `resume()` emits the ACTIVE LAYER's composition instead of `Op::Add`, still guarded by `cp.doc_have_acc`. The op is DERIVED from the `const Layer&` `resume()` already takes — it is NOT added to `TapeCheckpoint`. | `layer.composition`, already a parameter | `test_tape_prefix_reuse.cpp:167`, new subcases with a composed active layer; `require_identical(reused, full)` |
| 3 | `tape.h:355-375` — `compile_document_part` | Both halves stay correct compiles; only the JOIN changes, and it is the active layer's composition. Header restated: the join is no longer universally a hard Add, and a caller that re-applies the join itself must refuse unless `layer_join_is_hard_union`. Engine does not refuse. | caller's, see rows 6-8 | `combine(below, only, active.composition)` equals `compile_document` over a `lattice(16)`, memcmp, with the teeth check that a union-composed pair differs |
| 4 | `tape.h:386-405` — `compile_document_except` | NOT repairable and not repaired. With `A, B(Subtract), C` and `excluded = B`, no combine of `A+C` and `B` equals `A−B+C`: removing a middle layer changes what everything above it folds onto. The COMPILE stays valid ("the document without that layer"); the SUM promise is deleted from the header. The four callers whose contract IS the min composition REFUSE with `CLAY_ERROR_INVALID_ARGUMENT` naming the layer. | `document_fold_is_hard_union(doc)` — the strict predicate — at `clay_c.cpp:3149` (`compile_document_without`), `:14144` (`clay_brick_cache_eval_requests_excluding`, which already has a `CLAY_ERROR_NOT_FOUND` refusal to sit beside), `pyclay_module.cpp:6547` and `:6562` | `test_c_eval_excluding.cpp:83` and `:120` UPDATED, not weakened: the zero-differing-samples assertion is kept for the union arm with a comment saying it now holds only while every layer unions, and a composed arm asserts the refusal |
| 5 | `tape_build.cpp:1362` — `compile_document_append`'s `info`/`lipschitz_bounds_gradient`/`bounds` carry-over | REFUSE: `return false` when the trailing union is not a hard Add. The comment's justification ("a hard Add is exact and adds no extent") is then true wherever the function proceeds. A refusal costs one full compile, which `tape.h:290-296` already documents as the price of not being certain. Folding `info` by hand here is possible and is deliberately not done in v1: a wrongly-true `lipschitz_bounds_gradient` feeds `prove_uniform` and stores a brick that reads surface as outside. | `last_visible_sdf_layer(doc)` is already called at `:1343` | append a composed active layer, assert the call returns false and the caller's full compile is bit-identical to the reference |
| 6 | `bindings/c/clay_c.cpp:1498` `plan_resume` (statement at `:1545`) | `usable = false` when `has_below && !layer_join_is_hard_union(doc.document)`, inserted AFTER `:1517`. `has_below` keeps meaning "more than one visible SDF layer" — three callers probe it as a topology question and the existing comment at `:1513-1515` says it is set before any decline for exactly that reason. | the `Document` is a member | `test_c_frontier_resume.cpp:266`/`:681` shape: a composed document's plan comes back unusable |
| 7 | `bindings/c/clay_c.cpp:1558` `plan_frontier` (statement at `:1584`) | Identical insertion after `:1570`. | same | same |
| 8 | `bindings/c/clay_c.cpp:14250` `eval_requests_impl` | `const bool split = visible_sdf > 1 && layer_join_is_hard_union(...)` replaces `has_below` as the driver of `ChunkHalf` (`:14386`, `:14399`), the fold (`:14408`) and `store_seeds` (`:14418`). When refused: one `ChunkHalf::Whole` batch, and NO SEED STORED — the precedent is `resume_batch_into_host`'s "It stores nothing rather than something mislabelled" (`:12545-12552`). | `doc->doc.document`, in hand | a composed two-layer document's refill is sample-identical to a freshly built document's; the next batch reports `resumed_bricks == 0` |
| 9 | `bindings/c/clay_c.cpp:13263` `fold_layers_below` | **THE SITE THAT CANNOT DETECT, and therefore must not be reachable.** It takes six floats and no document. UNCHANGED; its comment gains the precondition that it is only ever reached where the join is a hard Add. Its `rev == now` caller at `:13903` does not consult a plan at all, so the enforcement is at the STORE (row 8): a two-half seed only exists if the join was a hard Add when it was taken, and any composition change bumps `revision`, so `rev == now` cannot see a stale one. | none — by construction | the row-8 test is the proof; assert `resumed_bricks` (a count) and sample identity, never the clock |
| 10 | `tests/unit/scene_utils.h:200` `ref_eval_document` — **the ninth site, which proposal.md's table of eight misses** | The independent reference evaluator hard-codes `ctape_combine_values(acc, lv, ccombine_add, cblend_hard, 0, 0)` between layers and applies no first-visible rule at layer level. It MUST learn the composition, copying `ref_eval_list`'s shape at `:175`. Left alone, the reference and the compiler agree only while every fixture unions — which is the exact condition under which a fold bug is invisible. | n/a (test code) | `test_scene.cpp:50` with a composed `gnarly_document` variant |
| — | `clay_c.cpp:12564` `resume_batch_into_host`, `:1727` `prefix_source`, `:1596` `shaped_entry` | UNCHANGED. The first two already refuse every multi-layer document. `shaped_entry`'s `want_below` gate keys on presence only, which is sufficient because row 8 never stores a two-half seed for a refused document. | — | covered by row 8 |

Two ops are refused at the setter and that refusal is load-bearing here rather
than cosmetic: `Op::None` (255, groups-only — `ctape_combine_dist` would write
it as an unknown mode and return the accumulator, discarding the layer) and both
transitions (their parameters live in `Node::transition`, which a
`LayerComposition` has nowhere to put; `emit_combine` would silently fall back to
`Compiler::default_transition_` at `tape_build.cpp:389`). `op_is_known`
(`clay_c.cpp:320`) already rejects `Op::None` and ACCEPTS the transitions, so
`validate_item_op_blend` alone is not enough — copy `validate_group_op_blend`'s
transition refusal (`:378`), which exists for the same reason.

### 3. Invalidation

**What a composition change invalidates.** It is an ordinary layer-property
command: it goes through `apply_edit` (`clay_c.cpp:3644`), lands on plain
`touch_region`, and must NOT be added to `command_is_structural` (`:3454`) or
`command_frontier` (`:3490`) — a composition change moves no root ordinals, and
marking it structural would retire every prefix seed in the document for nothing.
Derived state:

- compiled tape and cull index — free, both keyed on `revision`;
- SDF prefix cache and the whole-layer digest — composition MUST join
  `digest::mix_layer_head` (`src/session/layer_digest.h:212`), which enumerates
  fields explicitly and is invisible to a new one. `SdfPrefixCache::verify` is
  described in-file as the safety net that cannot be forgotten; a field
  `mix_layer_head` does not see is a field the safety net does not protect;
- brick seed store — through `revision` plus the `touch_region` bound below.
  Composition does NOT join `ResumeKey` (`clay_c.cpp:1256`); that key
  deliberately excludes document-wide values, and adding one strands entries
  rather than replacing them (the comment at `:1247-1255` says so);
- the cull pad — `Compiler::document_pad` (`tape_build.cpp:1112`) and
  `CullIndex::refresh_pad` (`src/scene/cull_index.cpp:36`) are a MAXIMUM OVER
  LAYERS of each layer's own sum and have no inter-layer term at all. A smooth
  or extended layer fold drags the document's running accumulator exactly as a
  smooth item combine drags a layer's, so the fold's support must enter as a
  per-layer constant, following `blend_k_seam` (`bounds.cpp:1211-1215`), which is
  the existing precedent for a LAYER-owned k reaching the pad. This is needed
  whether or not anything is split — see §4.

**The dirty bound of a composition change.** `command_influence_bound`
(`src/scene/commands.cpp:371`) sends every layer command to
`layer_command_bound` (`:330`), which is `layer_influence_bound(*l)` — the
layer's OWN extent. That is the right answer for a smooth-k or rounding change
(dilated by the fold's support) and for Subtract. It is TOO SMALL for Intersect.
`apply_edit` already unions the bound on both sides of the apply, so a change
`Intersect → Add` dirties the wider box too, for free. `layer_command_bound` is
the only function in the chain that holds the `Document`, so the below-extent
loop belongs there; `layer_influence_bound` (`bounds.cpp:1531`) takes only a
`Layer` and must not be widened in place.

**Does a subtractive or intersecting LAYER widen the influence of edits made
INSIDE the layers beneath it? NO — and this is the answer the field evidence
demands.** A combine is POINTWISE in its two operands: `ctape_combine_values`
reads `a.d` and `b.d` at the sample and nothing else. An edit beneath that
changes the accumulator at `p` changes the folded result at `p` and nowhere
else, whatever operator sits above. The only spatial spreading a fold adds is
the blend support of the fold itself, which is a fixed radius and is exactly
what `group_blend_support` (`bounds.cpp:1404`) already computes for an enclosing
group. So:

> An edit inside a lower layer dirties its own influence bound, dilated by the
> blend supports of the folds above it. Not the layer above's extent, not the
> document.

That is the direct analogue of what `node_reach_bound` (`:1443`) already does
once per enclosing group, and it keeps the host's measured 16.8x brick-count
growth out of the ordinary edit path entirely.

**Where the cost genuinely is, and it is one arrow only.** What IS non-local is
editing the composed layer itself, and the asymmetry is the far field, not the
combine:

- **Subtract is LOCAL.** `max(a, −b)`: far from `b`'s geometry, `−b` is a large
  negative number and loses the max, so the result is `a`. This is why
  `op_is_local` (`include/clay/scene/types.h:160`) excludes only Intersect and
  the transitions, and why a subtract ITEM is culled by its own geometry. A
  subtract LAYER is bounded by ITS OWN extent, dilated by its fold's support.
- **Intersect is not.** `max(a, b)`: far from `b`'s geometry, `b` is a large
  POSITIVE number and WINS the max, so the result differs from `a` everywhere
  the accumulator has material. `bounds.cpp:1270` measures exactly this — drift
  exactly 0 outside the layer's extent over 400,000 points, against 0.100 and
  0.065 outside the item's own geometry. One level up, an intersect LAYER's
  influence is the accumulated extent of the visible SDF layers BELOW it, and
  nothing in the tree computes that today. It needs a document-level analogue of
  `layer_influence_extent` (`:1352`), memoized the way `LayerExtentCache`
  (`bounds.h:133-215`) memoizes the layer one and carrying the same
  `walks()`/`keeps()` counters, because a cache that quietly stops firing here
  reads as correct.
- The same widening applies to `SetLayerVisibleCmd` and to a reorder
  (`clay_document_move_layer`, `clay_c.cpp:5289`, a Remove+Add pair each bounded
  by the moved layer's own extent). Gate 6.1 ("hide/show a subtractive layer
  restores exact geometry") passes on the subtract case while the intersect case
  quietly leaves stale bricks; gate 6.2 tests the geometry of a reorder and not
  its dirty region. Both need an intersect arm.

**What conservative costs here, said plainly.** For the intersect arm the
conservative bound IS the box the host measured: 26.2x the surface bricks of the
geometry produced at reference size, 241.2x at 10x extent, 16.8x brick-count
growth for a 31.6x volume — 45.5 ms and 7.5 s per frame respectively. That is
the price of setting a layer to Intersect and then touching it. It is bounded
because it fires only for Intersect, only on that layer's own edits, and never
on edits beneath. It is not acceptable as a steady state.

**The measurement that would narrow it.** Not the bound — the REFILL REGION. The
host's own finding is that the intersect walks a BOX and produces a BAND: the
dirty region is the AABB and the refill visits the bricks of that VOLUME rather
than the bricks that hold band. Intersecting the dirty region with the bricks
that already hold band is a cache-side change that would cut the count by that
same 26.2x/241.2x, and — if the unresolved per-brick factor turns out to be item
overlap or brick population rather than extent as such — would also stop visiting
the deep-interior bricks that cull nothing away, recovering part of it too. The
host's in-flight 2x2 (dabs held at 0.18 versus scaled; cutter buried versus at
the surface, in both scenes) is what settles which. **Nothing in this change
depends on the outcome**, and nothing in this change should be built on a guess
about it: this decision commits only to the box, which is correct either way, and
names the band intersection as the follow-up. That work is not specific to layer
composition — an intersect ITEM pays it today, at the numbers above — and it
should be its own change.

### 4. Where the tree refutes design.md

House style is to say so.

1. **§1's argument against (b) does not describe this split.** "The accumulated
   value part-way down a chain" — there is no part-way-down. The split seam is
   the last visible SDF layer, fixed in five places, and `below` is the complete
   accumulator. Consequently **(c) is not a follow-up; it is the v1 answer, and
   it costs one predicate on one layer rather than a loop over all of them.**
   §1's leaning would have refused the fast path for `A − B + C` sculpted on `C`,
   which is the ordinary shape the feature creates, for no correctness gain.
2. **§1 attributes the smooth-drag problem to the split. It belongs to the cull
   pad.** `document_pad` has no inter-layer term, so a smooth layer fold makes
   per-brick tapes drop items the whole-document compile keeps — inside the band,
   where nothing is looking — whether or not anything is split. Refusing the
   split does not fix it and never would have. See §3.
3. **§3's bounds table is wrong in both directions, and contradicts the sentence
   directly beneath it** ("the item-level bound logic is the single source"):
   - `Intersect | the intersection` is **TOO SMALL** and is precisely the
     missing-surface failure the same section warns about. The result's MATERIAL
     is in the intersection; the FIELD changes everywhere the left operand has
     material, because the intersect uses the right operand's far field. The
     item-level source says so in as many words (`bounds.cpp:1260-1270`,
     `Nonlocality::BoundedByLayer`).
   - `Subtract | the left operand's alone` is **looser than the item-level
     source**, which makes a subtract LOCAL (`op_is_local`, `types.h:160`) and
     bounds it by its own geometry dilated by rounding and blend support
     (`geometry_bound`, `bounds.cpp:918-923`).

   Corrected, from the single source: **Subtract → its own extent, dilated.
   Intersect → the extent of the visible SDF layers BELOW it. Smooth/extended →
   the union, dilated by `ccombine_extended_support` / `Blend::support()`.**
4. **§5 says composition joins the key of "the brick seed store". It must not
   join `ResumeKey`** (`clay_c.cpp:1256`), which deliberately excludes
   document-wide values and would strand entries rather than replace them. It
   invalidates through `revision` plus `touch_region`.
5. **§2's "SHALL follow the item rule" needs one qualification or it inverts.**
   The item rule at `tape_build.cpp:957` SKIPS a non-Add op with nothing beneath;
   the spec requires the first visible layer to INITIALISE. What transfers is the
   `if (have_acc)` guard on the combine — which `run()` at `:1200` and
   `run_part()` at `:1166` already have — not the `continue`. Copying the
   `continue` produces the blank screen §2 exists to prevent.
6. **design.md never names `if (!layer_val) continue;`** (`tape_build.cpp:1199`
   and `:1165`), and it is a second silent-wrong-field site INSIDE `run()`:
   skipping the combine for a layer whose chain culled to nothing is right for
   Add and Subtract and catastrophic for Intersect, where combining with nothing
   must remove everything below. It differs per brick, so the whole-document tape
   and the per-brick tape disagree exactly where nobody is looking. §2 must
   decide it — `emit_empty` (`:272`) plus the combine, mirroring `seeded` at
   `:975` — and the split inherits whatever it decides for free, which is one
   more reason for (c) over (b).
7. **§4 (symmetry) costs zero code, confirmed.** Mirror and radial copies are
   emitted per-item inside `emit_item` and folded by their own seam combines
   (`tape_build.cpp:818`, `:859`) before `compile_list` returns, so a combine
   emitted where `:1200` sits today is already after the layer's symmetry has
   resolved, once. It stays true only while nobody hoists the layer combine
   earlier for a bounds or cull reason.
8. **Line numbers and one path.** `src/scene/clay_c.cpp` does not exist — it is
   `bindings/c/clay_c.cpp`. The `have_acc` rule is `tape_build.cpp:957`, not
   `:876`. "A hard Add is exact and adds no extent" is `tape_build.cpp:1362`, not
   `:1281`. The two refill statements are `clay_c.cpp:1545` and `:1584`, not
   `:1502` and `:1541`. `compile_document_except` is `tape.h:405`, not `:390`.
   Grep the quoted sentence, never the line number.

## 6. What a layer-wide dirty region would cost, measured

The invalidation policy in §5 says "conservative first, then narrow". This
section is the number that says how conservative is too conservative, measured
by ClaySpaceDesktop on 2026-09-06 against an intersect ITEM — which is the same
shape this change gives a LAYER, one level up.

A 12-frame drag of an intersecting cylinder over one SDF layer of 97 items,
against a subtracting control on the identical fixture and frame path, in two
scenes differing only in extent. Refill per frame, cutter placed on the surface
in both so nothing is confounded:

| | refill ms | bricks/frame | µs/brick |
|---|---:|---:|---:|
| subtract (`op_is_local`) | 14.78 | 741 | 19.9 |
| intersect (`BoundedByLayer`) | 11,512.38 | 100,800 | 114.2 |
| ratio | **779x** | **136x** | **5.7x** |

The 779x is a product of two factors and both are properties of WHICH BRICKS GET
VISITED:

- **Count, 136x.** An intersect's dirty region is the layer's AABB and the refill
  walks the bricks of that VOLUME rather than the bricks that hold band. It
  refills 26.2x the surface bricks of the geometry it produces at reference size
  and 241.2x at 10x, and the ratio grows with radius.
- **Population, 5.7x.** A box walk visits interior bricks, where nothing culls
  the document away and every tape is long; a band walk visits rim bricks, where
  most of it culls out. Measured directly at 4.4x with the brick count pinned at
  741 by construction, varying only whether the cutter is buried or on the
  surface.

**There is no extent-driven per-brick cost.** Holding the dab at 0.18 and the
cutter on the surface, a brick costs 9.95 µs at r=1 and 9.15 µs at r=√10 — 0.92x,
flat. The per-brick growth in the first measurement was item overlap (a fixture
whose dabs scale √10 against a fixed 0.16 brick edge: 12.5x) plus brick
population (4.4x). A first reading of a second, extent-driven engine slope was
retracted by the host that found it once its own control was shown to be
confounded.

**What this requires of this change.** A composition change is a layer-property
edit; it must not be given a region that scales with the layer when a tighter one
is correct. And an edit made INSIDE a layer beneath a subtractive or intersecting
layer must not inherit the composed layer's whole extent by default, because that
is exactly the 779x above, arriving one level up and on every frame of a drag.
Where this change chooses to be conservative, §5 says what it costs and names the
measurement that would narrow it; it does not choose conservative by omission.

**And count matters on its own.** With the overlap effect entirely removed, the
box walk is still 88,200 bricks at 9.15 µs — 806 ms a frame. A future fix that
only made bricks cheaper would leave a 0.8-second frame; the region is the thing.

## 7. Writing at minor 17 — REFUSE, do not degrade

Raised by ClaySpaceDesktop on 2026-09-06 while the host surface was still being
designed, and it changes what stage 5 builds.

The repo rule is that a new minor must be **writable at the previous one,
degrading to whatever that minor meant, with the notes saying exactly what the
downgrade loses**. Every minor so far has obeyed it cheaply because the loss was
never something an artist made — 16 → 17's own note says writing at the older
minor costs "the payload deduplication and nothing an artist authored — a file
that is larger and identical in content".

**18 → 17 is the first minor where the degrade changes the model.** A
subtractive layer written at 17 comes back as a union: the cutter that was
carving a hole is a lump welded onto the form. Nothing is corrupt, nothing
refuses, the file opens, and the sculpture is wrong in a way that looks
deliberate. That is the empty-tape `max(below, FAR)` failure one level up and
visible to the artist rather than buried in a brick.

**The decision:**

1. `serialize_document(doc, minor)` with `minor < 18` **refuses** when any SDF
   layer carries a composition that is not the default hard union. Refusing is
   the direction this format already fails in — records are not length-prefixed
   precisely so an older build meeting a newer minor fails rather than misreads —
   and it is the only direction that cannot be quietly wrong.
2. Where every layer's composition IS the default, writing at 17 is allowed and
   produces exactly what 17 always meant, byte for byte. So the repo rule stays
   true for every document the older minor can actually express, and the refusal
   covers exactly the documents it cannot. "Writable at the previous minor"
   means *when the previous minor can say it*, not *by discarding what it
   cannot*.
3. **A host must be able to ask before it saves.** This change ships a query
   across the C ABI: can this document be written at minor N without losing
   authored intent? A host that can ask puts an honest sentence in front of a
   person; one that cannot guesses on their behalf. `CLAY_ERROR_UNSUPPORTED` is
   the code the refusal itself returns.

**Not in this change, and recorded as a gap rather than inherited:** a C-ABI host
cannot choose the minor it writes at all. `clay_document_save` takes a path and
`clay_document_save_memory` takes a blob; neither takes a version, and the minor
is a parameter on the C++ `scene::serialize_document` that does not cross the
ABI. Three releases of upgrade notes have advised hosts to "write at the older
minor if you exchange documents with an older build", and no C-ABI host has ever
been able to take that advice. It cost nothing while the loss was deduplication.
It is not free now: a host that wants an interchange copy cannot offer one, and a
host that wants to refuse to write 17 has nothing to refuse because it could
never ask. A save-at-minor entry point needs its own change — the blob variant,
the autosave and journal paths, and the other lossy minors all come with it — and
the query above is the half that makes this change's decision answerable from a
host meanwhile.

## 8. SDF-only is the shape of the FEATURE, not of stage 1

Asked by ClaySpaceDesktop on 2026-09-06, because the answer decides whether their
interface explains a live boolean per OPERATION or per OPERAND, and they would
rather write the sentence once. It is per operand, and it is durable.

`run()` folds `if (!layer.visible || layer.kind != LayerKind::Sdf || !layer.sdf)
continue;` (`src/scene/tape_build.cpp:1243`, and again at `:1281` for the part
compile). A mesh or voxel layer contributes NOTHING to what a document evaluates
to, and that is a standing architectural property rather than an omission: for
mesh it is structural, since `tools/check_layering.py` withholds `mesh` from
`clay::scene`, which is what makes "a mesh layer does not change what the
document evaluates to" a fact about the build rather than a maintained promise.

So a composition on a non-SDF layer would be **state that does nothing**, which
the spec delta forbids in as many words: "A layer whose kind cannot enter the tape
SHALL REFUSE a composition rather than store one that does nothing, so that a
control a host offers is a control that acts." Widening the setter later would
mean either lifting a representation into the tape or storing a control that lies,
and the first is a different change entirely.

**The route for the other representations is CONVERSION, not a later widening.** A
mesh becomes a field through the mesh-to-field import and is then an ordinary SDF
layer that can carry a composition — which is why gate 6.5 of this change is "a
converted mesh-to-SDF layer works as a cutter" and not an afterthought. A voxel
region becomes a field item through a captured volume. Both are existing routes.

**The sentence a host can write and keep:** a subtool is live when it is a FIELD
subtool; a mesh or grid subtool becomes live by being converted into one; and a
resolved boolean remains first-class for operands that are not converted, rather
than being the old way waiting to be retired.

## 9. An absent operand, and two gates that follow from it

Two things ClaySpaceDesktop raised on 2026-09-06 against stage 2's `fold_layer`.
Both are gates for stage 3, and both are cheap.

### The divergence is real, and this side of it is not free to change

Their RESOLVED boolean filters an empty subtool out of `boolean_operands`
entirely — "because there is nothing in them to combine" — so an absent operand
is not an operand. The live fold does the opposite: when a layer produces no
value it emits an explicit empty and folds it, whenever
`fold_changes_an_empty_layer` says the operator reads an absent operand as a
change (`src/scene/tape_build.cpp:99`, which probes `ctape_combine_dist` against
`CLAY_TAPE_FAR` at five sample distances rather than hard-coding a list of ops).

**That is not a preference and it cannot follow theirs**, because `layer_val` is
false for two different reasons and only one of them is emptiness:

- the layer has no visible contributing items — genuinely empty, and
- **the layer's chain was wholly CULLED in this compile's region**, which a
  per-brick compile does constantly for a layer with content elsewhere.

Skipping the fold in the second case would be a silent per-brick wrongness of
exactly the kind this change exists to avoid: an intersecting layer must still
remove material from a brick its own geometry does not reach, because the
whole-document compile removes it there. So the fold stays.

**What follows is a documentation duty, not a code change.** For a
DOCUMENT-empty operand the two routes now disagree: a resolved boolean skips the
operand, a live one applies it, so an empty intersecting layer blanks the field
where the resolved path would leave it alone. The header must say so beside the
setter, so a host that wants parity can filter empty operands itself — which is a
host policy, and the engine cannot take it without breaking the culled case.

### Gate: a converted layer as the BASE, not only as the cutter

Task 6.5 gates "a converted mesh-to-SDF layer works as a cutter". The host's
`boolean_operands` puts every representation on BOTH sides, and a mesh converted
to a field so that something can be cut out OF it is at least as common as
converting the cutter. Add the base case beside it: **a converted layer beneath a
field cutter, with the fold applied to it.** If the fold treats base and cutter
symmetrically the gate is redundant and costs one fixture; if it does not, it is
the gate that finds it.

Their crossings are first-class controls a sculptor already has — `MeshToSdf`
(triangles onto a lattice as a volume item) and `VoxelToSdf` (occupancy read back
as a distance field, redistanced) — so §8's "convert this subtool to make the
boolean live" names a menu entry rather than work anyone has to build.

### Gate: an empty intersecting layer, decided rather than discovered

A test asserting what a DOCUMENT-empty layer set to Intersect does, so the
divergence above is deliberate and stays that way. The test is the record.

## 10. The cull pad has no term for a layer-level blend k — REQUIRED before this ships

Raised by stage 2's handover, recorded here because a handover is read by the
next stage and this must be read by all of them.

`Compiler::document_pad` sums the pad terms a layer's ITEMS need. A layer
composition can now carry a smooth blend with its own `k`, and nothing adds a
term for it. The fold is live as of stage 2, so this is a hole in the tree today
rather than a future one, and it is the same silent class as everything else in
this change: a per-brick culled tape that drops items the whole-document compile
keeps returns a field that never existed, with no error and no visual tell beyond
geometry that is subtly wrong at a brick boundary.

**What is required, not optional, before this change is reviewable:**

1. A pad term for the layer combine's `k` and rounding, folded into
   `document_pad` the way an item's chain terms already fold. `cull_pad_terms`
   is the place the tree already keeps terms UNADDED and unresolved, and
   `blend_cull_pad`'s definition records why the chain envelope grows with the
   contributor count — a layer fold is one more contributor to that chain, at
   the document level.
2. A test that FAILS without the term: a document whose layers fold with a
   smooth k, compiled per brick against a region small enough that the naive pad
   drops a contributing item, compared with the whole-document compile. Assert
   band-clamped identity, which is what the culled tape already promises.
3. Where the pad is deliberately conservative, say what it costs. A pad that is
   too wide is a slower compile; one that is too narrow is wrong geometry. Those
   are not symmetric and the comment should say so.

A hard-union fold needs no term, which is why nothing needed one before and why
every existing document stays exactly as fast as it was.

### §10a. The term is a SUM over the stack, and it is not the layer's own

Written after §13's sweep found it, and it corrects requirement 1 above rather
than merely satisfying it.

Requirement 1 said the term folds into `document_pad` "the way an item's chain
terms already fold", and named `cull_pad_terms` as the place to keep it. The
first implementation did exactly that: `cull_pad_terms(content, layer)` raised
`blend_fixed` by that LAYER's own `layer_blend_support`, and both readers —
`document_pad` and `CullIndex::refresh_pad` — are a MAXIMUM OVER LAYERS. That is
wrong twice, and only the maximum hid the second one:

- **A max where the quantity is a SUM.** The drag an item passes through is
  every fold ABOVE it, and folds compose — the second one sees a field that
  already differs over the first's dilated box and can move its own result that
  much further again. The change already spelled this out for the dirty region
  (`folds_from_layer_support`, `src/scene/commands.cpp`), so the pad took the
  smaller of this change's own two answers to one question.
- **Charged to the layer that OWNS the fold**, while the items that need it are
  in the layers below. Under a document-wide maximum that misattribution is
  invisible, so fixing the sum without fixing the attribution would have moved
  the error rather than closed it.

Measured, four spheres r = 0.5 at x = 0, 0.62, 1.24, 1.86 with the top N folds
set to a quadratic k, swept over 240 regions of 0.06 each dilated by a 0.1 band,
comparing a culled compile with the whole-document one inside the band: worst
drift 0 at one composed fold — which is the only shape the original test
exercised — 0.0180 at two (k = 0.3), 0.0229 and 0.0268 at three (k = 0.3, 0.45),
and 0 for the all-hard baseline. Dilating each region by a further 2k took the
two-fold row to 0 and the three-fold row to 0.0049, which is what identified the
PAD rather than the fold, and showed the shortfall scaling with the fold COUNT.

**What is there now.** `folds_from_layer_support(doc, layer)` moves to
`scene/bounds.{h,cpp}` as the one definition of the quantity, and
`scene::document_cull_pad(doc)` is a maximum over visible SDF layers of that
layer's own `cull_pad` PLUS its fold sum. `cull_pad_terms` carries no fold term
at all: it answers what one layer's ITEM CHAIN needs, and a fold is not a
property of the layer that owns it. `CullIndex::refresh_pad` is the same
expression over cached terms, and the two are now held equal by a test rather
than by a comment, because a compile takes whichever it has.

**And the first visible SDF layer's own composition is not a term**, because it
is never applied. Counting it was safe and not free: over-wide keeps items a
compile did not need and costs a longer tape, too narrow drops an item the field
needed and costs the geometry — the directions are not symmetric, which is
exactly why the merely-slow one is still not taken when the exact term is in
hand.

## 11. The placement classifier does not read the composition — REQUIRED, and it is live now

Raised by ClaySpaceDesktop on 2026-09-06, checked against the tree, and it is a
real defect introduced by stage 2 rather than a hypothetical.

`scene::layer_scales_cleanly` (`src/scene/placement.cpp:46`) decides whether a
uniformly scaled layer is a SIMILARITY of its own field. It walks the layer's
ITEM nodes and returns false for any visible node with a soft blend and a
positive `k` — "the blend radius is the term the layer's scale does not reach".
**It does not look at `layer.composition`.** Stage 2 gave the LAYER its own
blend, so a layer whose items are all hard but whose COMPOSITION carries a smooth
`k` now classifies as Similarity, and takes the cheap invalidation, while its
field changes in a way a similarity does not describe. Three callers act on that
verdict: `layer_placement_change` (`:64`), `clay_c.cpp:5702` and
`pyclay_module.cpp:4430`.

This is the item-level asymmetry the v0.84.0 known limits already record, one
level up: "Rounding scales with the placement and `blend.k` does not; measured at
1.289 where a similarity says 2." And the asymmetry is genuinely only `k` here —
`fold_layer` takes `comp.rounding * layer_distance_scale(layer)`, so the layer's
rounding DOES follow its scale. Only the radius does not.

It matters more here than in the item case, because the verdict feeds
`clay_layer_placement_begin/_update/_commit` — the gesture that exists to SKIP
work. A wrong Similarity there is a picture that lags its own field, not a
recomputation that costs a little.

**Required:**

1. `layer_scales_cleanly` returns false when
   `layer.composition.blend.profile != BlendProfile::Hard &&
   layer.composition.blend.k > 0.0f`, so such a layer classifies GENERAL and
   promises nothing — the same answer the item case gives, for the same reason.
2. A regression test: a layer whose ITEMS all scale cleanly but whose composition
   carries a smooth `k`, scaled uniformly, reports GENERAL from
   `clay_layer_placement_report`. **Prove it by reverting the predicate and
   watching it fail**, which is this repository's rule for a regression test and
   the only way to know the test could ever have caught it.
3. Say it in the header BESIDE the composition setter, not only in the placement
   notes. A host reading `LayerComposition` has no reason to look under placement
   to find out what a blend does to a drag — the host that raised this could not
   have found it from outside, which is the argument for where the sentence goes.

**Why an artist meets this on an ordinary day**, in the host's words: a blend
radius is an absolute world distance, and whole subtools get scaled. Place a
cutter, set a soft join, scale the cutter — the join then covers the same
absolute distance across a bigger cutter, so the cut reads as getting harder as
the subtool grows. That part is inherent to a radius in world units. What must
not also happen is the gesture skipping invalidation on the strength of a
similarity that is not one.

### §11a. The header must state the TRADE, not only the classification

Raised by the host on 2026-09-06 and checked: a composition change is a document
command (`SetLayerCompositionCmd`, stage 1), while `clay_layer_placement_*` is a
gesture whose premise is that the document does not move until the commit. So a
host cannot have both halves of what it will assume it has:

- **An absolute radius** — what a blend `k` is, in the layer's units — keeps the
  drag cheap, and the join covers the same world distance however large the
  subtool grows, so the cut reads as hardening with size.
- **A radius scaled to compensate** keeps the join proportional at every size,
  and makes scaling that layer an EDIT rather than a placement, because writing
  the composition is a command. The gesture is gone for that layer, and nothing
  reports its absence.

Neither is wrong and the engine does not pick. But a host reading
`LayerComposition` has every reason to assume it can have both — the blend is on
the composition, the scale is on the transform, and the interaction lives in a
third file. A header that says only "a soft `k` classifies GENERAL" teaches a
host that it is slow and not why, and the obvious fix (scale the radius to
compensate) silently removes the gesture.

**Required beside the composition setter, in addition to the classification
sentence:** *a blend radius is an absolute distance and does not follow the
layer's scale; a host that compensates for that turns every scale of that layer
into an edit.* One clause more than the classification, and it is the clause that
stops someone discovering the trade by measuring it.

## 12. The excluding preview: the refusal is right, and it must not be the whole answer

Raised by ClaySpaceDesktop on 2026-09-06, checked against the tree, and it
changes stage 5's scope.

**First, the reassurance they asked for.** `clay_brick_cache_eval_requests_excluding`
(`bindings/c/clay_c.cpp:14290`) carries the SAME refusal as the document form:
`first_composed_fold_layer` on the whole document, `CLAY_ERROR_INVALID_ARGUMENT`,
before any work. So a composed document gives their preview an error and not a
wrong picture. That was the failure they were worried about and it does not
exist.

**What the refusal costs them, and it is not small.** Their live Suavizar and
Relaxar evaluate every visible SDF layer EXCEPT the one under the brush once at
pointer-down, then compose the preview per frame with a `min`. That is exact
today for the reason the header states — visible layers hard-union, and a union
IS the smaller of two distances. Once any layer composes, `min` is not that
field, so the call refuses and **live smoothing stops working on any document
that uses this feature**. A change that adds a capability and silently removes
one from the host's most-used tool is not a good trade.

**Their proposed repair is sound, but only under a condition the refusal must
state.** Excluding a layer from the MIDDLE of a fold cannot be repaired: layers
above it fold onto an accumulator that included it, so the two halves are not two
operands of one combine. But when the excluded layer is the LAST visible SDF
layer, the halves ARE two operands of one combine — which is exactly what stage 4
repaired for `compile_document_part`, and `scene::layer_join_composition(doc,
active)` is already the combine they rejoin under.

So the pairing that works is **Below + Active**, not Except + Active, and the
join is the active layer's own composition, which a host can already read through
`clay_document_layer_composition`.

**The gap that makes this a stage 5 task rather than advice:** `ChunkHalf::Below`
exists (`clay_c.cpp:4122`, used by the resume split) and **no C entry point
exposes it**. A host can ask for one layer (`clay_brick_cache_eval_requests_layer`)
and for everything-except-one (`_excluding`), and cannot ask for everything-below.

**Required of stage 5:**

1. `clay_brick_cache_eval_requests_below` — the same shape as its two siblings,
   `ChunkHalf::Below`, refusing only when the named layer is NOT the last visible
   SDF layer, with the message saying which layer is. That refusal is narrow: the
   layers beneath may compose however they like, because `compile_document_part`
   folds them with their own compositions.
2. The header on `_excluding` says what to use instead and when — that a preview
   of the TOP layer composes exactly with `_below` plus that layer's own
   composition, and that excluding from the middle of a fold has no repair. A
   refusal that names the alternative is a different thing from one that does not.
3. A test that the two routes agree: `below` folded with the active layer's
   composition equals the whole document, over sampled points, for a document
   whose lower layers compose and whose top layer composes.

This is the same asymmetry as the 0.1 decision, arriving at a host-facing call:
the seam is one question, and everything below it is already answered.

### §12a. The active layer is often NOT the top, and the refusal must name what blocks it

Corrected by the host on 2026-09-06, against its own reference fixture rather
than against an intuition. Their `visual_shell` stack is four rows —
`Detalhes_secundarios`, `Poros`, `Forma_principal`, `Base` — and the ACTIVE layer
is `Forma_principal`, third of four. The excluded layer is whatever the sculptor
clicked; nothing constrains it to the top. A sculptor blocks out a form, adds
pores and fine detail above it, and goes back down to smooth the form underneath.
So "the layer under the brush is usually the top" is wrong, and §12's condition
bites in the ordinary case rather than in a corner.

**Two things narrow it, and neither changes the rule.** The condition is the last
visible **SDF** layer, so mesh, voxel and hierarchy subtools above the active one
do not disqualify it and neither do hidden ones — a stack whose upper rows are a
carried mesh and a rasterised grid still qualifies while looking to the artist as
though something is above. And a document where nothing composes is unaffected
entirely.

**What does NOT narrow it, checked rather than assumed:** "every layer above is a
plain union" is not sufficient. With `⊕` the active layer's fold and `U` the
union of the layers above, the document is `(below ⊕ active) ∪ U`, and neither
`_excluding` nor `_below` alone can produce that — `_excluding` gives
`below ∪ U`, and `(below ∪ U) ⊕ active` is a different field for any `⊕` that is
not itself a union. Repairing that case needs a THREE-way split (below, active,
above) and a host composing twice. Recorded as the widening this could take if
the fallthrough measures large enough to want it; not this change.

**Required, and it is the third time this medicine applies:** `_below`'s refusal
SHALL hand back the id of the layer that blocks it — the lowest visible SDF layer
above the named one — exactly as `clay_document_writable_at_minor` returns its
blocking layer and as the composition setter names what it refused. The host's
subtool rows are engine layers, so an id becomes a row a person can select, and
the sentence becomes *"hide or move Poros to smooth Forma_principal live"* rather
than *"live smoothing is not available here"*. One names an action; the other
names a wall. It matters more here than in the `writable_at_minor` case: a
document has one format and a stack has many layers, so without the id the host
walks the stack to re-derive a fact the refusal already computed.

### §12b. Make the refusal rule executable, not a review note

The rule this change produced — a refusal that knows an id returns it — is
currently three separate implementations and a sentence in the roadmap. A
sentence is checked when someone leans on it; a test is checked every run. So the
rule gets a test rather than a reviewer:

**Required:** one test case walking every refusal in this change that has an id
to give — the composition setter on a non-SDF layer, `clay_document_writable_at_minor`
on a document with a composed layer, and `clay_brick_cache_eval_requests_below`
on a layer that is not the topmost visible SDF one — asserting each returns the
error code AND a non-zero blocking id that names the layer actually responsible.
A refusal that returns the code with a zero id fails the test.

It is cheap, it fails the day a fourth refusal is added without its id, and it
turns "we agreed to do this" into something that does not depend on anyone
remembering.

## 13. The general form of all three blockers, and the sweep it requires

Named by ClaySpaceDesktop on 2026-09-06 after reading the review findings, and it
is one sentence that covers all three:

> **A culled compile is not a small whole-document compile.** Every question this
> change asks must be asked of the DOCUMENT, not of the compile in front of it.

The three instances, all correct as "what has this compile seen" and all wrong as
"what does this document contain":

| predicate | true meaning | mistaken for |
|---|---|---|
| `layer_val` false | empty **or wholly culled in this region** | the layer is empty |
| `have_acc` false | nothing emitted yet **or the cull dropped everything below** | this is the first visible SDF layer |
| `cull_pad_terms` | the terms an ITEM chain needs | the terms the document needs, fold included |

The property they share is that **the cull is an optimisation and a predicate can
observe it.** An optimisation is supposed to be invisible to results; any value
derived from it leaks it, and the leak is visible only per brick, which is
precisely where nobody looks.

`have_acc` is the sharpest case and the diagnosis is worth keeping: it reused the
right rule at the WRONG SCOPE. The item-level rule is correct because an item
chain is compiled whole; the layer-level question is asked against a document a
brick has already been allowed to forget most of. Reusing rather than inventing
was the right instinct and it is what carried the bug.

**Required, and it is a sweep rather than three fixes:** every place the fold
path reads state a cull region can change must be found and decided, not only the
three the reviewers named. The first was caught before it shipped, the third by
being promoted out of a handover note, and the second by review — so the score is
one found by looking and two by writing things down where someone had to pass
them. A fourth would need somebody looking for the same thing a third time.

**A host dependency, so a partial landing is not mistaken for their bug:**
ClaySpaceDesktop's `place_layer` and `set_object_transform` both refill
`union(before, after)` from bounds this engine computes. If a bound does not
account for the folds ABOVE a layer, their refill is too small and they leave
stale geometry with no error, having asked for exactly what they were told. Their
refill correctness rides on the upward widening landing completely.

### §13a. Two adjacent bools are one transposition apart — make the compiler hold it

Raised by the host on 2026-09-06 against the fix for §13's blocker, and checked
against the tree, where it is real rather than hypothetical:

    bool fold_layer(const Layer& layer, bool layer_val, bool have_acc);
    bool compile_and_fold_layer(const Layer& layer, bool first, bool have_acc);

Two adjacent `bool`s in each, three call sites between them (`run`, `run_part`,
and `compile_and_fold_layer` into `fold_layer`), and a transposition at any of
them compiles silently and produces the exact defect the fix just closed — a
composed layer initialising instead of folding, per brick, with no error.

The shout-case comment above `fold_layer` is right and stays, but a comment
protects a reader who is LOOKING. It does not protect a caller who is confident,
and it is in the category this change has twice recorded as decaying: nothing
re-runs it.

**Required before merge:** give first-ness a distinct type — a one-field struct
is enough (`struct FirstVisibleLayer { bool value; };`) — so a swap is a compile
error at every call site, forever, with no test to run and nothing to remember.
This is the asan argument applied to a signature rather than to memory: the
change is exactly the shape where the compiler can hold an invariant a human
otherwise keeps having to.

The reviewers' second pass should answer one question about every predicate pair
in the fold path: **could these two arguments be swapped, and would anything
notice?** If the answer is "no, and nothing would", the comment is the whole
defence, which this change has already proved is not enough — the first version
of that line passed a full green gate run including asan and tsan.

### §13b. The exact call path a real host takes, confirmed

Confirmed by ClaySpaceDesktop on 2026-09-06, against its own source rather than
from memory, and it narrows §13's closing paragraph from a warning to two
symbols:

    node_bound      -> Document::node_influence_bound -> clay_layer_node_influence_bound
    refill_region   -> BrickCache::mark_dirty         -> clay_brick_cache_mark_dirty

`place_layer` and `set_object_transform` compute `union(before, after)` from
`clay_layer_node_influence_bound` — the reader that the first fix left reporting
the UN-DILATED box — and hand the result to `clay_brick_cache_mark_dirty`, which
takes the region it is given and cannot correct it. So the query is the surface
that has to be right; the dirty call is downstream of the mistake and blameless.

**This is why the query and the command path may not disagree.** A host that
dirties by what it was told leaves stale geometry having asked for exactly the
right thing, and the symptom on its side is missing surface with nothing to point
at — the host's own note says its first instinct would have been to look at its
mesh layer rather than at a bound the engine handed back. `clay.h` already
promises this of `mark_dirty_nodes`: "the region is the single most likely thing
to get silently wrong and a bound that is too tight leaves visibly stale bricks
at a blend seam."

So the fix for blockers 2, 3 and 4 is not "dilate four call sites". It is that
**one function answers "where can an edit reach in this document", and the query,
the dirty calls, the command path and the gesture reaches all go through it** —
which is what `node_influence_bound_in_document`'s own comment already claims and
this change made false.

### §13c. Which of the three gesture reaches a real host actually drives

Established 2026-09-06 by reading both trees rather than reasoning about them,
and it reorders blocker 3 rather than widening it.

The three `GestureRegion` reaches that bypass `command_influence_bound` belong to
`clay_layer_place_stamps` (`bindings/c/clay_c.cpp:9488`),
`clay_layer_move_surface` (`:7613`) and `clay_layer_magnify_surface` (`:7745`).
Against the one host we can check:

| entry point | driven? |
|---|---|
| `clay_layer_place_stamps` | **no caller anywhere** — one of the 29 entry points v0.84.0 added that this host calls none of |
| `clay_layer_move_surface` / `_preview` | **yes** — wrapped in its `sculpt.rs` and driven by its Mover tool, which is a sculptor pulling the surface with the pointer |
| `clay_layer_magnify_surface` / `_preview` | wrapped by neither and called by nobody |

**And the stroke path was never broken.** `clay_layer_apply_stroke` (`:7989`)
applies each stroke node through `apply_edit` (`:8024`) inside an undo group, so
every field dab already routes through `command_influence_bound` and inherited
the first fix. The asymmetry blocker 3 names is real and it is between
PLACE-STAMPS and apply-edit, not between strokes and apply-edit.

So the live exposure is **one tool, on a drag** — where a region that is too
small shows as the surface tearing behind the pointer rather than as a stale
patch found later. All three reaches still get fixed; this says which one has a
user behind it today, and it is the one whose symptom is continuous.

**Method note, because it is the transferable part.** Both sides of this were
asserted before they were checked and both assertions were wrong: the host said
its stroke path carried every dab (it takes the fixed route), and this file said
the stamp stroke was the exposed one (it is `place_stamps`, which nobody calls).
Each was checkable in a minute because the other named a SYMBOL and a FILE rather
than describing a flow. Name the symbol even when you might be wrong about it —
especially then, since that is what makes the correction cheap.

### §13c. One function, and what the second review's blockers 2, 3 and 4 turned out to be

§13b closes by saying the fix "is not 'dilate four call sites'. It is that ONE
function answers 'where can an edit reach in this document'". That is what is
there now, and this section records the shape of it and the one finding that
does not match the report it came from.

**The one function is `scene::layer_reach_in_document(doc, layer_id, in_layer)`**
(`include/clay/scene/bounds.h`): a box the caller knows the LAYER's field cannot
change outside of, carried to the box the DOCUMENT's field cannot change outside
of. It is the only place `folds_from_layer_support` is applied to a bound.
Everything goes through it:

| route | how it reaches the term |
|---|---|
| `clay_layer_node_influence_bound` | `node_influence_bound_in_document` |
| `clay_brick_cache_mark_dirty_nodes` | the same function |
| `scene::node_command_bound` | IS that function now — it looks the content up from a layer id and calls it |
| `clay_layer_influence_bound` | `layer_influence_bound_in_document` |
| `clay_brick_cache_mark_dirty_layer` | the same function |
| `scene::layer_command_bound` | that function plus `first_visible_flip_bound`, the one term only a command has |
| `first_visible_flip_bound` | `layer_influence_bound_in_document` for the promoted layer |
| the stamp stroke, the surface drag, the surface magnify | `layer_reach_in_document` on each stated reach; the drag's instanced-sharer boxes come from `layer_influence_bound_in_document` |

Two consequences worth stating rather than discovering:

1. **The node query is `node_reach_bound` now, not `node_influence_bound`.** The
   query used to stop at the node's own box while the command path dilated by
   each enclosing GROUP's blend support — a pre-existing instance of exactly the
   disagreement §13b forbids, one level below the fold. `test_c_undo_bound.cpp`
   encoded it: its "a child of a blended group covers the seam" case asserted
   the undo bound was strictly WIDER than the query. That case is updated, not
   weakened — the requirement is now asserted against the child's own geometry,
   and a new subcase holds the query and the undo bound EQUAL, which is the
   property this change needs.
2. **`layer_influence_bound` is not widened in place** and now says in its own
   comment why it cannot be: it takes a `Layer`, and the folds above are a
   property of the stack. `node_reach_bound` carries the same sentence.

**Blocker 3 is real as a contract violation and its stated consequence is not
reachable. Both halves matter.** The three gesture reaches genuinely carried no
fold term, and `GestureRegion`'s own contract is "It MUST cover everything the
bracket does — a region that does not is stale bricks". But a gesture's reach
has exactly one consumer, `clay_document::touch_regions`, and
`touch_region_locked` compares each seed's brick DILATED BY `band + pad`, where
`pad` is the document cull pad — a maximum over layers of that layer's chain pad
PLUS the folds above it, so `pad >= folds_from_layer_support(edited layer)` for
every document, by construction. The shortfall was inside the pad.

Measured, on a three-layer fixture over a 504-brick window (258,048 samples):
with the gesture's dilation removed a drag leaves 288 seeds where the fixed one
leaves 216, and the refill that follows is bit-identical to a cold document's —
0 stale samples, worst 0.0, either way. So "every dab left stale bricks" did not
happen, and the reason it did not is a number owned by the CULL PAD rather than
by the reach. That is the accidental kind of correctness this change exists to
remove: the coverage is not what `GestureRegion` promises, it would not survive
a second consumer of a gesture's reach, and it depends on stage 1's own fix
having landed first. The reaches are dilated, and the regression tests assert
the INVALIDATION (a seed in the shell the fold adds is dropped; a seed outside
both reaches is kept — exactly one of two survives) rather than pretending to a
stale brick that does not occur.

### §13d. A test the fix modified is not evidence for the fix

Raised by the host on 2026-09-06 while stage 2 was still uncommitted, and it is
the sharpest thing said about this change's own test discipline.

If a test asserted the too-small bound and now asserts the dilated one, it agrees
with the new code for the same reason it agreed with the old: **it was updated
to.** That is not an argument against updating it — it had to change. It is that
the evidence has to come from somewhere the fix did not touch.

**The question to ask of every test file this change MODIFIED, rather than
added:**

> Would this file still fail if the fix were reverted, and is the thing that
> fails a line that existed BEFORE?

- Both yes: it was a genuine regression test all along, and it caught the defect
  the moment the defect appeared.
- Only a line the fix added fails: the file is DOCUMENTATION of the new
  behaviour rather than a check on it. That is fine — as long as nobody counts
  it twice, in a report or in a review.

This is the same shape as a test that asserts only what CAN be read and therefore
passes on both sides of the change it exists to announce, arriving from the other
direction: **a test that moves with the code it tests has the same blindness as a
test that never moves.**

**Required of the third review:** for every modified test file in this change —
`tests/unit/test_c_undo_bound.cpp` is the one that prompted this, and it is not
the only one — apply the question above and report which category each falls
into. The revert proof is the instrument: flipping the fix and watching a NEW
assertion fail says something that the modified assertion passing cannot.
