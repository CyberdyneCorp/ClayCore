# Proposal: the dual mesher samples twelve corners a cell where it needs eight

## Why

The meshing spec promises surface nets as a **cheap preview mesher**, gated on
it beating marching cubes at equal resolution. #302 batched the attribute pass
that both meshers were paying, and with that confound gone the promise turned
out to be false: nets was measured **1.68x slower to build** on the same
fixture (#304).

The gate had been passing for a reason unrelated to the mesher. Both benchmarks
meshed with the default attributes, the attribute pass was 80-96% of each, and
nets emits 3.2x fewer vertices — so it paid 3.2x less of a cost that was not
the geometry. The pair was a vertex-count comparison wearing a stopwatch, and
#302 removed it rather than weaken it.

#304 asked whether the spec claim should change or the mesher should get
faster. Profiling says the mesher, and by a wide margin. On a 179x168x138
lattice of the benchmark document:

| `dual_grid_mesh`, where its 113 ms went | |
|---|---:|
| vertex pass, 8 `sample` calls a cell | 52.6 ms |
| **quad pass, 4 more `sample` calls a cell** | **27.4 ms** |
| the same reads with the sampler inlined | 6.7 ms |
| the cell hash map, at the real 73,638 surface cells | ~3.7 ms |

Two things, neither of them the algorithm. `sample` is a `std::function`, so
none of its 33 million calls inlines. And the mesher walks the whole cell range
TWICE, the second time re-reading four corners it already had.

## What

**One walk instead of two.** A cell's vertex is placed and then the quads on
the edges it owns are emitted immediately. That is sound because a quad only
ever references cells reached by stepping BACK along the two axes that are not
the edge's — the owning cell and three already placed — so the walk never needs
a vertex it has not made yet.

It also drops the second pass's sampling entirely: the four corners it read are
corners 0, 1, 2 and 4 of the eight the vertex pass already has, and it was
reading them for every cell in the range including the ones that own no vertex
and can contribute no quad.

**Templated on the sampler**, so the tape-level meshers' lambda inlines. The
public `mesh_lattice_nets` / `mesh_lattice_dc` keep their `std::function`
parameter and instantiate on it, which is no worse than what they paid.

Both changes are shared by every dual mesher: surface nets, dual contouring and
the quad mesher all go through `dual_grid_mesh`.

## Impact

Byte-identical output, verified across all five entry points — `mesh_tape_nets`,
`mesh_tape_dc`, `mesh_tape_quads`, `mesh_lattice_nets`, `mesh_lattice_dc` —
by hashing positions, indices, quads and normals before and after.

| the meshers on one lattice, no field evaluation | before | after | |
|---|---:|---:|---|
| marching (`mesh_lattice`) | 107.0 ms | 108.5 ms | unchanged |
| surface nets (`mesh_lattice_nets`) | 113.1 ms | **75.4 ms** | 1.50x |
| nets / marching | 1.06x | **0.69x** | the claim, restored |

| the tape meshers, geometry only | before | after | |
|---|---:|---:|---|
| `mesh_tape` (marching, pooled) | 82.5 ms | 77.5 ms | |
| `mesh_tape_nets` (nets, serial) | 138.3 ms | **72.3 ms** | 1.91x |

Nets is now faster end to end than marching even though marching gets the
thread pool and the dual walk does not.

## The gate comes back, on a different pair

`("BM_MeshLatticeNets", "BM_MeshLatticeMarch")` — the two meshers over ONE
precomputed lattice, 86.8 ms against 120.1 ms.

Not the old `("BM_SurfaceNets", "BM_MeshTape")`. Both of its sides spend about
half their time in `eval_tape_grid` evaluating the same field, and that shared
half compresses whatever the meshers differ by: the same result reads 1.18x
there against 1.38x on the lattice. It would also fail for the wrong reason if
marching's grid evaluation or its thread pool ever improved, which is not a
regression in nets.

## Non-goals

**Parallelising the dual walk.** Marching records per-plane and replays serially
to stay byte-identical (`mesh_lattice_parallel`); the dual walk could do the
same and would then beat marching by more than the 1.38x here. It is not needed
to restore the claim, and it is a bigger change with its own ordering argument
to make.

**The same treatment for marching.** `march_cells` samples eight corners a cell
through the same `std::function` and would take the same templating. It is
already pooled, which is why it did not show up as the problem, but the 52.6 ms
of indirect calls above is a cost it pays too.
