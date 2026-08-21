# Tasks

## 1. Decide the shape

- [x] 1.1 DECIDED: `mesh/`. Confirmed against check_layering.py rather than assumed — voxel may include mesh and mesh may not include voxel, so `mesh/` is the only home the one existing caller can reach. ORIGINAL: DECIDE where it lives. `mesh/` is the obvious home, but the only
      caller today is `voxel/grid.cpp`, and voxel may include mesh while mesh
      may not include voxel — so `mesh/` works and the reverse would not.
      Confirm against `tools/check_layering.py` rather than assuming
- [x] 1.2 DECIDED: leave the target's own attribute alone. It composes (colour from A, uvs from B) and the report's per-channel flags are what keep 'nothing happened' visible. ORIGINAL: DECIDE the fallback when no source attribute exists: refuse the call,
      or leave the target's own attribute alone? Leaving it alone composes
      better (transfer colour from A, uvs from B) but makes "nothing happened"
      quiet. Whichever, the count reported has to make it visible
- [x] 1.3 DECIDED: relative with an absolute override — zero derives 5% of the source's bounding diagonal, any positive value is taken as given and reported back. ORIGINAL: DECIDE whether the threshold is absolute or relative to the
      source's bounds. Absolute is predictable; relative survives a host that
      works in millimetres. Probably relative with an absolute override

## 2. The operation

- [x] 2.1 `transfer_attributes(source, target*, options)` over the target's
      vertices, reading the source through one `Bvh` built once
- [x] 2.2 Colours and uvs by default; normals opt-in, and the option's
      documentation says why the default is off
- [x] 2.3 Positions never move — asserted, not merely intended
- [x] 2.4 A per-vertex distance threshold, with a documented fallback past it
- [x] 2.5 Report what happened: how many vertices were transferred, and how
      many took the fallback. A result that silently fell back for most of the
      mesh must be distinguishable from a good one
- [x] 2.6 Done as a shared BARYCENTRIC READ (`sample_color` / `sample_uv`) rather than by calling transfer_attributes: the voxel path queries per CELL against a BVH it already built, so calling the whole transfer would build one per cell. ORIGINAL: `mesh_colour_at` becomes a caller rather than
      a private duplicate — the point of generalising it

## 3. Pin the properties

- [x] 3.1 An identity transfer is exact — and the claim needed NARROWING, which the test did: exact for every vertex whose position is unique (0 of 169 differed), and inexact only where the source has coincident vertices with differing attributes (27 of 272 on a sphere with 16 coincident verts per pole), which is the seam limitation arriving early rather than an exactness failure: transferring a mesh's attributes onto
      ITSELF returns them bit-identically, which is the only case where an
      exact answer is available and therefore the only honest exactness test
- [x] 3.2 Positions and topology are byte-identical before and after
- [x] 3.3 A target vertex beyond the threshold takes the fallback, and the
      report counts it
- [x] 3.4 A source with no colours (or no uvs) does whatever 1.2 decided, and
      the test says which
- [x] 3.5 The round trip it exists for — and it caught the open-sheet trap for the THIRD time in this repo: the first fixture was a height field, and `to_field` takes its sign from a winding number, which is undefined on an open surface. A closed sphere was needed, and its winding had to be corrected too before the field had an inside at all, end to end: a coloured, UV'd mesh →
      `Volume.from_mesh` → mesh → transfer from the original. Colour comes back
      close; measure HOW close rather than asserting it does
- [x] 3.6 The uv seam case, as a test that DOCUMENTS the limitation rather than
      one that fails: a source with a seam, a target vertex on it, and an
      assertion about what actually happens — so the behaviour is pinned and
      the next reader does not think it is a bug
- [x] 3.7 Deterministic

## 4. Reach it

- [x] 4.1 C ABI
- [x] 4.2 pyclay, so `check_binding_parity` stays clean
- [x] 4.3 ABI 0.39.0 in all three places the release checklist names

## 5. Say what it does and does not do

- [x] 5.1 Spec delta on `meshing`
- [x] 5.2 `docs/08-mesh-readback.md` or `docs/07`, whichever owns "getting a
      mesh out and back", gains the round-trip recipe
- [x] 5.3 The README's composability row currently says a mesh conversion
      "drops the UVs and edge loops". After this it drops the edge loops and
      MOSTLY keeps the uvs, which is a different sentence
- [x] 5.4 `examples/58_attribute_transfer.py` — median colour error 0.0018 against the nearest source vertex, and 0 of 400 target vertices coinciding with a source one, which is the topology loss stated as a number. It also moved the vertex-colour rasteriser out of example 56 into `_render.render_mesh_array`: the ordinary preview traces a DOCUMENT, and a mesh reaches one through `Volume.from_mesh`, which carries a single colour per item — so a picture of per-vertex colour cannot come from it — the colour
      recovery and the topology that is still gone
