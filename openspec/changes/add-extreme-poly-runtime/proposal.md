# Proposal: the extreme-poly sculpt runtime

## Why

Three representations are about to become five, and the two new ones exist
precisely to hold more geometry than anything in this library has held before.
The engine's stated performance principle — *a dab should cost what it touches*
— is currently a property each subsystem maintains for itself, and at ten
million vertices "for itself" is not enough: a single O(model) step anywhere in
the chain from pointer event to pixel makes the whole thing unusable, and it
does not matter which subsystem owns it.

The device this is for makes it sharper. `io::MemoryReport` exists because iOS
asks what a document costs and does not ask twice; the breakdown is the feature
because under pressure a host needs to know which part it may release. That
report was built for a document of edit lists, grids and masks. A multires
hierarchy with layers and chunked spatial indices adds a large new category
whose defining property is that MOST of it is rebuildable — and the report
cannot say so, so a host has to treat all of it as precious.

The second device-shaped problem is peak, not steady state. Adding a
subdivision level multiplies geometry by four; the allocation that fails is the
one that briefly holds the old and the new together. An engine that discovers
this by being killed has no way to tell the user what happened.

## What changes

- **A shared surface chunk** — one unit that is simultaneously the spatial
  index leaf, the brush candidate set, the parallel work unit, the normal
  recompute unit, the dirty-tracking unit and the host's upload unit, rather
  than a different granularity invented per subsystem.
- **Revisioned dirty-chunk transport across the C ABI**, with caller-owned
  buffers, so a stroke's cost to a host follows what changed.
- **A memory profile** — constrained, mobile, desktop — that the HOST fills
  with byte budgets, with no device detection in the portable core.
- **Pressure trim** with a stated eviction order, and the guarantee that
  authoritative content is never in it.
- **Preflight** on every operation whose peak exceeds its result.
- **Scaling and allocation gates** that make the performance principle
  testable rather than aspirational, at 1M to 20M vertices.

## Approach

Optimise a correct architecture; do not compensate for a missing one. This is
last of the five deliberately — the documents that proposed it say the same
thing, and the repository has the receipts: a spatial index for the edit list
was built, measured and REVERTED because the build-to-query ratio was 1:1, and
the finding was only available because the architecture it indexed was already
settled.

Eviction order is the load-bearing part and it is a policy, not a heuristic:
transient scratch, then preview buffers, then evaluated caches, then inactive
spatial indices, then inactive derived positions, then other rebuildable
caches, then history to the host's policy — and never unsaved authoritative
geometry, detail or layers. Written into the spec and asserted by a checksum
test, because "we would never drop the user's work" is exactly the kind of
promise that decays into a bug.

GPU is out of scope here beyond the transport. Authoritative editing stays on
the CPU: undo, serialization, exact topology and crash recovery all need it,
and a GPU-authoritative surface trades all four for a frame.

## Open questions

- **Chunk size.** Somewhere between 64 and 1024 triangles by every prior art;
  the number comes from a benchmark matrix on this library's own workloads and
  not from a citation.
- **Whether the memory profile is a new module.** `include/clay/memory/` has no
  entry in the layering table; `parallel` is the precedent for adding one.
- **How much residency policy belongs in the engine.** A host that must ask
  before every level switch is a bad host API; an engine that evicts on its own
  is a document mutating behind a host that may be mid-save.
- **Whether the interactive budget is a hint or a contract.** Deferring exact
  normals during a drag is safe; deferring a topology decision changes the
  committed result and is not.

## Impact

A new `sculpt-runtime` capability. `scene-model` gains the memory categories;
`c-abi` gains the transport, the profile and the trim; `build-packaging` gains
whatever module the profile lands in. No behaviour change to anything that
exists: a host that sets no profile gets the current behaviour.
