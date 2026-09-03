# Design

## 1. What is actually broken, and where

`MeshSculptor::surface_index()` returns null unless something else built a ray
tree — deliberately, and the measurement behind the deliberation stands: a build
is 689 ms against 1.24 ms saved per stamp. A host that picks gets the tree for
free from `raycast`. `MultiresSculptor` never picks, so a level sculptor never
has one, and every unseeded stamp takes the scan inside `geodesic_region`
(`src/mesh/adjacency.cpp:220-230`) — O(classes), measured at ~1.2 ns a vertex.

Three other sites scan on the same path and are in scope for the same reason:
`nearest_class`'s fallback (`src/mesh/sculpt.cpp:331`), the alpha frame's lazy
direction fallback, and the connectivity automask's anchor.

## 2. THE DESIGN RISK: an anchor is not a hint, it is part of the answer

The source guide says, in `Step C1`:

```
seed = caller_seed;
if (!valid(seed)) seed = carried_previous_stamp_seed;
```

**Taken literally this silently changes every sculpt result in the library**, and
it took reading `geodesic_region` to see why.

The walk is a Dijkstra whose distances accumulate *from the seed*:

```cpp
const float seed_d = kernel::clength(position_of(seed) - seed_position);
if (seed_d > radius) return;              // ← an anchor too far LOSES THE DAB
scratch.distance[seed] = seed_d;
...
const float d = top.first + kernel::clength(q - p);   // ← accumulates from it
```

`out_distance` is the brush's falloff input. A different anchor is therefore a
different weight per vertex and a different final position — not a different
route to the same answer. Today, with no index, the anchor is the class the scan
finds: the globally nearest. Carrying the previous stamp's anchor forward would
substitute a *different* class and move the surface.

`tests/unit/test_mesh_sculpt_parity.cpp` is what makes this concrete rather than
theoretical. It runs 5 fixtures × 16 verbs, **three stamps along a path**, with
`MeshSculptor sculptor(m)` and no ray tree — so all 80 cases are on exactly the
path this change touches. The moved counts are gated on every toolchain,
baselined or not.

### The decision

**The carried anchor accelerates the SEARCH for the class the scan would have
found. It is never used as the anchor directly.**

From the carried class, descend the adjacency to the local minimum of distance
to the new centre:

```
c = carried
loop:
    best = argmin over c and ring(c) of |position(x) - centre|
    if best == c: stop
    c = best
```

Each step is O(valence) and a stroke's dabs are a fraction of a radius apart, so
this terminates in a few steps. Where the surface under the ball is connected —
which is every fixture but one — the local minimum IS the global minimum and the
result is bit-identical.

### Where it is not, and why that is the right answer anyway

`two_close_sheets` is the counter-case and it is already a golden fixture: two
sheets a thirty-second apart, "the two lips of a closed mouth". A ball spanning
the gap reaches both; a descent constrained to the adjacency reaches one. If the
centre were nearer the far sheet, the descent would return the near-sheet class
and the global scan the far one.

That difference is the Move Topological rule, which the library already commits
to: a brush on the upper lip must not drag the chin through the closed mouth.
The descent's answer is the connected one. **But this change does not get to
decide that by argument** — the `sheets` goldens are in the table and either they
move or they do not. If they move, the descent is wrong for this change and the
fallback is §2.1.

### 2.1 MEASURED, and the descent is REJECTED

The descent was implemented and run against the table. **7 of the 80 cases move**,
and a probe comparing the descent's anchor against the scan's answer at every
resolution root-caused all seven — 7 divergences in ~160 descents:

| Fixture | Verbs | What moved | Cause |
|---|---|---|---|
| `plane` | grab, snakehook | hash only, moved count identical (31) | a local minimum on a connected surface: descent d=0.1786 against scan d=0.1707 |
| `cube` | grab, snakehook, nudge | moved 40 -> 37, 40 -> 39 | the same, at an edge: d=0.1698 against d=0.1398 |
| `sheets` | crease, nudge | hash only, counts identical (23, 24) | **genuine disconnection**: descent lands on class 24 (upper sheet), the scan on class 73 (lower). `two_close_sheets` is 7x7 per sheet, so >= 49 is the far sheet |

Two conclusions, and they point opposite ways:

- The `plane` and `cube` divergences are local minima on a CONNECTED surface, and
  a fixpoint — re-anchor at the closest class the walk actually reached, re-walk,
  repeat — would converge to the scan's answer, because the walk explores the
  whole connected ball rather than one downhill path. Each step strictly
  decreases the anchor's distance, so it terminates.
- The `sheets` divergence is **not reachable by any surface-local method**. The
  nearest class is across a gap the adjacency does not cross. No walk, fixpoint
  or descent can return it.

So the requirement as written — "identical, rather than close" — is **not
satisfiable by a carried anchor**, and re-baselining is not available either: the
moved counts hold on every toolchain, but the two `sheets` hashes would need new
tables on macOS and MSVC, which cannot be regenerated from this box.

**The carried anchor is rejected.** Not narrowed, not put behind a flag — a flag
would move the hierarchy's results silently, which is the same defect where
nobody is looking.

### 2.2 What the requirement actually asked for

Re-reading task 3.1 after the measurement: the query path it specifies is

> brush volume -> **top-level tree -> candidate chunks** -> candidate vertices ->
> exact footprint

That is not a carried seed. It is a spatial descent over the CHUNK TABLE, which
already exists per multires level (`src/mesh/multires_chunks.cpp` builds one with
per-chunk bounds and vertex lists). The carried seed was proposed in the task's
later notes as the cheaper alternative to a ray tree per level; the requirement
itself names the chunk path, and the chunk path is EXACT — it returns the class
the scan returns, so no golden moves and the euclidean `classes_in_ball` case is
fixed too, which a carried anchor could never reach.

`bench::ChunkTree` (`benchmarks/chunk_tree.h`, 111 lines) is already that tree,
built over a `ChunkTable`'s bounds by median split, and its own header says why
it exists: both benchmarks first wrote the query as a walk over every chunk, and
"a real query descends a tree". It lives in `benchmarks/` because no runtime
caller had one.

**The design is therefore: promote the chunk tree into the library, have
`MeshSculptor` borrow a `ChunkTable`, and answer the ball and nearest-class
queries from it.** It subsumes §5 (the sculptor needs the table in hand to mark
it), and a sculptor with no table keeps today's scan exactly — which is what
leaves the 80 goldens untouched, since they construct a bare `MeshSculptor`.

The one new obligation: chunk bounds are set at partition time and do not follow
vertices, so a stroke must refit the bounds of the chunks it wrote, on the same
discipline as `refit_bvh`.

## 3. Validity, and the three ways an anchor dies

An anchor is carried in the sculptor, not in the settings, because it is state
about a stroke and not a parameter of a dab.

| Retired by | Mechanism |
|---|---|
| a new class space | `seed_revision_` is per-sculptor and `MultiresSculptor::bind` builds a new one on a level or generation change. Nothing extra needed |
| a centre out of reach | the descent's result is range-checked against `radius` exactly as `geodesic_region` would, so an anchor too far falls back to the scan rather than returning empty — this is the defect 3.2 found, and it must not be reintroduced here |
| a new stroke | reset explicitly. A stroke that starts elsewhere on the model has an anchor whose descent would begin in the wrong basin |

Precedence, highest first: the caller's `seed_class` (unchanged), the ray tree
when one exists (unchanged, so the fixed-mesh-with-pick path is untouched), the
carried anchor, then the scan.

## 4. Stage telemetry

One enum in `sculpt_common.h`, shared by the three sculptors so a stage name
means the same thing on all of them, published through a borrowed pointer that
is null by default. **No clock is read when it is null** — the benchmark's own
warning is that instrumenting a stamp perturbs what it measures.

The eight named in 7.2 are gather, geodesic, snapshot, weight, alpha, automask,
kernel and normals. They resolve the `stamp*` bucket the benchmark prints today.

## 5. Chunk marking

`MeshSculptor` borrows a `ChunkTable` rather than owning one: the adaptive
surface's table is owned by `DynamicBvh` and a multires level's by the hierarchy,
so an owned table here would be the second ownership rule for one concept. Null
is the default and costs a null check per stamp.

After a write: geometry over the write region, normals when they were refreshed,
attributes when the verb wrote colour. **Never topology** — the fixed-topology
contract is that `indices` and `quads` come back byte-identical, so a fixed
sculptor that marked topology dirty would be telling a host to re-upload an index
buffer that cannot have changed.
