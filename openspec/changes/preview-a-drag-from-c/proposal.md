# Preview a Move drag from C

## Why

`SdfMoveTransaction::preview_layer()` is what the sdf-sculpt-transaction spec
names as the way a host previews a drag:

> A Move transaction SHALL expose a preview as ordinary scene content — a layer
> whose affected items carry the drag — so it compiles, draws and picks like any
> other layer.

It is C++-only. `clay.h` carries `clay_sdf_move_preview_nodes`,
`_preview_grab_count` and `_preview_grab`, and not the content — so the route
the spec designates is closed to a C host (issue #388).

What a C host does instead, per pointer event: update the transaction, write
each resolved grab onto the layer with `clay_layer_add_deformer`, sample the
dragged surface out of the document, and then UNDO every grab inside the same
segment. Two document mutations and a full undo round-trip, to draw something
the transaction is already holding — and step four is not optional, because the
commit correctly refuses a layer that moved and the host's own gesture
accounting counts a live segment by the undo depth it left behind.

## What Changes

- **ADDED** `clay_sdf_move_preview_document`, returning a borrowed read-only
  document: the real document's layers with the dragged one replaced by the
  transaction's preview. It compiles, evaluates, meshes, picks and refills
  exactly as any document does, so a host draws the drag through machinery it
  already has and the real document is never touched.
- **ADDED** `clay_layer_deformer_count`, `clay_layer_remove_deformer` and
  `clay_layer_clear_deformers` — the issue's smaller, separate ask.
  `clay_layer_add_deformer` had no inverse, so undo was the only way to take a
  warp back, which also spends an undo entry the gesture never meant to make.

## Two decisions worth stating

**It carries the SDF layers and not the other representations.** A voxel grid, a
mask and a mesh layer are not part of the field tape — `compile_document` takes
`LayerKind::Sdf` and nothing else — so nothing that reads the FIELD is affected,
and a host that also draws voxel or mesh content reads it from the real document
as it already does. Copying them would charge a drag, by value, for content it
cannot change.

**The preview document is TOUCHED on every request**, and that is the part that
is silent when missed. A document caches its compiled tape by a revision the
mutating entry points bump, and none of them run here: a drag changes the shared
edit list behind the cache's back. Without the touch, the second frame of a
gesture is served the first frame's tape and the preview freezes after one
update — which is what the test caught.

## Impact

- Affected specs: `c-abi`
- Affected code: `bindings/c/clay.h`, `bindings/c/clay_c.cpp`
- Additive: no existing signature, struct or behaviour changes.
