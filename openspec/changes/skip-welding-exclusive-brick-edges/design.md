# Design

An ordinary edge can belong to two closed brick boxes only if both endpoints lie on one shared boundary plane. Locally deduplicated edges that satisfy no such plane condition are exclusive to their brick. They can bypass the global table without changing first-encounter vertex numbering, canonical endpoint order or interpolation.

The proof requires distinct requested bricks, local recording deduplication and coordinates within the canonical 21-bit packing domain. Validate dimensions and all requested keys once, using 64-bit arithmetic for bound checks. Duplicate keys, dimensions outside 1–16, coordinates outside that domain, unoptimized reference recording and any straddlers retain the original global-welding path. Keep the eligibility check outside per-edge processing.

The Builder receives a private flag controlling only lookup/insertion; canonical endpoint ordering and position calculation remain shared code. Boundary edges always use the table. The table remains per mesh invocation, with no retained cache or shared mutable ownership state.

Regress eligibility boundaries and duplicates directly. Compare interior classification against independent neighboring-box containment over all dimensions 1–16 and translated bricks. Extend complete mesh/reference comparisons to repeated requested keys and dimension 1; retain existing attributes, subsets, empty requests, LOD, fallback dimensions and concurrent execution coverage. Record exact output, scratch memory and timing before claiming application gains.
