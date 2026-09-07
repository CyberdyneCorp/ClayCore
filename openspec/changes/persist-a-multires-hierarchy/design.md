## Context

Three facts in the tree shape this, and two of them mean the design is largely
already made:

- **`mesh::MultiresSurface::encode()` / `decode()` exist and are versioned.**
  `add-mesh-sculpt-layers` moved them to `kSurfaceVersion` 2, and a version-1
  stream still decodes as a hierarchy with no sculpt layers
  (`include/clay/mesh/multires.h:701-715`). The expensive half of serialization
  is done and this change does not open it.
- **`tools/check_layering.py` withholds `clay/mesh` from `clay::scene`.**
  `include/clay/scene/document.h:267` records why voxel and mesh content live
  beside the document rather than inside it: the withholding is what makes "this
  content does not change what the document evaluates to" structural rather than
  a rule someone has to keep. A hierarchy is `mesh::` and inherits that verbatim.
- **A document-held payload handed out as a borrowed handle is an established
  convention, not a new idea.** `bindings/c/clay.h:4823` states it for masks and
  says it "mirrors `clay_voxel_grid` exactly": create returns a handle the caller
  destroys, the document accessors return one BORROWED, destroy rejects a
  borrowed handle, and a borrowed handle onto a removed payload fails
  `CLAY_ERROR_NOT_FOUND` rather than dangling. This change is the third instance.

The pressure is a host's, not the engine's. ClaySpaceDesktop's rank-2 ask says a
hierarchy row is two objects on their side and the engine calls the layer a MESH
layer, so their side-car is the only record that the row was ever a hierarchy.

## Goals / Non-Goals

**Goals:**

- A `.clayspace` carries a hierarchy, and a reload produces the same surface.
- The engine can answer "does this layer carry a hierarchy" without a side-car.
- The cost to a document with no hierarchy is nothing — no chunk, no bytes.
- A reader predating the chunk still opens the document.

**Non-Goals:**

- A hierarchy reaching the evaluated field. It does not today and must not here.
- A `LayerRepresentation` enum. The host's ask names one; no such type exists in
  this tree, and a chunk keyed by layer id answers the question without it.
  Inventing an enum to describe payload presence would give two sources of truth
  for the same fact.
- Changing the surface encoding. If implementation shows `encode()` is
  insufficient, that is a finding to record, not a format to widen quietly.
- Region-scoped levels, transition polygons, cross-level neighbours — all named
  in other rows and none of them reachable from this one.

## Decisions

**D1. The home is `io::ClaySpaceDoc::multires_layers`, and it is forced.**
`std::map<scene::LayerId, mesh::MultiresSurface>` beside `mesh_layers`,
`voxel_layers` and `masks`. This is not a preference: `scene::Layer` cannot hold
a `mesh::` type without the layering table rejecting the include, and the table
is the mechanism that keeps authored geometry out of the evaluated field. The
alternative — a `scene`-side handle or id pointing at mesh storage — reintroduces
by indirection exactly what the table forbids directly.

**D2. One chunk per hierarchy, gated on the container minor 17 → 18.** Older
readers skip it. `add-mesh-multires`' own design.md anticipated this in as many
words: "the document format gains a chunk older readers skip."

**D3. The container does not version the surface.** The chunk carries the layer
id and the bytes `encode()` produced, and `decode()` reads them. A second version
negotiated at the container level would be a second place to answer the same
question, and the two would eventually disagree. The container's minor gates
whether the chunk EXISTS; the surface's own version gates what is IN it.

**D4. The orphan policy is `mesh_layers`', verbatim.** An entry survives its
layer's removal, because the inverse of a layer removal restores a `Layer` by
value and cannot carry a payload — so erasing on removal would break undo within
a session. The writer emits a chunk only for an id that is still a mesh layer;
the reader drops a chunk naming none. This is copied rather than re-derived
because the reasoning is identical and `document.h:203-207` already carries it.

**D5. The handle is borrowed, and `clay_multires_destroy` rejects it.** Exactly
as `clay_mask_destroy` rejects a borrowed mask. A host that loads a document and
takes a hierarchy handle must not free it; a host that built one with the
existing standalone create still owns that one. The asymmetry is real and is the
reason the rule is stated in the header beside both calls rather than only in one
place — it is the shape a host gets wrong once.

**D6. "Not a mesh layer" and "a mesh layer with no hierarchy" are different
answers.** A host walking every layer to build its outliner asks this about rows
that legitimately have no hierarchy, so the empty answer is ordinary and must not
look like a malformed call. Asking for a HANDLE where there is none is
`CLAY_ERROR_NOT_FOUND`, not `INVALID_ARGUMENT` — the two mean opposite things to
a host, and getting that wrong once already cost this repo a frame with nothing
on screen saying so.

**D7. Attaching to a non-mesh layer is refused, and replacing is explicit.**
Replacing silently would drop sculpted detail that the base cage cannot
reconstruct. `clay_document_add_mask` replaces any mask the layer already had and
says so; a hierarchy is not a mask, because a mask can be repainted and levels
cannot be re-sculpted.

**D8. `io::document_memory` counts hierarchies.** `MultiresSurface::memory()`
exists and nothing on the document side calls it. A hierarchy is routinely the
largest payload a document carries, so a memory report that omits it answers a
question nobody asked — and the budget work in `add-history-budget` already found
one accounting that measured low by walking a stale member list.

**D9. The downgrade is REFUSED, not degraded — reversing what this design first
said.** Written before #477 landed, this file said the new minor would be
"writable at the previous one" with the release notes stating what was lost. That
followed the format's older rule, and #477 replaced it: `scene/commands.h:370`
now reads *"writable at the previous minor" is read as "when the previous minor
can SAY it", not "by discarding what it cannot"*, and `serialize_document`
refuses rather than turning a subtractive layer into a union.

The discriminator that rule uses is whether the degradation is *plainer* or a
*different sculpture*. Dropping a hierarchy is neither, quite: the row comes back
as its honest base cage rather than as something misrepresented. But what it
discards is **authored** — hours of sculpted levels — which no earlier downgrade
did (14's squash, 15's instances, 17's deduplication all cost bytes or shape a
host can rebuild). A file that opens cleanly, looks deliberate and is missing a
day's work is the hazard #477 named, arriving by a different door.

So: writing at minor 18 is refused for a document whose hierarchy carries detail
above its base, and allowed for one whose hierarchies are bare cages, where
nothing authored is lost. A query names the first layer that blocked it, mirroring
`scene::layer_blocking_minor` — a refusal a caller cannot name is one it has to
explain by guessing.

**D10. The cage and the hierarchy's base are not reconciled, and a host can ask
whether they agree.** Task 0.1 settled this against the tree rather than by
preference: `clay_multires_from_mesh` takes a `clay_mesh*`, not a layer, so a
hierarchy already holds its own copy of the cage with no link back, and the two
can already diverge. Persisting both does not create that hazard — it makes it
observable, which is strictly better than a side-car that also could not detect
it.

The query is computed on demand from two objects already in memory and is **not
stored**. `snapshot_identity` is the right shape and the wrong storage: it
documents itself as stable "neither across builds that change the document
encoding nor across byte orders", so a hash written into the file would report a
false divergence the first time an encoding moved — a worse failure than the one
it was added to catch.

## Risks / Trade-offs

**The base cage exists twice, and the two copies can disagree.** Settled by D10
rather than left open — kept here because the risk does not disappear, it becomes
detectable. The mesh layer
holds the cage as `mesh::Mesh`; the hierarchy holds its own base level. Nothing
today forces them equal, and after this change a save writes both. If a host
sculpts the mesh layer directly while a hierarchy is attached, a reload restores
two states that never coexisted. **This is the one question implementation must
answer with a measurement rather than a preference** — whether to refuse the
direct edit, to re-project, or to declare the hierarchy authoritative and the
layer's triangles a cache. It is named here because discovering it during the
chunk work would look like a serialization bug and is not one.

**A hierarchy is large, and saves are already synchronous.** `survive-a-crash`
records that `clay_document_save` is whole-document and synchronous, and that a
large sculpt costs the same class of time as `sdf_consolidate` at 661 ms. Adding
the largest payload in the document to that path makes an autosave stall worse
exactly for the users who most need one. This change does not fix that and must
not pretend to; the journal work is where it belongs. Worth measuring the added
save cost and recording it rather than leaving a host to discover it.

**Gallery churn.** A minor bump regenerates the gallery documents and
`tools/check_gallery.py` compares them. That is routine, but it collides with any
other in-flight branch touching the same files — which is why implementation is
sequenced after #477 rather than beside it.

**The downgrade is lossy and silent unless the notes say so.** Writing at minor
17 drops the levels, the detail field and the sculpt-layer stack. The format rule
requires a new minor to be writable at the previous one; it does not by itself
make the loss legible. The release note is the mitigation and it is a task, not a
nicety.
