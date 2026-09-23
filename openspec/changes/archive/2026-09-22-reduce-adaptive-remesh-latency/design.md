## Context

Baseline `96fc007f` has local split/collapse/flip, bounded operation counts and
stable slot-order processing. The investigation records adaptive Draw stamp
p50 6.77 ms, sampled p95 21.36 ms and maximum 27.97 ms.

## Design

First separate gather, split, collapse, flip, relax and index-maintenance costs
with diagnostic instrumentation. Choose a local optimization from those results,
then record its mechanism here before implementation. Do not lower detail,
skip valid operations or change evaluation order to obtain a timing improvement.

## Validation

Compare alternating release baseline/candidate processes on identical inputs,
with no concurrent build or test jobs. Check per-stamp operation counts and exact
exported surface data. Exercise adaptive history, spatial query correctness,
constraints and topology validity; run sanitizer coverage for changed lifetimes.
Measure changed functions with the cognitive-complexity skill.

## Risks

Index bounds affect subsequent insertion choices. Scratch reuse must survive slot
recycling without retaining stale handles. Timing measurements on a shared host
need contention checks; a small sample cannot establish a universal 16 ms bound.

## Selected optimization

Collect normal updates during the remesh operation sequence and flush before
relaxation or returning. Standalone public topology operators keep immediate
normal refresh. The private batched operators preserve operation order and all
geometry-based refusal checks; none reads stored vertex/face normals when
selecting or applying topology changes. Collect generation-bearing face and
vertex handles at each successful operation, deduplicate at flush and skip dead
handles. Record and synchronize changed normals in the gesture delta.

The first quiet profile placed slow-stamp collapse work at 11–17 ms. Follow-up
instrumentation under contention (diagnostic only, not speedup evidence) placed
normal refresh ahead of planning, outgoing-handle repair and index maintenance.

The initial unconditional batch reduced tails but increased light-stamp median
with undo. Keep the first 64 successful normal updates immediate, then collect
subsequent updates. This bounds the setup cost on small edits without changing
which topology operations execute. Recheck the threshold on larger footprints;
its purpose is amortizing batch overhead, not changing detail or latency policy.
