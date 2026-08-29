# Design

## Context

Two costs in this library grow with how long an artist has been working rather
than with what they are doing right now, and both have the same cause: a
layer's field is defined by its whole edit list, and every evaluation walks all
of it.

`add-consolidation-policy` measured the degradation and deliberately never
baked. `add-sdf-sculpt-transaction` moved Smooth's whole-layer bake from
per-dab to once-per-gesture and wrote down, in its own §17.4, that this was not
the end of it. `reuse-the-tape-prefix` established that a stable prefix of an
edit list produces byte-identical output and that appending is the dominant
sculpt pattern. `seed-a-suffix-tape` established that a suffix can be continued
from a value on the stack — `compile_layer_prefix`, `compile_layer_suffix`,
`eval::eval_points_seeded`.

So the pieces for "sample the old half, keep the nodes" already exist. What did
not exist is the answer to the only question that matters: **when is a cached
VOLUME an acceptable stand-in for a compiled prefix?** Most of this document is
that answer, because it was measured wrong three times before it was measured
right, and each wrong version produced a field that looked fine and was off by
cells.

## Goals / Non-Goals

**Goals:**

- Interaction cost that follows the work being edited, not the work already
  done, with the artist's parametric history intact.
- A cache whose deletion is a performance event and never a correctness one.
- A Smooth gesture whose `begin()` evaluates nothing and whose dabs materialize
  what they read.
- A preview a host can patch brick by brick instead of copying per frame.
- Every accelerated path optional, with a correct slow path beside it.

**Non-Goals:**

- A boundary inside a group.
- Command-aware invalidation as the safety mechanism rather than as an
  optimisation.
- A serializable cache, in any form.
- Exposing the cache itself to C, Python or Swift in this change.
- Byte-parity with the whole-layer Smooth commit this replaces — it is not
  reachable, and the reason is stated below rather than papered over.

## Decisions

### The composition is exact, and that was measured before anything else

Before any caching question, the fold has to hold: continuing a suffix tape
from a seed must equal compiling the document.

> **`compile_layer_prefix` + `eval_points_seeded(compile_layer_suffix)` is
> BIT-IDENTICAL to `compile_document`, over 20,000 random points. Zero
> difference, not a tolerance.**

That is the foundation and it is unconditional. Everything after it is about
substituting a VOLUME for the prefix tape, so every error introduced from here
is attributable to sampling and to nothing else.

### The far-bound rule

A sparse `FieldVolume` answers two different questions depending on where it is
asked. Where it stores samples, `eval` interpolates them. Where it does not,
`eval` returns a conservative far bound — a number chosen so that a marcher
stepping by it cannot cross the surface, and emphatically **not** the distance
the history actually had there.

That distinction is invisible to a caller and fatal to a composition. Measured,
seeding a suffix from a prefix volume and comparing against the full walk:

| where the window sits | worst error |
|---|---:|
| the prefix volume stores every sample of it | **3e-7** — float rounding |
| the prefix volume stores none of it | **0.27 — about 14 cells** |

The 14 cells barely move with cell size or with blend width. It is categorical,
not a tuning problem: a blend in the suffix folded onto a far bound is a smooth
union with a number that was never a distance, and the result is wrong by
whatever the far bound happened to be.

So the rule is a hard gate rather than a tolerance:

> **The volume may seed a window only where it stores every sample of that
> window. Anywhere else, the prefix TAPE is evaluated for that window instead.**

Both answers are correct; only the cost differs. Two consequences follow that
are worth stating:

- The decision is per WINDOW, not per point. The fast path stays a straight
  loop and the slow path is one extra tape evaluation, rather than a branch
  inside the inner loop with two evaluators half-used.
- For `block_fill` the window is one BRICK, not the 512-brick run
  `sample_blocks` walks. One uncovered brick would otherwise drag a whole run
  onto the slow path, and coverage is a brick's question to answer.

`fallback_windows` and `seeded_windows` are counters on the cache for exactly
this reason. A cache with a high fallback rate is not wrong — the output is the
walk's — it is **not working**, and that is a different bug with a different
cure, so the two have to be distinguishable without a profiler.

### The prefix is baked WITHOUT redistance, and over the WHOLE LAYER's region

Both of these were got wrong first, both produced a plausible-looking field,
and both are stated in `sdf_prefix_cache.cpp` rather than left to be
rediscovered.

**No redistance.** A bake's default post-process replaces every sample with the
distance to the surface those samples imply. That is exactly right for a volume
that is about to BE the layer, and exactly wrong for one that has to reproduce
an ACCUMULATOR: the suffix folds onto the number the prefix produced, and a
redistanced approximation of that number is a different number.

> With redistance on, a bake through the cached source differed from the full
> walk by **0.063 on its own lattice — two cells** — where the raw samples
> differ by float rounding.

`compact` rides along, because it only runs after a successful redistance and
because dropping bricks would also shrink the region the far-bound rule calls
covered.

**The whole layer's region, not the prefix's own.** `sample_blocks` takes its
lattice origin straight from `region.min`, so two volumes share a lattice
exactly when they share a region and a cell size. A seed read off a lattice it
shares with its consumer IS the stored sample; one read off a lattice half a
cell away is an interpolation of two.

> With the prefix baked over its own padded bounds, a bake through the cached
> source differed from the full walk by **0.0074** on the consumer's lattice.
> Over the layer's region it is **3.3e-7**.

It is also right on its own terms: the prefix must answer wherever the SUFFIX
might need it, and a suffix grows the surface into places the prefix never
reached. The prefix's own bounds are precisely the region that fails to cover
that.

### What "correct" means for `SdfSourceField`, exactly

This is a SAMPLING source, and it is a different promise at three kinds of
point. Stating all three is the point; a source that claimed to be "the field"
would be lying at the second one.

| where | worst error | what it is |
|---|---:|---|
| on the lattice the prefix was built for | 3.3e-7 | float rounding |
| between lattice points | 7.6e-3 at a 0.03 cell | a quarter of a cell — ordinary trilinear interpolation |
| outside the prefix's stored bricks | exact | the far-bound rule sent it to the tape |

The middle row is not a defect and it is not hidden. It is the same fidelity a
consolidation of the same prefix would have, and it is NOT the walk's answer. A
consumer that needs the exact field at arbitrary points wants the walk, and
gets it by passing no cache.

The first row is the one that makes the cache usable for Smooth at all, whose
working field is that very lattice: a seed read there is the stored sample
rather than an interpolation of two. `block_fill()` over the same region gives
it, because of the `region.min` origin rule above.

### `session/`, not `scene::Document` or `scene::Layer`

The cache is DERIVED (recomputable from the document), DEVICE-DEPENDENT (a cell
size and a memory ceiling belong to a session and a machine) and NEVER
SERIALIZED. All three disqualify the same owners the sculpt transaction's
design disqualified, for the same reasons and one more:

- On `scene::Document` or `scene::Layer` it becomes a state the format has to
  describe. A `.clayspace` saying "roots 0..K are also over here as a volume at
  0.03" is a second copy of the truth, and every loader, journal replay and
  crash recovery would have to decide whether to believe it.
- A resolution and a byte budget stored in the artwork is a cache policy in the
  file — the same argument `ConsolidationParams` already makes about why a
  document has no intrinsic sampling resolution.

`session/` is where things that live for a sitting and are never saved already
live, so the cache goes where its lifetime already has a home.

### The prefix digest is separate from `layer_fingerprint`, and it is the safety net

**Why separate.** A whole-layer digest would be the wrong key. Appending a root
changes it, and the represented prefix did not change at all — so under a
whole-layer key, every append (which is what a stroke IS) throws away a cache
that is still perfectly valid. `layer_prefix_fingerprint(layer, K)` covers the
layer's own properties plus roots `[0, K)` and nothing after them. The count is
mixed in as well as bounding the walk, so a prefix of three roots and a prefix
of four that agree on the first three are still different digests: a boundary
is part of what is being identified.

The two cannot drift, because `layer_prefix_fingerprint(l, l.sdf->roots.size())`
IS `layer_fingerprint(l)` by construction, and a test pins that.

**Why a digest rather than command-aware invalidation.** `invalidate_layer`
exists and is an optimisation. The re-check on every `find` is the guarantee.
The asymmetry is the whole argument:

> **A missed invalidation is wrong geometry. A redundant one is only slow.**

A command-aware scheme has to be right for every mutating entry point in the
codebase, for ever, including the ones added next year. A digest is computed
from what the layer holds right now, so it cannot be forgotten.

**Why it reads content and not the pointer.** An instance layer shares its
`SdfContent`, so an edit through a sibling instance IS an edit to this layer —
and the shared pointer has not moved. Hashing the pointer would serve a stale
prefix in exactly the case `instance-a-layer` exists to support. Shared
immutable payloads (a volume's samples, a gate) are still folded in by identity
and size, because those are `shared_ptr<const T>` and a replacement is a
different object; that costs an address rather than megabytes.

64 bits collide in principle. The consequence of a false MATCH is a stale
prefix — wrong geometry, which is why the digest covers everything an edit can
change and mixes floats by their BITS so a value that prints the same cannot
fool it. A full structural compare costs the thing the digest exists to avoid.

### Materialization FORCE-stores rather than adding a `kBrickUnknown` sentinel

A lazily filled working field has to tell "this brick holds no surface" apart
from "nobody has asked for this brick yet". The tempting fix is a third state
in the index.

It was rejected. `kBrickEmpty` already MEANS something a reader is entitled to
believe: no surface here, and here is which side — a sign and a distance. That
reading is relied on by the marcher, the mesher, the brick cache and the blob
validator. A third state hidden inside a two-state sentinel would need changes
at roughly nine call sites plus the blob validator, and every one of them is a
place where the old reading is still correct for every volume that is not a
lazily-filled working field.

So stored-ness IS the record of what has been filled in: a materialized brick
stores samples, full stop, even one whose samples all lie past the band. It
costs the samples of a brick that says nothing interesting. That is the price
of not overloading a sentinel that already has a meaning, and it is paid only
by working fields.

It has one measurable consequence, and it is the honest cost of the choice:

> The lazy working field differs from a whole-layer relax of the same lattice by
> **5.5e-5 in the band** — about a thousandth of a cell. At the BAND EDGE a
> written sample's stencil reaches bricks the lazy field force-stored and a
> normal bake left empty, so a tap exists here and is missing there, and relax
> renormalizes over a different neighbourhood.

### `materialize_region` beside `resample_region`, not instead of it

`resample_region` genuinely creates bricks that were `kBrickEmpty`, so "use the
one that exists" is a real alternative and it was measured against.

It re-DECIDES sparsity from the values `fill` produced, which is exactly right
for an operator that DISPLACES a surface into bricks that held nothing — and to
do it, it rebuilds the whole sample store: `O(stored)` plus a `shrink_to_fit`
plus a full-lattice chamfer to re-derive the far bounds. That is a bake's cost,
and paying it per dab is the term this change exists to remove.

    rewrite_region()     values change, sparse support fixed
    resample_region()    values change, sparse support re-decided,  O(stored)
    materialize_region() sparsity taken as GIVEN by the caller,     O(added)

`materialize_region` APPENDS. The far bounds are deliberately not re-derived:
they describe the distance from a sample-free brick to the nearest stored one,
and a caller materializing a region is by definition going to read inside it,
where the stored samples answer.

### The dependency halo is a brick DIAGONAL

A dab must materialize every brick its relax will READ, and getting this short
does not crash — it silently smooths against a smaller neighbourhood.

The reasoning, in the order the bricks are involved:

1. `rewrite_region` writes every brick whose BOX meets the ball. So a written
   sample can sit a whole brick **diagonal** beyond the ball's surface — not a
   brick edge, which is the mistake this had first.
2. From that sample, relax's stencil reaches `radius_cells` further.
3. `relax` silently widens a falloff narrower than its kernel, because a
   falloff that narrow cannot hide the seam the kernel makes. The region it
   rewrites is the WIDENED one, so the halo reproduces that widening rather
   than guessing.
4. A tap landing in a brick nobody materialized does not read a wrong number,
   it reads NOTHING: `sample_at` returns nothing for an unstored brick and
   relax renormalizes over the taps that exist.

Point 4 is why this is worth being careful about. The failure is a **seam at a
brick face** — invisible except as a measurement, and permanent in the
committed volume. `sqrt(3)` is the diagonal and `1.75` is it rounded up: a
brick of margin costs a brick of fill, and being short costs correctness.

The margin is on the ARGUMENT, not on a number. The whole-layer comparison this
was chased with turned out to be dominated by force-stored bricks past the band
(above), so widening the halo did not move it; what stands behind the constant
is the derivation, not a fitted error curve.

### Commit assembles once, and cannot be byte-identical to what it replaces

A LOCAL working field is not a layer, so a commit cannot simply install it.
`commit` samples the whole layer once through the SAME source the dabs
materialized from and on the SAME lattice, materializes the edited region (a
dab can move the surface into a brick the source classified as empty, and
`rewrite_region` writes only bricks that store samples), overlays exactly the
samples the dabs changed, and post-processes once.

The overlay is identity outside the edited region by construction — where the
working field has no sample the source's value stands — which is precisely what
`rewrite_region` requires.

**The one semantic difference, stated because it is real.** The old whole-layer
path relaxed a REDISTANCED bake. This redistances a RELAXED field.

> Measured on the surface an artist sees: **0.0037, or 0.073 of a cell.**

Both are sound signed distance fields and neither is an approximation of the
other. Exact parity is **not reachable** for a local field, and the reason is
structural rather than a missing effort: the old path's starting point was
globally post-processed, so reproducing it would require the global
post-process the lazy path exists to avoid. Four byte-identity tests from
`add-sdf-sculpt-transaction` were therefore rewritten to the contract that is
true — parity against a whole-layer relax of the same lattice, split by whether
the sample is in the band — and the remaining gap is MEASURED and bounded
rather than asserted away.

This is the only place in the change where an already-shipped guarantee gets
weaker, and it is weaker in a way an artist cannot see and a test can.

### A gesture that changed nothing installs nothing

Pointer-down and pointer-up with no effective dab between them — no update, a
strength of zero, a mask that froze everything — commits **nothing at all**: no
volume, no undo entry, no consolidation, and the layer keeps every parametric
item it had.

Under the old whole-layer path this mattered less, because the working volume
was a faithful bake of the layer and installing it was merely wasteful. Under a
lazy path it would be worse than wasteful: it would collapse an artist's edit
list on a gesture they did not make. A no-op must not be a way to lose history.

### The C delta takes NOTHING on a short buffer

Three decisions, and each one is forced by what the call does to the state.

**Nothing on a short buffer.** Taking is what CLEARS the delta. A partial drain
would strand bricks that no later call reports, because nothing records that
they were skipped. So `clay_sdf_smooth_preview_delta_take` returns
`CLAY_ERROR_BUFFER_TOO_SMALL`, takes nothing, and writes the counts it needs
into the out-parameters — the caller grows and asks again. The alternative
(drain what fits) makes a short buffer a silent corruption of the preview
rather than a retry.

**Deduplicated by BRICK.** A dab materializes a brick and then relaxes it, so
the same coordinate arrives twice by construction. Folded once per update over
what that update appended, rather than per push: a host uploading a brick twice
is paying for the bookkeeping this exists to save.

**The generation moves only when the preview does.** A dab that changed nothing
leaves it where it was. That is what lets a host tell a duplicate read from a
skipped frame, and drop an upload it began against an older one; a counter that
moved on every call would say nothing. Taking does not move it either — it
names the state the caller now HOLDS, not what is waiting.

The delta accumulates until taken, so skipping a frame loses nothing. Reading
twice is told the same thing twice, which is the property that makes
`_delta_info` safe to call every frame to decide whether to bother.

### `check_layering` gains `session -> eval`

A cached prefix is only worth having if the suffix that continues it can be
evaluated onto it, and `eval::eval_points_seeded` is the one function that does
that.

The edge is free, on the same two facts the `session -> brush` edge rests on:
`eval`'s own set `{parallel, kernel, math, scene, field}` is already a subset of
`session`'s, and nothing in `eval` includes `session`. So no transitive edge and
no cycle. It is also the ordinary shape for a module at this height — `mesh`,
`pick` and `io` all depend on `eval`, and `src/mesh` reaches the backend
registry exactly as this does.

The `scene::BakePointEval` injection pattern exists for the OPPOSITE case: a
module BELOW `eval` that must not name it. `session` is above, so injecting
here would be ceremony that hides a dependency the table should state.

## Risks / Trade-offs

**A stale prefix is wrong GEOMETRY, not a slow path.** This is the sharpest
risk in the change, and it is why the digest is checked on every `find` rather
than trusted from an invalidation call, why it reads content rather than
pointers, and why it covers the layer's own properties as well as its roots — a
prefix built under one mirror and reused under another is a different field.
A 64-bit collision remains possible; the mitigation is coverage and bit-exact
float mixing, not width.

**A cache that never hits is a memory cost with no benefit.** Nothing detects
that automatically. `seeded_windows` / `fallback_windows` / `hits` / `misses` /
`builds` are the instrument, and they are on the cache rather than behind a
build flag because a host tuning `keep_live_suffix_roots` on a device needs them
in production.

**`SdfSourceField::open` never builds, so the first gesture on a layer is never
accelerated.** Deliberate: a build at pointer-down is the whole-layer cost this
change removes. It means a host that never calls `build` gets none of the
benefit and all of the code, and the header says so. Scheduling that work
somewhere an artist is not waiting is a host's problem and is not solved here.

**Interpolation error between lattice points is now reachable through a path
that used to be exact.** A caller that passes a cache and then asks for
arbitrary points gets a quarter-cell answer where it used to get the walk's.
Mitigated by the source stating all three regimes explicitly, and by the fast
path being opt-in — but a caller that opts in without reading is measuring
something slightly different from what it was.

**The lazy commit is not byte-identical to the path it replaces.** 0.073 of a
cell, structural, unreachable. Anything downstream that pinned those exact bytes
had to be rewritten, and four tests were. The mitigation is that the gap is now
a measured, bounded, named quantity rather than an implicit one.

**The dependency halo is derived, and a change to `relax`'s stencil silently
invalidates it.** If relax grows a wider kernel or a different widening rule and
`dependency_region` is not updated, the result is a seam at a brick face that no
existing test would catch by shape. Held only by the in-band parity test's
5.5e-5 bound and by the reasoning being written down at both ends.

**Force-storing bricks past the band costs memory in a working field.** The
alternative was a third index state at nine call sites plus the blob validator.
Paid only by working fields, which are transient by construction.

**The whole thing can be deleted.** That is the point, and it is also the risk:
a host that drops the cache under load gets correct output at the old cost, and
"the app got slow" is a harder report to act on than "the app got wrong". The
counters are the answer to that, which is why they are part of the capability
rather than a debug aid.
