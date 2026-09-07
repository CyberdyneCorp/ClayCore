## Why

**Creating a `MeshSculptor` over a mesh layer is 120 ms, and the second one over
the same unchanged triangles pays it again in full.**

Measured on this machine, on the ~296k-triangle fixture the roadmap already
records the construction split for — a 384x384 plane, 148,225 vertices,
294,912 triangles:

| | ms |
|---|---:|
| first `MeshSculptor(mesh)` | 120.8 |
| **second `MeshSculptor(mesh)`, nothing changed** | **123.6** |
| `Adjacency::build`, default weld epsilon | 120.2 |
| `Adjacency::build`, exact welding | 81.4 |
| copying a built `Adjacency` | 0.20 – 0.78 |
| `Bvh::build` (lazy; not paid at construction) | 108.5 |

So construction IS the adjacency build — the sculptor's other members are empty
vectors — and nothing shares it. Every host that opens two sessions on one
layer, or destroys and recreates a sculptor between strokes, or rebinds a
hierarchy level, pays 120 ms of weld-and-CSR again for a partition that did not
change.

`MultiresSurface` already caches a level's `Adjacency` in its `LevelCache`,
which is the same idea reached once and not shared: `MultiresSculptor::bind`
then **copies** it into the `MeshSculptor` (0.2–0.78 ms per rebind), because
`MeshSculptor(Mesh&, Adjacency)` takes it by value.

## What Changes

**`MeshSculptor` stops owning its adjacency and starts sharing it.**

- `std::shared_ptr<const Adjacency>` replaces the by-value member. Both existing
  constructors are kept and mean exactly what they meant; a third takes a
  shared pointer. All 40 internal uses are unchanged — a `const Adjacency&`
  member bound to the pointee keeps the call sites identical.
- `MultiresSurface::level_adjacency_shared` hands the level's adjacency over
  instead of lending a reference to be copied.

**A document-owned `mesh::TopologyCache` is what makes two sculptors share
one.** Keyed by the layer's stable identity, not by counts.

**The entry is VALIDATED, not trusted, and that is the load-bearing decision.**
The obvious key is (identity, topology revision), and it is one bug away from
serving a stale adjacency over replaced triangles — which is precisely the open
defect #472 reports about that revision. So an entry carries a
`TopologyFingerprint` — vertex count, triangle count, weld epsilon and a hash of
the index buffer — and a lookup that does not match it is a miss:

- the cache is correct even where a replacement path forgets to invalidate it,
  so it does not inherit the correctness of the counter it is keyed on;
- "identical vertex and index counts, different connectivity" is a miss
  structurally rather than by revision hygiene;
- it costs **0.25 ms** against the 120 ms it is deciding whether to skip.

Both mechanisms are kept and both mean something: `forget` on wholesale
replacement is the fast, intentional invalidation, and the fingerprint is the
net under it.

**What the cache deliberately does NOT invalidate on: positions moving.** An
`Adjacency` pins its weld partition at build time and the header has always said
positions may move under it freely — that is the fixed-topology contract, and a
live sculptor already behaves this way through a whole stroke. Serving a cached
entry after a sculpt therefore gives a NEW sculptor exactly what a live one
would have had. It is a real semantic and it is stated rather than assumed: a
vertex dragged out of a coincidence it was welded into stays in its class, where
a rebuild would have separated it. A wholesale geometry replacement — the path
that changes what the artist means by "this mesh" — calls `forget` and rebuilds.

**Reachable, measurable and trimmable from the host**, because a cache a host
cannot see is a leak with a good reason:

- `clay_document_topology_cache_stats` — entries, bytes, hits, misses,
  evictions, and cumulative build and verify time.
- `clay_document_trim_topology_cache` — releases every entry no live sculptor
  is holding, and reports the bytes. Entries are `shared_ptr`, so "no live
  sculptor" is a fact rather than a heuristic.
- The bytes join `clay_memory_report` under a new `topology_cache` line and its
  `rebuildable` roll-up, appended the way the surface tier was.

ABI 0.88.0 -> 0.89.0.

## What it measures at

Same fixture, same machine, after the change:

| | ms |
|---|---:|
| first create (cache miss) | 121.3 |
| **second create (cache hit)** | **0.25** |
| create after a position-only change | 0.25 |
| create after two indices are swapped — same counts | 121.3 |
| the fingerprint itself | 0.25 |

**480x on the second create**, and the two invalidation rows are the ones that
say the number means something: a stroke keeps the entry, and a mesh that is a
different surface at identical counts does not get it.

The cache holds **10,048,720 bytes** for that one layer, which is why it is
reported and trimmable rather than merely fast.

**THE GATE IS THE COUNT, NOT THE CLOCK.** `tests/unit/test_topology_cache.cpp`
asserts pointer identity and the hit/miss counters; the milliseconds above are
evidence and are recorded here rather than asserted anywhere, because a
duration is a claim about this machine.

## What measuring refuted

Recorded because the plan was wrong about two things and the numbers said so.

1. **"Key it on the topology revision, as the roadmap proposes."** That makes
   the cache's correctness a function of #472's, on an open defect, in another
   branch. The fingerprint costs 0.25 ms and removes the coupling entirely — at
   which point the revision is not needed for correctness at all and is kept
   only as the cheap, intentional invalidation and as a diagnostic.
2. **"Adjacency is topology-derived, so it is safe under a geometry revision."**
   It is not purely topology-derived: `Adjacency::build` welds by POSITION
   within an epsilon, so the partition is a function of the positions it was
   built over. The cache is correct anyway — for the reason above, which is
   about matching a live sculptor rather than about the input being immutable —
   but a cache written on the roadmap's stated dependency classification would
   have been documented as safe for a reason that is false.

## Capabilities

### Modified Capabilities
- `sculpt-runtime`: topology-derived preparation is shared between sculptors
  over one mesh, is validated rather than trusted, and is visible and trimmable
  through the memory model.
- `c-abi`: a host can read the topology cache's occupancy and effect, and
  release what nothing is holding.

## Impact

- `include/clay/mesh/adjacency.h`, `src/mesh/adjacency.cpp` — `bytes()`.
- `include/clay/mesh/topology_cache.h`, `src/mesh/topology_cache.cpp` — new.
- `include/clay/mesh/sculpt.h`, `src/mesh/sculpt.cpp` — the shared member.
- `include/clay/mesh/multires.h`, `src/mesh/multires_eval.cpp`,
  `src/mesh/multires_sculpt.cpp` — the level adjacency is shared, not copied.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp` — the two entry points, the
  report line, the document's cache and the `forget` on replacement.
- `bindings/python/pyclay_module.cpp` — the document's cache, the sculptor's
  acquire, `topology_cache_stats` and `trim_topology_cache`. The Swift surface
  consumes `clay.h` directly and needs no change.
- `docs/05-claycore-library.md` — the host-facing account, beside the arena.
- `tests/unit/test_topology_cache.cpp`, `tests/unit/test_c_mesh_sculpt.cpp`,
  `bindings/python/tests/test_pyclay.py` — the gates.
- `CMakeLists.txt`, `tests/CMakeLists.txt`, `pyproject.toml` — the new source,
  the new test file, the version lines.
- **No format change: nothing here is serialized.** `report_surface_memory` is
  deliberately untouched — a per-sculptor line would double-count the document's
  own, which is now where these bytes are reported.
