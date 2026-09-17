## Design

Each parallel boundary-cell range owns a lazy lattice cache. Before marching a
cell, select its owner brick using floor division, including negative indices.
Changing the owner clears only a validity bitmap; values are written on first
access. Points on the high brick face remain in the owner's closed lattice even
though the underlying sampler obtains them from a neighboring brick.

For dimensions 1–16, scratch is bounded by 17³ floats plus a validity bitmap.
No sampled value survives the recording range or crosses a caller thread. Every
miss calls the existing sampler with the original integer coordinates. Cache
hits reuse the same immutable brick-cache sample. General dimensions and the
reference recorder retain the uncached path.

Keep the existing list and order of shell cells, parallel write ownership,
serial attribution, straddler rules and welding. Test sample counts, bit patterns,
negative owners, boundary points, owner changes, LOD, nested execution and exact
mesh/range equivalence to the uncached recorder. Retain only with paired engine
and application latency evidence. Do not attribute lower source allocations to
this change or consider #531 closed while actions remain above 16 ms.
