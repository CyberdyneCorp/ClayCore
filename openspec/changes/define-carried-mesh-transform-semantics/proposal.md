## Why

**A host that raycasts a mesh layer and then stamps where the ray hit sculpts
nothing, silently, and the ABI reports success.**

Measured through the C ABI, on a mesh layer translated by 3 and scaled by 2:

```text
world ray from (3.40, 2.00, 0.20) hit at (3.4000, 0.0000, 0.2000)
stamp at the world hit: result=0 (CLAY_OK) moved=0 vertices
it moved nothing
the local point under that world hit is (0.2000, _, 0.1000)
```

`clay_mesh_sculptor_raycast` takes a **world** ray and a `clay_mesh_frame` and
returns a **world** hit — `pick.h` says the conversion is done there on purpose,
"because a caller doing it by hand gets a brush whose radius changes when a
layer is scaled, and gets it wrong silently". The very next call a host makes,
`clay_mesh_sculptor_stamp`, reads its centre and radius as **local**, takes no
frame, and says nothing about it.

`moved == 0` is documented to mean "reached nothing, fully masked, or no
displacement". It now also means "you were in the wrong space", and those are
indistinguishable.

**The roadmap describes this as "the stored triangle arrays are not
transformed". That is true and it is not the defect.** Layer-local arrays are
the RIGHT contract: baking a transform into vertices on every layer move is
expensive, lossy and hostile to history. The defect is that the calls crossing
the boundary do not agree, and the header does not say which is which. The full
audit is in `design.md`.

**A second defect, deeper.** `clay_mesh_frame` carries a UNIFORM scale, because
`math::Transform` is a similarity. A layer also carries `scale_axes`, and
`scene::layer_matrix` composes both. So a host whose mesh layer has a
non-uniform scale — which `clay_document_set_layer_transform_nonuniform` exists
to give it — **cannot express its own layer's frame to the sculptor at all**.
Bounds and export honour that scale; nothing the sculptor takes can.

## What Changes

**The contract is stated, unchanged, and then honoured everywhere.**

```text
mesh vertex arrays = layer-local
layer.xform * diag(scale_axes) = local -> document/world
```

1. **Every crossing says its space, in `clay.h`, beside itself.** The audit
   table lands as header text on the calls it describes, not only in an
   OpenSpec document a host integrator will not read.

2. **A session declares the space it speaks.**
   `clay_mesh_sculptor_set_world_frame` takes a frame; once set, every
   position, radius and direction crossing that handle is world and every
   readback is world. **Absent is identity, which is exactly today's
   behaviour** — the rule `clay_sculpt_policy`'s three knobs already follow, so
   a host that has not heard of this is not opted into it.

3. **`clay_mesh_sculptor_use_layer_transform`** adopts the layer's own frame,
   which is what a host actually wants and the only form that can carry a
   per-axis scale. Refused for a standalone mesh, which belongs to no layer.

4. **A frame that can express a layer.** The per-axis scale reaches the
   sculptor, or the call refuses rather than silently dropping it — a scale
   quietly read as uniform is the same class of failure as the space mismatch.

5. **The helpers are centralized**, so no two crossings can disagree about
   whether a normal goes through the inverse transpose.

## What building it found

1. **A third crossing nobody had listed: the MASK.** `clay_mesh_sculptor_stamp`
   built its gate as `field_mask->sample(p)` where `p` is the vertex position
   the sculptor hands it — which is LOCAL — against a mask that is
   world-addressed by design. The stroke calls had a `mesh_to_world` for exactly
   this and the single-stamp path had nothing. Masking one stamp on a
   transformed layer was wrong, silently, and it is fixed by the same frame.

2. **The helpers went in the wrong place and the compiler said so.** Put beside
   the entry points they serve, inside `extern "C"`, they returned `cfloat3` and
   `MaskGate` under C linkage — the C4190 family this file's own comments warn
   about. They now live in the anonymous namespace with a note saying why.

3. **The local→world half of the helper set has no caller and was deleted.**
   The only value this ABI returns in world is a raycast hit, and
   `pick::raycast_mesh` already owns both directions of that conversion. A
   second pair here would have been a second answer to one question; `-Werror`
   caught it before it became one.

4. **pyclay cannot set a layer's per-axis scale at all.** `Document.set_layer_transform`
   takes a uniform scale, and `scale_axes` appears nowhere but `placement_report`,
   which reads. So a Python script cannot build the state the per-axis refusal is
   about — that gate is in C only, and this is recorded as a reachability gap for
   the ABI audit rather than worked around.

5. **The binding-parity gate caught a real asymmetry mid-change.** pyclay had
   grown a `clear_world_frame` that C spells as `set_world_frame(NULL)`. Rather
   than exempting it, pyclay now has one `set_world_frame` whose empty call
   clears, so the two bindings do not grow different vocabularies for one piece
   of state.

## Capabilities

### Modified Capabilities
- `c-abi`: every mesh-sculpting call states the space it takes and returns, a
  session can declare a world frame, and a layer's own transform — per-axis
  scale included — can be adopted rather than reconstructed by the host.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`
- `include/clay/mesh/`, `src/mesh/` — the sculptor's frame, kept
  representation-neutral: the adaptation belongs at the document/ABI wrapper
  rather than giving `MeshSculptor` scene knowledge.
- `bindings/python/pyclay_module.cpp` — parity.
- `docs/05-claycore-library.md` — the table.
- `tests/unit/test_c_mesh_sculpt.cpp` — translation, rotation, uniform and
  non-uniform scale, a combined transform, a transformed raycast feeding a
  transformed stamp, normals under non-uniform scale, undo/redo, save/reopen.
  On a deliberately ASYMMETRIC mesh, so an axis or rotation error is obvious.
