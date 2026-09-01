# Design: the extreme-poly sculpt runtime

## Context

This change optimises an architecture rather than supplying one, so the first
job of the design is to say what is already there. Everything below was read
out of the tree at `a44b1f5`, not inferred from the proposal.

**The chunk unit already exists — three times.** `mesh::SurfaceLeaf` in
`include/clay/mesh/dynamic_bvh.h` already claims, in its header comment, to be
simultaneously the BVH leaf, the brush candidate set, the parallel work unit,
the normal-recompute unit, the dirty-tracking unit and the host's upload unit.
It already marks dirty by epoch rather than by a hash set per dab, already
tracks `quality()` and already refuses to rebuild on its own behalf. It is also
bound to `DynamicSurface` and reaches neither of the other two representations.
Beside it, `MeshSculptor` tracks dirty WELD CLASSES with a list-plus-mark reset
and refits `Bvh` leaves it never names, and `MultiresSurface` tracks dirty BASE
PATCHES with three revisions and no chunks at all. Task 2.2 says one unit and
says a subsystem inventing a second must justify it; the honest position is that
the tree already has three and this change owes the justification for the one it
keeps.

**Much of §4 and §5 exists for multires and only for multires.**
`MultiresMemory` already separates authoritative from rebuildable in the order a
host should reach for it. `drop_inactive_caches`, `drop_intermediate_caches`,
`drop_all_caches`, `level_resident`, `cache_generation` and `detail_checksum` all
exist, and `detail_checksum` exists precisely so a test can assert that dropping
and rebuilding changed nothing that matters — which is gate 4.6 with no document
around it. `MultiresPreflight` already reports `persistent_bytes` and
`peak_bytes` and already refuses with a typed error before allocating. None of
this is reachable for an adaptive surface or a fixed mesh, none of it takes a
pressure level, and none of it rolls up.

**Part of §6 exists for the adaptive surface and only for it.**
`clay_dynamic_chunk_info`, `clay_dynamic_surface_dirty_chunks` and
`clay_dynamic_surface_copy_chunk` are shipped ABI and already state the
caller-owned-buffer and no-borrowed-pointer rules this change would otherwise
have had to invent. They carry ONE revision where 2.1 wants four, they have no
acknowledgement (only an all-or-nothing `clear_dirty`), and they take a
`clay_dynamic_sculptor`.

**The report cannot see the surface tier, and not by omission.**
`io::document_memory` walks a `ClaySpaceDoc`, and a `ClaySpaceDoc` does not hold
a `MultiresSurface` or a `DynamicSurface` — the C ABI comment is explicit that
the hierarchy is opaque and owning and that the host holds it beside its
document. So the scene-model requirement cannot be met by adding six lines to
`MemoryReport`; it needs a seam by which a host-held surface contributes to a
document figure.

**The allocation harness exists and already reads bytes.**
`tests/unit/test_sculpt_allocation.cpp` replaces `operator new`, counts both
calls and bytes, and its own comment gives the reason: a whole-surface sweep
into a vector is ONE allocation however big the surface is, and only its size
gives it away. Gate 7.4 extends this file; it does not need a second harness.

**The pool runs a nested `parallel_for` inline by design.**
`include/clay/parallel/thread_pool.h` says so and calls the guard a decision
placed before the first nested caller. Task 3.6 is therefore a rule this change
must not break rather than a mechanism it must build.

Two claims in the brief needed correction. `add-bvh-refit` is archived at
`2026-08-24` and `add-mesh-multires` is complete (52/52) but not yet archived —
both landed, as stated. `add-shared-brush-kernels` is ALSO complete (41/41) and
on this branch's base: `sculpt_kernels.h`, `brush_model.h` and
`sculpt_workset.h` are present and `SculptWorkset` already separates the read
halo from the write region. What is not on this base is `add-mesh-sculpt-layers`
(0/49), which is what the layered rows of 7.1 wait on.

## Goals / Non-Goals

**Goals:**

- One chunk unit, one dirty set, one transport, across all three surface
  representations.
- A host can answer a memory warning with three numbers it may act on, for a
  document that includes host-held surfaces.
- A trim releases caches in a stated order and provably never the work.
- Every operation whose peak exceeds its result refuses before allocating, with
  arithmetic that reports a refusal rather than a small number on overflow.
- The performance principle becomes a gate: 20× the vertices at the same
  footprint is not 20× the dab.

**Non-Goals:**

- GPU-authoritative geometry. The transport goes to a host; editing stays on
  the CPU for undo, serialization, exact topology and crash recovery.
- A sculpt-layer stack. That is `add-mesh-sculpt-layers`, concurrent and not on
  this base; this change designs the ledger and the transport so that a layer
  cache is a category and a layer level is a partition, and nothing more.
- Detail compression or quantization. `detail_field.h` records why, and this
  change does not reopen it.
- Device detection anywhere in the portable core.

## Decisions

### D1 — One chunk TABLE with three partitioners, not one chunk GEOMETRY

**The question.** Task 2.2 demands a single unit. Three granularities already
exist and each is defensible where it stands: the fixed sculptor's weld class is
the identity its adjacency walk produces, the adaptive surface's leaf is the
only stable identity a surface with moving face slots has, and multires' base
patch is the only identity that survives subdivision. What exactly is unified?

**The choice.** `include/clay/mesh/surface_chunks.h` defines a `SurfaceChunk`
that owns bounds, a CSR span of faces, a chunk-local vertex map, and FOUR
revisions — topology, geometry, normals, attributes — plus a `ChunkTable` that
owns the epoch dirty set, the identity space and the transport. Each
representation supplies a PARTITIONER that decides only which faces are in which
chunk. The unit is the table and the revision quad; what differs per
representation is the partition rule, which is the one thing that genuinely
cannot be shared because the three do not have the same notion of a stable face.

The multires partitioner is the interesting one and it is where the "one
granularity" claim would have quietly failed. A base patch is not a fixed-size
unit: Catmull-Clark quadruples faces per level, so one base quad owns 1024 faces
at level 5 — four times the leaf target and sixteen times it at level 6. So the
multires chunk id is `(base_patch, quadrant at depth d)` with `d` chosen per
level to land the chunk on the target face count, and `d = 0` at levels coarse
enough that the patch is already small. The identity is still stable under an
edit, because a patch's subtree never moves between base faces, and the size is
now fixed. That is the same granularity keyed by the only identity subdivision
preserves — not a second one — and the header will say exactly that, because
task 2.2 requires a subsystem that looks like it invented one to explain itself.

**What the alternative would have cost.** Leaving each subsystem its own unit
costs the transport most: §6 would need three C entry points and a host would
need three code paths whose dirty sets mean different things — a weld class, a
face chunk and a base patch are not interchangeable and a host that treated them
as such would upload the wrong thing. It also makes the ledger unattributable: a
"chunk index" line in the memory report could not be one line. And it is
precisely the failure the sculpt-runtime spec names.

**The second rejected alternative is inside `SurfaceLeaf` today.** A chunk that
owns its faces in a `std::vector<FaceId>` is a heap object per chunk: at 20M
vertices and a 256-face chunk that is roughly 156k vectors, 156k allocations at
build, and a reallocation every time membership changes — which is a stamp, on
an adaptive surface. It also directly contradicts §6.2's "no heap object per
chunk per frame". The table therefore holds ONE CSR arena — a face array plus
per-chunk offsets, with a free list for the slack a split leaves behind — and
`SurfaceChunk` holds a span into it. This is not incidental: it is what makes
gate 7.4 reachable at all, because a stamp that splits a chunk must come out of
the arena rather than out of the allocator.

### D2 — The chunk size is not decided here; the EXPERIMENT and its decision rule are

**The question.** Task 1.1, and it says in terms not to adopt a number from
prior art.

**The choice.** This design fixes the experiment and the rule for reading it,
and leaves the number blank. `benchmarks/bench_surface_chunks.cpp` sweeps 64,
128, 256, 512 and 1024 target faces per chunk, across the three representations,
at footprints of 1k, 5k, 20k, 100k and 500k touched vertices, on a
fixed-spacing fixture whose extent grows. Six quantities, all named by the task:
chunk-query time; false-positive touched vertices (chunk-admitted minus
exact-footprint); normal-recompute time over the write region and its ring;
upload bytes per stamp; a locality proxy (bytes of position data resident per
chunk against the L2 working set); and topology mutation cost (split and merge,
which is the term that punishes a large chunk).

The decision rule is written now, before the data exists, so the number cannot
be rationalised afterwards:

> Minimise the P95 of (chunk query + gather + normal recompute + index update)
> at the 20k footprint, subject to false-positive touched vertices at or under
> 2× the exact footprint and upload bytes per stamp at or under 3× the bytes
> actually moved. Ties, and differences inside the run-to-run spread, break
> toward the SMALLER chunk, because mutation cost grows superlinearly in chunk
> size and the false-positive term grows linearly.

The null hypothesis to beat is 256 target / 64 min / 512 max — `DynamicBvhOptions`'s
current defaults, which are prior art in the strict sense: they are in this tree
and they were never measured here.

One size for the library, not one per representation. If the matrix shows the
optima differ by more than a single step of the sweep, that is a finding to
report and re-decide in this file, not to paper over with three constants — a
per-representation size is the second granularity D1 exists to prevent, arriving
by the back door.

**What the alternative would have cost.** Adopting 256 because three other
engines use it costs exactly what the reverted `add-item-spatial-index` cost:
a plausible structure whose build-to-query ratio nobody measured until it was
in. It also costs the ability to explain the number in a review, which in this
repository is the same thing as not having it.

**Status: task 1.1 is NOT ticked by this design.** The decision is deferred to
implementation with its rule pinned. Ticking a DECIDE task on an unrun benchmark
is the failure mode the task was written against.

### D2a — THE MEASUREMENT, AND THE ANSWER: 128 target / 32 min / 256 max

The benchmark has now been run and this section records what it said, because
D2 reserved the right to re-decide here rather than to paper a finding over with
three constants.

**The run.** `benchmarks/bench_surface_chunks.cpp` over a 2,076,481-vertex
fixed-spacing plane (1440 quads a side at 0.01), 40 repetitions per cell, four
stamp centres so a chunk boundary that happens to fall on the origin does not
decide the number. Load average 3.7–8.0 and stable within every run; three
repeats of the decision row and three of the mutation row. At the 20k footprint:

| target | P95(query + normals + index), 3 runs | false positives | upload | mutation P95, 3 runs | index bytes |
|---|---|---|---|---|---|
| 64   | 345, 348, 359 us | 1.45x | 2.20x | 9.53, 9.54, 10.55 ms | 91.6 MB |
| 128  | 337, 337, 354 us | 1.51x | 2.08x | 8.76, 8.79, 8.82 ms | 88.3 MB |
| 256  | 358, 359, 373 us | 1.64x | 2.06x | 8.41, 8.92, 9.25 ms | 77.7 MB |
| 512  | 367, 376, 407 us | 1.64x | 1.95x | 8.96, 9.04, 9.59 ms | 78.0 MB |
| 1024 | 893 us           | 1.64x | 1.84x | 9.50, 9.64, 10.47 ms | 73.3 MB |

Both constraints hold at every size, so the rule reduces to minimising the P95.

**Two things the harness got wrong first, and they are worth recording because
each reversed the ordering.** The query was a linear scan over every chunk
testing its bounds, which is O(chunks) and reported the query getting four times
FASTER every time the chunk size doubled — a measurement of the harness. It is
now a median-split tree over the chunk bounds, which is what a real query
descends. And the admitted-vertex count summed each chunk's vertex list, which
double-counts a vertex on a chunk boundary once per chunk it belongs to; a small
chunk has more boundary per face, so the double counting grew as the chunk
shrank and reported the 64-face partition admitting MORE vertices than the
1024-face one, which is backwards.

**The decision, and where it departs from the letter of the rule.** 64 and 128
tie on the term the rule minimises — their three-run ranges overlap — and the
rule says a tie breaks toward the smaller chunk. Its stated ground for that is
"mutation cost grows superlinearly in chunk size". **The measurement falsifies
that ground.** Mutation is a shallow U with its minimum at 128, not a monotone
climb: a 64-face chunk is twice the tree to refit and twice the split-and-merge
bookkeeping, and it comes out 8–20% worse than 128 and five times noisier. With
its premise gone the tie-break does not apply, so the tie breaks the other way,
to the size that is at the minimum of the term the rule got wrong.

**Against the null hypothesis.** 256 is beaten on the decision term by 5–6%,
outside the spread of either, and on false positives by 1.51x against 1.64x. It
wins on index memory by 14% (77.7 MB against 88.3 MB over 4.1M triangles), which
the rule does not weigh and which is worth naming as the cost of the change. The
move from 256 to 128 is real and it is modest; presenting a 5% P95 as a
discovery would be worse than not having measured.

**One size for the library.** `ChunkOptions` and `DynamicBvhOptions` both carry
it. The optima did not differ by more than one sweep step between the fixed
partition's terms and the adaptive surface's mutation term, so the condition
under which D2 said to report three constants did not arise.

**What the sweep did NOT settle.** The multires chunk depth. The achieved
distribution is exactly on target at every level of the fixture measured
(level 1 through 4, all 256 faces per chunk at a 256 target, one chunk of 64 at
level 0 where the whole cage is smaller than a chunk), so the depth derivation
lands where it says it does — but a cage whose base patch count is not a power
of four has not been swept, and the risk section's wording about a level
producing chunks far under target stands unmeasured for that case.

### D2b — THE SERIAL THRESHOLD, MEASURED: 32,768 vertices / 576 chunks

Task 3.7 asks for the dispatch threshold to be measured rather than guessed,
and it shipped guessed: `kVertexParallelGrain = 1024` and
`kChunkParallelGrain = 4`, under a comment asserting that "at a few hundred
vertices the dispatch is the measurement". `benchmarks/bench_parallel_grain.cpp`
sweeps a per-vertex sculpt pass — a falloff weight from a distance, applied
along the vertex normal, which is the shape of the weight pass and the
write-back the constant gates — serially and through `parallel::for_range`, at
64 through 131,072 vertices, 201 repetitions a cell. The rule was fixed before
the data, in D2's shape:

> The grain is the SMALLEST footprint at which the parallel form's P50 is at
> least 15% faster than the serial form's, and stays faster at every larger
> footprint measured.

**The run.** Four sweeps, load average 2.5 to 8.7 and unmoved within each run.

| vertices | serial P50 | parallel P50 | ratio |
|---|---|---|---|
| 1,024   | 1.9-2.3 us | 18.9-20.0 us | 0.10-0.12x |
| 8,192   | 11.0-11.9 us | 22.3-24.3 us | 0.47-0.49x |
| 16,384  | 20.2-22.7 us | 24.1-25.6 us | 0.81-0.93x |
| 32,768  | 39.2-42.7 us | 24.9-27.5 us | **1.45-1.58x** |
| 131,072 | 149-170 us | 32.5-40.8 us | 4.17-4.66x |

The crossover is 32,768 in every run and holds at every larger size. The
dispatch costs about 17-20 us at ANY size, which is the whole finding: the
shipped 1024 is not merely low, it is a point at which dispatching is TEN TIMES
SLOWER than running the loop.

**The chunk figure is a conversion, not a second sweep,** and the design records
why that is legitimate: a chunk-level dispatch runs the same per-vertex body
over the chunk's vertices, so the crossover is decided by the total work behind
one dispatch rather than by how it is addressed. The benchmark measures the one
quantity the conversion needs — 58.0 vertices per chunk at the D2a options —
giving 565, rounded up to 576. Up rather than down because the rule's two errors
are not symmetric: erring high costs a little parallelism on a medium dab,
erring low costs a dispatch on every small dab of every stroke.

**What this does NOT claim.** Nothing reads either constant. The stamp path is
serial — `MeshSculptor::stamp` dispatches nothing — so these are the numbers a
future chunk-parallel pass starts from, not a description of what the library
does today. Recording that is the point: the measurement's real content is that
a chunk-parallel stamp at the OLD values would have been a pessimisation at
every footprint this change benchmarks (1k to 500k, of which only 500k clears
the threshold), which is a conclusion that would have been invisible had 3.7
been ticked on the constants' plausibility.

### D3 — The memory profile is a new `memory` leaf module

**The question.** Task 1.2: a new module with a layering entry, or `io` beside
`io::MemoryReport`.

**The choice.** A new `memory` module, depending on nothing — `"memory": set()`
in `tools/check_layering.py`, beside `parallel` and `kernel`. It holds the
profile, the pressure level, the category vocabulary, the ledger and its three
roll-ups, the trim report, the scratch arena and the checked-arithmetic capacity
estimator.

The argument is structural and the layering table settles it. The profile is
READ by `mesh`: the scratch arena's hard bound, the deferral decisions in D4,
level residency, and the preflight budget are all things `mesh` must consult
per stamp. `io` is the TOP of the table — `io` includes `mesh`. So `mesh` may
never include `io`, and putting the profile there leaves two options, both bad:
thread every budget through call signatures from the host down (which puts
residency policy in the host, contradicting D5) or add a `mesh -> io` edge and
make the table cyclic. `parallel` is the precedent and `check_layering.py`'s own
comment records what the mistake cost last time: the thread pool was private to
a backend, so every mesher, every voxel verb, redistance and the per-brick cull
were serial because they could not legally reach it.

`io::MemoryReport` does not move. It stays where it is, gains the surface-tier
lines, and adds up ledgers that each representation fills — which is what its
header already says it does: "Each subsystem answers for itself; this header
only adds up."

**What the alternatives would have cost.** `io` costs the cycle above, or a
worse API. `scene` looks tempting — it is below `mesh`, so the include direction
works — and costs something subtler: `scene` is what gets serialized, so a
budget descriptor that landed there would drift into the file format, and a
host's device budget travelling inside a document to another machine is a bug
with a long fuse. `memory` also has to be reachable from `voxel`, `brick` and
`session` for the ledger to cover them, and a leaf module is the only shape that
is reachable from all of them at once.

### D4 — The interactive budget is a hint for the derivable and a contract for the committed

**The question.** Task 1.3, per deferrable item.

**The choice.** Item by item, and the split is not "cheap versus expensive" but
"can be recomputed exactly from what was committed" versus "IS what was
committed":

| Deferrable | Hint or contract | Why |
|---|---|---|
| Exact normals during a drag | hint | A function of positions. The stroke-end recompute is exact and the gate asserts it equals per-stamp recomputation. |
| Spatial index quality and rebuild | hint | A refit keeps a tree correct and only quality is at stake. `Bvh::quality`'s recorded finding — a rebuild helped one of five measured deformations and hurt two — is why this is deferred rather than automatic. |
| Display level | hint | What the host draws, not what the artist edited. |
| Cache residency, compaction, storage promotion | hint | Reconstructs bit-identically by construction; that is what makes it a cache. |
| Preview drain rate (chunks per frame) | hint | The acknowledgement in D6 makes a partial drain lossless. |
| The deformation, and the stroke's trajectory or sample decimation | **contract** | It is the result. |
| Topology decisions: split and collapse thresholds, remesh targets | **contract** | A deferred split changes the committed mesh, so the same stroke would produce different geometry on a slower machine. This tree spends real effort on determinism — chunking in slot order, per-platform golden hashes — and a budget-dependent topology throws all of it away. |
| Detail coefficients, layer content, masks | **contract** | Authoritative. |
| Brush strength, radius, falloff | **contract** | The artist set them. |

This is enforced by the shape of the type rather than by prose: `SculptMemoryProfile`
carries fields for the HINT rows only. There is no field a host can set that
reaches a contract row, so "a memory-saving mode changed my sculpt" is
unrepresentable rather than merely forbidden. The gate is the spec's own
scenario — the same stroke under a full and a constrained profile produces
byte-identical committed geometry.

**What the alternative would have cost.** One global "interactive mode" flag has
to mean something for topology, and there are only two answers: ignore it there,
which makes the flag a lie an API reviewer will find, or defer the split, which
makes the mesh a function of machine speed. A per-item table is more surface and
it is the surface that carries the meaning.

### D5 — The engine owns the mechanism and the order; the host owns the moment

**The question.** Task 1.4.

**The choice.** The engine owns what is droppable, the eviction order, the
reconstruction guarantee, the ledger and its own scratch bounds. The host owns
WHEN. Eviction happens at exactly three moments and nowhere else:

1. an explicit `trim(pressure)` call;
2. a residency change the host itself caused — `set_sculpt_level` /
   `set_display_level`, which already drops inactive caches today;
3. the scratch arena falling back to its soft bound, at a STROKE boundary only,
   never inside a pointer event.

Plus one guard the proposal's open question asks for directly: a `memory::MemoryPin`,
an RAII scope a serializer or a readback holds, under which `trim` becomes a
no-op that returns what it WOULD have released. A host that receives a memory
warning mid-save then gets an honest answer instead of a document mutating under
the writer.

**What the alternatives would have cost.** An engine that evicts on its own
high-water mark mutates the document behind a host that may be mid-save or
holding a readback. `cache_generation()` exists in `multires.h` precisely
because a released cache is a use-after-free waiting for memory pressure to
find it, and autonomous eviction would add a second invalidation source the host
did not cause and cannot predict. A host that must ask before every level switch
is the other extreme the proposal already names: a bad API, and one that costs a
round trip per switch to learn what the engine already knows.

### D6 — One representation-independent transport; the shipped dynamic path is generalised, not duplicated

**The question.** §6 asks for a revisioned dirty-chunk C ABI. Part of one is
already shipped for the adaptive surface.

**The choice.** A `clay_surface_view` — a non-owning handle a host obtains from
any of the three sculptors — and one set of chunk calls against it: capacity
query, bulk info fill, dirty-set drain, per-chunk copy, and acknowledgement. The
existing `clay_dynamic_surface_*` chunk entry points stay, byte-compatible, and
become thin forwards; they are shipped ABI and a host using them keeps working.

Four things the existing path lacks and this one has:

- **Four revisions, not one.** `clay_dynamic_chunk_info` carries a single
  `revision` plus two dirty booleans, which cannot say "geometry moved,
  connectivity did not, normals are still deferred". Separate topology,
  geometry, normal and attribute revisions let a host re-upload an index buffer
  only when connectivity changed — 6.3, and the spec scenario that names it.
- **Acknowledgement.** `clear_dirty` is all-or-nothing. A host that drains half
  a set and drops a frame must either re-upload everything or lose a change.
  `clay_surface_ack_chunks(view, indices, revisions, n)` retires a chunk from
  the dirty set only when its CURRENT revision equals the acknowledged one, so
  a chunk that changed again between the copy and the acknowledgement stays
  dirty. That is the one thing `clear_dirty` structurally cannot express.
- **Bulk info.** A `clay_chunk_info` array filled in one call rather than one
  struct per call. It is registered in `check_c_abi.py`'s
  `ARRAY_ELEMENT_STRUCTS` with its reason stated, beside `clay_brick_request`,
  which is the same case: a fixed layout a caller reads thousands of, where a
  `struct_size` per element would forbid the memcpy that is the whole point.
- **Staleness.** The readback echoes the revision the caller asked for beside
  what the engine is at now, so a stale result is identifiable rather than
  merely wrong — 6.4.

**What the alternatives would have cost.** A second parallel API per
representation costs three host code paths and re-imports the granularity
confusion D1 removes. Engine-owned buffers with a borrowed pointer and a
generation token costs a use-after-free the existing header already refuses in
writing, and the reason it gives — a mutation can move or free anything — is
more true at 20M vertices, not less.

### D7 — The allocation gate asserts bytes AND a surface-independent high-water mark

**The question.** 7.4, and the blind spot the brief names: a gate that counts
touches cannot see an O(surface) READ.

**The choice.** Extend `tests/unit/test_sculpt_allocation.cpp`, which already
counts bytes for exactly this reason, with a third assertion the existing two
cannot make. Count and bytes both go to zero for a warm stable-topology stamp;
but a `std::vector<char>` sized to the vertex count that is allocated ONCE
during warm-up and reused forever passes both — it is not an allocation after
warm-up and it costs no bytes after warm-up, and it is still O(model) storage
whose touch cost scales. So the gate also asserts that the scratch high-water
mark reported by 7.7's telemetry does not move between the 1M and the 20M
fixture at the same footprint. Three assertions, three different defects.

`memory::ScratchArena` is what makes zero reachable: soft bound tracking the
largest footprint of the last N stamps, hard bound from the profile, and above
the hard bound the work is processed in BLOCKS rather than allocated — 4.8, and
also what stops a 500k footprint on a constrained profile from becoming the peak
that kills the app.

**What the alternative would have cost.** Counting allocations alone is what
this repository has already been bitten by, twice by its own record: once in the
colour path, where a pre-stamp copy was a local, and once in a scale gate that
counted touches. Bytes caught the first. Only the high-water assertion catches
the third shape.

### D8 — One checked-arithmetic estimator, five callers

**The question.** 5.1–5.4.

**The choice.** `MultiresPreflight` is already the right shape — persistent and
peak separately, a typed error, allocates nothing, has no side effects. Its
ARITHMETIC becomes `memory::CapacityEstimate`, with checked multiply and add
that saturate into a refusal, and the other four operations named by 5.1 —
representation conversion, layer flatten, global remesh, serialization — take
the same struct and the same typed budget error. `MultiresPreflight` keeps its
name and its fields and is filled from it, so nothing that exists changes shape.

Build-then-publish and the existing cancellation token on all five, so a
refusal, a failure and a cancellation are the same outcome from the document's
point of view: unchanged.

**What the alternative would have cost.** Five bespoke estimates is five places
for `vertices * bytes_per_vertex` to wrap a `uint32_t` and report a small
number, and the failure mode of that bug is that the operation is ALLOWED —
which is precisely the outcome 5.3 exists to prevent.

## The files

### New

| File | Why |
|---|---|
| `include/clay/memory/budget.h` | `MemoryClass`, `SculptMemoryProfile` (hint-only fields, D4), `Pressure`, `MemoryCategory`, `MemoryLedger` with the essential/rebuildable/undoable roll-up, `TrimReport`, `MemoryPin`. |
| `include/clay/memory/capacity.h` | Checked add/multiply, `CapacityEstimate`, `BudgetError`. D8. |
| `include/clay/memory/scratch.h` | `ScratchArena`: soft bound from recent footprints, hard bound from the profile, block processing above it. 4.8, D7. |
| `src/memory/budget.cpp`, `src/memory/capacity.cpp`, `src/memory/scratch.cpp` | Their definitions. |
| `include/clay/mesh/surface_chunks.h` | `SurfaceChunk` (bounds, CSR face span, chunk-local vertex map, four revisions) and `ChunkTable` (CSR arena, epoch dirty set, partitioner seam). 2.1–2.4, D1. |
| `src/mesh/surface_chunks.cpp` | The table, the arena and its free list, the epoch dirty set. |
| `include/clay/mesh/surface_view.h` | The representation-independent read seam the transport, the ledger and the benchmark all go through. One place that knows all three surfaces so the C ABI does not have to. |
| `src/mesh/surface_view.cpp` | The three adapters. |
| `include/clay/mesh/maintenance.h`, `src/mesh/maintenance.cpp` | The deferred-maintenance queue: index rebuild, cache compaction, sparse-to-dense promotion, slot-pool compaction, serviced with a time budget between interactions. 3.5. |
| `benchmarks/bench_surface_chunks.cpp` | The 1.1 matrix and nothing else, so the chunk-size decision has one artefact. |
| `benchmarks/bench_extreme_poly.cpp` | The 7.1 matrix with the fourteen per-stage timers of 7.2. |
| `tools/bench_extreme_poly.py` | The driver: records `uptime` before and after every row, emits P50/P95/P99/max and RATIOS against the 1M row, and flags a row whose load average moved. |
| `tests/unit/test_surface_chunks.cpp` | Partition determinism, the revision quad, the epoch dirty set, chunk-local indexing. |
| `tests/unit/test_chunk_transport.cpp` | 6.5: reconstruct from the dirty stream and compare against the whole-surface path. |
| `tests/unit/test_memory_profile.cpp` | The ledger sums, the three roll-ups, constrained behaviour on a desktop. |
| `tests/unit/test_memory_trim.cpp` | 4.6 / 7.6: checksum unchanged, every dropped cache reconstructs identically, the pin. |
| `tests/unit/test_preflight_budget.cpp` | 5.2 / 5.3: refusal before allocation, overflow reports a refusal. |
| `tests/unit/test_scratch_arena.cpp` | Soft and hard bounds, block fallback above the hard bound. |
| `tests/unit/test_extreme_poly_scaling.cpp` | 7.3 and 7.5 as a ctest gate at sizes CI can afford, with the full matrix in the benchmark. |
| `tests/unit/test_c_surface_chunks.cpp` | The C transport, the acknowledgement, the stale readback. |

### Changed

| File | Why |
|---|---|
| `include/clay/mesh/dynamic_bvh.h`, `src/mesh/dynamic_bvh.cpp` | `SurfaceLeaf`'s per-chunk face vector, single revision and private epoch set move into `ChunkTable`; what stays is the adaptive PARTITIONER. This is the largest single edit and the one that makes D1 true rather than aspirational. |
| `include/clay/mesh/multires.h`, `src/mesh/multires_eval.cpp` | The `(base_patch, quadrant)` partitioner and a `SurfaceView`; `MultiresMemory` becomes a `MemoryLedger` filler. Detail summation is not touched beyond that — the sibling layers branch owns it. |
| `include/clay/mesh/sculpt.h`, `src/mesh/sculpt.cpp` | The fixed sculptor publishes a chunk table over the `Bvh`'s leaves and marks chunks beside `dirty_classes_`. Deliberately minimal: weight composition is not touched, so the rebase onto the shared-brush and layers branches stays mechanical. |
| `include/clay/mesh/detail_field.h`, `src/mesh/detail_field.cpp` | Report bytes into the ledger's categories; expose promotion as a maintenance item rather than an inline decision. |
| `include/clay/io/memory.h`, `src/io/memory.cpp` | The surface-tier lines and the three roll-ups, plus the seam by which a host-held surface contributes to a document figure. |
| `bindings/c/clay.h`, `bindings/c/clay_c.cpp` | `clay_surface_view` and the chunk transport, the profile descriptor, `trim`, the trim report, the ledger. `CLAY_ABI_MINOR` 74 → 77. |
| `bindings/python/pyclay_module.cpp` | 6.6: the same transport, so parity stays green. |
| `tools/check_layering.py` | `"memory": set()`, and `memory` added to the sets of `mesh`, `voxel`, `brick`, `session`, `io`. |
| `tools/check_c_abi.py` | `clay_chunk_info` in `ARRAY_ELEMENT_STRUCTS`, with its reason. |
| `CMakeLists.txt` | The new sources, the new benchmark targets, `VERSION 0.77.0`. |
| `tests/CMakeLists.txt` | The eight new test files. |
| `pyproject.toml` | `0.77.0`. |
| `tools/release_check.py` | The version row follows the three above. |
| `tests/unit/test_sculpt_allocation.cpp` | D7's third assertion. |
| `docs/09-brush-latency-and-coverage.md` | 7.10: the new representations' measured costs and the scaling ratios. |
| `docs/05-claycore-library.md` | The module map gains `memory`; the memory section gains the eviction order verbatim, because a host implementer needs it in prose and not only in a spec. |

## The gates, and what a failure of each means

| Gate | Where | A failure means |
|---|---|---|
| 7.3 locality | `test_extreme_poly_scaling.cpp` (CI sizes) + `bench_extreme_poly` (1M–20M) | Something in the chain from pointer event to dirty set is O(model). The per-stage timing in 7.2 is what says which stage, which is why a total alone was never enough. |
| 7.4 allocation | `test_sculpt_allocation.cpp`, three assertions | Count: a temporary in the stamp path. Bytes: a whole-surface sweep into one vector. High-water: a per-vertex scratch buffer allocated at warm-up — the shape the first two cannot see. |
| 7.5 preview | `test_chunk_transport.cpp` | The dirty set is over-reporting, or the transport is copying the surface. Either makes a host's frame cost follow the model. |
| 7.6 memory pressure | `test_memory_trim.cpp` | Either a trim touched authoritative content (checksum moves — the serious failure) or a cache does not reconstruct identically (the reconstruction differs — which means it was never a cache). |
| 6.5 correctness of the stream | `test_chunk_transport.cpp` | The dirty stream and the whole-surface path disagree, so a host drawing incrementally is drawing something the engine does not think it made. |
| 5.2 / 5.3 preflight | `test_preflight_budget.cpp` | Refusal after a partial allocation, or an overflow reporting a small number — which is worse than a wrong number, because it ALLOWS the operation. |
| 2.2 one unit | `test_surface_chunks.cpp` | Two representations disagree about which chunk a region belongs to, which is the granularity confusion arriving anyway. |
| 1.1 chunk size | `bench_surface_chunks` | If the three representations' optima differ by more than one sweep step, D2 says re-decide in this file rather than ship three constants. |
| Layering | `tools/check_layering.py` | The `memory` module gained an edge it does not need, or a module reached it that the table does not permit. |
| Parity | `tools/check_binding_parity.py` | The transport reached C and not pyclay, which is the failure that gate was built after. |

Benchmarks are run under the shared-box discipline: `uptime` before and after
every row, re-run any row whose load moved materially, and report ratios (20M
over 1M at the same footprint) rather than bare milliseconds, with P50, P95,
P99 and max. The fixture is fixed-spacing with growing extent — more of the
same geometry at the same detail — because a more finely subdivided sphere does
not hold the footprint constant and would make the locality gate measure
nothing.

## Risks / Trade-offs

- **`SurfaceLeaf` is shipped through pyclay** (`DynamicSculptor.chunk_count`,
  `.dirty_chunks`) and through the C ABI. D1 changes its internals; the design
  keeps every existing signature and semantic, which constrains the arena
  refactor more than a clean sheet would.
- **The rebase.** This branch is stacked last and does not carry
  `add-mesh-sculpt-layers`. The layered rows of 7.1 are scripted and recorded as
  awaiting the rebase rather than invented, and `sculpt.cpp`'s weight
  composition and `multires_eval.cpp`'s detail summation are touched only as far
  as chunk marking requires.
- **The multires chunk depth `d` is a second tunable** that D2's sweep does not
  directly cover: it is derived from the chosen target face count, but a level
  whose patch count is small may produce chunks far under target at low depth.
  The benchmark must report the achieved chunk-size distribution per level, not
  just the target, or the decision rule reads a number that no chunk has.
- **Twenty million vertices in a benchmark is 60 GB of temptation.** The fixture
  builder itself must be preflighted through D8, or the benchmark becomes the
  first thing this change's own gate would have refused.
- **`io::MemoryReport`'s "the fields sum" invariant** is asserted in code today.
  Adding categories without adding them to the sum is the exact defect the
  scene-model spec's second scenario exists to catch, and the test has to fail
  on a new unaccounted member rather than on review.

## Open Questions

- Whether the fixed sculptor's weld-class dirty list is retired in favour of the
  chunk dirty set or kept beside it. Keeping both is a second granularity by
  D1's own definition; retiring it touches `sculpt.cpp` more than the rebase
  risk above wants. Resolved by measurement in 1.1, since the answer depends on
  how many classes a chunk holds.
- Whether `trim` at critical pressure may release the ADAPTIVE surface's slot
  pool slack, which is rebuildable but whose reconstruction renumbers slots and
  therefore changes chunk membership. Reconstruction would be identical as a
  SURFACE and different as a partition, which the 4.6 wording does not settle.
