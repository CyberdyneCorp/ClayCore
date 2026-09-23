## Context

The mesh pipeline records edges in parallel, then replays them through one global Builder. Repeated edge calls within one brick do not create vertices but still allocate records and perform global hash lookups. Temporary experiments rejected a recent-entry cache and a replacement global hash table: both slowed welding. A dense brick-local recorder reduced the observed welding median from 18.124 to 12.886 ms in an adjacent control pair; this is preliminary evidence under host contention.

## Decisions

Keep ShellCollector and Builder as the reference/general path. Wrap only the ordinary per-brick march in a local collector. Freudenthal tetrahedra use seven monotone unit-edge directions. Index these by the lower endpoint within the brick's (dim+1)^3 lattice and direction. Store record index plus one so zero means unseen. On the first encounter, append the original endpoint/value record; on repetition, return its existing index.

The global Builder sees the same first occurrence of every edge in the same order. Triangle order and indices must remain byte-identical. Boundary straddlers continue through the existing collector after ordinary cells, preserving ownership and range semantics. The same sampled field value applies to each occurrence of a lattice edge during a const cache march.

Bound scratch by enabling the lookup only for supported small dimensions (at most 16); larger dimensions retain general recording. Coordinates or directions outside the dense representation also fall back. Scratch is per active worker invocation/brick, not retained for an entire wave. Keep the implementation simple before attempting reuse or additional optimizations.

## Risks and validation

An incorrect direction, endpoint canonicalization or record index could corrupt topology without changing counts. Regressions must compare positions, indices, normals, colors and brick ranges to the unoptimized recording reference, including negative coordinates, subsets/straddlers, hard surfaces, LOD, empty input and multiple thread counts. Test bounded fallback dimensions too. Run mesh correctness suites, sanitizers and relevant cross-platform gates.

Benchmark the actual uninstrumented implementation against the current branch baseline, with alternating runs and matching workloads. Reject or revise it if gains do not survive controls or small-mesh regressions outweigh them. Full application latency remains a separate acceptance check.
