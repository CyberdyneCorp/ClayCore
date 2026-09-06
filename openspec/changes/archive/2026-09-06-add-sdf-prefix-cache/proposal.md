# Proposal: a dab should not re-evaluate the history it is sculpting on top of

## Why

`add-sdf-sculpt-transaction` shipped the gesture lifetime and left one number
standing. Its §14.6 measured `SdfSmoothTransaction::begin()` deliberately —
"the number that decides whether local checkpoints are worth building" — and
its §17.4 is still open: **LOCAL CHECKPOINTS for Smooth, so `begin()` stops
being O(model)**. This is that task, plus the reason `begin()` was O(model) in
the first place, which turned out to be the bigger of the two.

**P0-1: every evaluation over worked geometry re-walks every item that ever
touched it, and almost none of them changed.** Measured (#306) at a 0.05
voxel, one dab into 12 bricks:

| items in the layer | one dab |
|---:|---:|
| 200 | 0.23 ms |
| 5,000 | 1.86 ms |
| 50,000 | 18.07 ms |

Consolidating the layer shows where the floor is — the same work falls to
about a third — and the price is the artist's history: a bake discards the
parameters of everything it absorbs, which `consolidate.h` is explicit about
and which no library should do unasked. So the cure that exists is one an
artist cannot afford, and the cost that remains grows with how long they have
been sculpting. That is the wrong direction for the one number an interactive
tool is judged on.

**P0-2: `SdfSmoothTransaction::begin()` bakes the whole finite layer.** That
was the honest first version and its design said so: a local working patch
needs a rule for what it means where it meets the field it was cut from, and
that rule is new correctness surface of the kind that fails as a visible seam.
The cost was moved from per-dab to once-per-gesture, at the moment a brush
cursor can cover it. It is still O(model), it lands at pointer-down where the
artist is already waiting, and on a large layer at a fine cell size it is felt.

**P0-3: drawing the preview costs the whole working volume per frame.**
`clay_sdf_smooth_preview_item` hands back a COPY of the working volume, which
is the right refusal (a borrowed view of samples the transaction is still
mutating is exactly the bug the transaction exists to prevent) and the wrong
shape for a per-frame loop: a dab moves a ball of bricks and the host
re-uploads the model. The dirty bounds say *where* to look and nothing says
*what to fetch*.

All three are the same missing thing, twice over: nothing in the library can
say **which part of a layer's history is old and stable**, and nothing can say
**which part of a working volume is new**.

## What Changes

**An ephemeral field cache for a stable edit-list PREFIX, in `session/`.**
The same split consolidation makes, without the loss: sample the old roots into
a volume and KEEP THE NODES.

    roots [0, K)          roots [K, N)
    ───────────────       ─────────────
    cached FieldVolume  + live suffix    ==  the layer's field

The document is untouched, every item stays editable, and the cache is derived
state a host may drop at any moment. **Deleting every entry is semantically
equivalent to flushing a CPU cache: slower, and identical output.** That is the
rule every decision in this change is measured against, and the tests assert
the output half directly against a full walk.

It can be exact because the compiler already emits a layer's chain as a FOLD at
item boundaries: after every root the stack holds exactly one value.
`seed-a-suffix-tape` named that boundary (`compile_layer_prefix`,
`compile_layer_suffix`, `eval::eval_points_seeded`). Measured over 20,000
random points, prefix-tape-then-seeded-suffix is **BIT-IDENTICAL** to compiling
the whole document — not a tolerance, zero difference.

**The far-bound rule, which is the whole correctness argument.** A sparse
`FieldVolume` has two regimes: where it stores samples `eval` interpolates
them, and where it does not `eval` answers with a conservative FAR BOUND — a
number chosen so a marcher cannot overstep, and emphatically not the distance
the history had there. Measured, seeding a suffix from a prefix volume against
the full walk:

| the prefix volume | worst error |
|---|---:|
| stores every sample of the window | 3e-7 (float rounding) |
| stores none | **0.27 — about 14 CELLS** |

and 14 cells barely moves with cell size or blend width, so it is categorical
rather than a tuning problem. **Therefore the volume seeds a window only where
it stores every sample of it, and anywhere else the prefix TAPE is evaluated
instead.** Correct either way; only the cost differs. `fallback_windows` counts
how often the slow answer was needed, because a cache with a high fallback rate
is one that is not working rather than one that is wrong.

**A Smooth gesture whose working field is materialized around the brush.**
`begin()` now evaluates NOTHING: it compiles the layer, allocates the lattice
with no stored samples, and takes a digest. A dab materializes the bricks its
relax will READ — its rewrite region plus the stencil's reach — and nothing
else. A later dab over the same place materializes nothing; one that reaches
past it materializes only the new bricks. Commit assembles the layer once,
through the same source the dabs materialized from, and overlays exactly what
the dabs changed.

**An incremental preview delta, in C++ and across the ABI.** Which bricks hold
bytes a consumer has not seen — the ones a dab materialized, and the ones its
relax actually moved — deduplicated by brick and accumulated until taken, with
a generation that moves only when the preview does.
`clay_sdf_smooth_preview_delta_info` / `_take` are the two-call size-query
shape `clay_brick_cache_take_dirty` and `clay_voxel_take_dirty_chunks` already
use.

**Four smaller pieces, each the half of an existing thing that was not
separable.**

- `scene::bake_tape` — the half of `bake_layer` that does not need a layer.
  The prefix cache samples a tape belonging to no layer, and there must not be
  a second definition of what a baked volume is. `bake_layer` is now "compile a
  local view" plus this.
- `FieldVolume::empty_lattice` — the index, the brick counts and the far
  bounds, and no samples. What a lazily filled working field starts from, and
  cheap by construction.
- `FieldVolume::materialize_region` — FORCE every brick meeting a region to
  store samples, appending rather than rebuilding the store. `resample_region`
  genuinely creates bricks from `kBrickEmpty`, but it re-DECIDES sparsity and
  rebuilds the whole store to do it — `O(stored)` plus a `shrink_to_fit` plus a
  full-lattice chamfer, which is a bake's cost per dab.
- `FieldVolume::BrickCoord` / `read_brick` / `brick_origin`, and an
  `out_changed` on `rewrite_region_tallied` — a bounding box says where to
  look; these say what to fetch.

**One shared layer digest.** `src/session/layer_digest.h`, private to
`src/session`, now carries `mix_layer_head` and `mix_roots` for two consumers
with the same correctness requirement and different scopes: a transaction
digests a whole layer, the cache digests the first N roots. A second
implementation that forgot a field would be a cache silently serving a stale
prefix, which is the one failure neither can tolerate.
`layer_prefix_fingerprint(l, l.sdf->roots.size())` equals `layer_fingerprint(l)`
by construction, and a test pins it.

## Capabilities

### New Capabilities

- `sdf-prefix-cache`: an ephemeral, droppable field cache for an old and stable
  edit-list prefix — what it may stand in for and what it may never stand in
  for, when it must be considered stale, that its presence changes cost and
  never output, and that a host owns the memory ceiling because a device does.

### Modified Capabilities

- `sdf-sculpt-transaction`: a Smooth gesture no longer bakes at begin. What was
  "evaluates its source once, at begin" becomes "materializes what the brush
  reads, and assembles the layer once at commit"; the byte-identity between a
  live sequence and a standalone one is replaced by the contract that is
  actually true of a local field, and the difference is MEASURED rather than
  asserted away. Plus: a gesture that changed nothing installs nothing, and a
  host can take the preview incrementally.
- `sdf-kernels`: a volume can be built as a bare lattice and filled in later; a
  region can be materialized without re-deciding sparsity; a rewrite reports
  WHICH bricks moved and not only how many; and a brick can be read out by
  coordinate.
- `scene-model`: an already-compiled tape can be baked into a volume on the
  same terms a layer can, with the caller owing the frame and the colour
  question.
- `c-abi`: a host can drain a Smooth preview brick by brick instead of copying
  the working volume per frame.

## Impact

**Additive, and the accelerated half is optional everywhere.** New headers
`clay/session/sdf_prefix_cache.h` and new entry points on `field`, `scene` and
the ABI. A null cache, a zeroed policy or a declined boundary is the full walk:
correct, and slower. Nothing in the library requires a cache to exist, and no
test asserts one is being used except the ones that exist to assert exactly
that.

`src/session/sdf_prefix_cache.cpp` joins the library in `CMakeLists.txt` and
`tests/unit/test_sdf_prefix_cache.cpp` joins `tests/CMakeLists.txt`.
`tools/check_layering.py` gains **`session -> eval`**, documented in place:
`eval`'s own set is already a subset of `session`'s and nothing in `eval`
includes `session`, so it adds no transitive edge and no cycle; `mesh`, `pick`
and `io` already depend on `eval` at the same height.

**One behavioural change to an already-shipped surface, and it is stated rather
than hidden.** The old whole-layer commit relaxed a REDISTANCED bake; this
redistances a RELAXED field. Both are sound signed distance fields and neither
is an approximation of the other, so exact parity is impossible for a local
field — the old start point was globally post-processed. Measured on the
surface an artist sees: **0.0037, or 0.073 of a cell.** Four byte-identity
tests from `add-sdf-sculpt-transaction` were rewritten to the contract that is
true, and the new ones measure the gap rather than assert it away.

No file-format change and no on-disk version bump: nothing here is
serializable, by design.

## Non-goals

**Caching a boundary inside a group.** A group is one root and its children are
not a boundary the fold reaches. `prefix_boundary_for` returns a root COUNT and
not a node precisely so this stays a P0 restriction rather than a shape the API
has to grow out of.

**Command-aware invalidation as the safety mechanism.** `invalidate_layer` is
an optimisation. The digest re-check on every `find` is the guarantee, because
a missed invalidation is wrong geometry and a redundant one is only slow.

**Reaching the prefix cache from C, Python or Swift.** A cache is a session's
policy and a device's memory ceiling, and the shape of that across an ABI is
not guessed at here. The C surface in this change is the preview DELTA only.

**A cache that survives a save.** A `.clayspace` describing "roots 0..K are
also over here as a volume at 0.03" is a second copy of the truth for a loader
to reconcile. Nothing here is serializable and nothing here should become
serializable.

**Making the cache load-bearing.** Every accelerated path in this change has a
correct slow path beside it and takes it whenever the fast one cannot be
proven. That is why `SdfSourceField::open` never BUILDS: it is the call a
Smooth transaction makes at pointer-down, and a bake there would put back the
exact cost this change removes.
