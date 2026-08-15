# Proposal: expose layer enumeration to hosts

## Why

After `clay_document_load` a host knows nothing about what it just loaded
(#69, part 2 of #6). Every entry point that yields a `clay_layer_id` either
creates the layer or picks one with a ray, and the only per-layer getter a
`const clay_document*` offers is protection. No count, no index, no getter
for name, visibility, representation or stack position — all of which are
settable.

Hosts cope by probing ids against `clay_layer_bounds` with a guessed gap
constant. That recovers ids only, in O(highest id) calls: names are
regenerated, every layer is assumed visible and SDF (a voxel layer comes back
as an SDF one, its grid orphaned), and stack order is lost — probing yields
creation order, and after any `clay_document_move_layer` those differ. Stack
order is evaluation order, so a reopened document can silently EVALUATE
differently from the one saved. That last one is a correctness bug, not a
cosmetic gap.

## What

The read half of the layer surface, purely additive:

- `clay_document_layer_count` + `clay_document_layer_at(doc, index, out_id)`
  with `index` being stack position — the set and the order in one small
  pair, the issue's option 1.
- `clay_document_layer_info` filling a versioned `clay_layer_info` descriptor
  (`struct_size` leading, per the established convention): id,
  representation, stack index, visibility, ghost, locked — the issue's
  option 2.
- `clay_layer_name` by the size-query pattern `clay_list_backends` uses,
  because the name is the one layer property without a fixed size.
- `clay_layer_representation` declared as an enum whose values match the
  layer record's kind byte in a saved document.

## What it does not touch

- **Serialization.** Name, kind, visibility, protection and stack order
  already round-trip — `write_layer` records them all and layers are written
  and reread in vector order — and the file-io round-trip requirement already
  pins the bytes. The work is exposure, not plumbing; no format minor moves.
- **The scene model.** Everything exposed already exists on `scene::Layer`;
  evaluation reads nothing new.
- **Existing signatures.** Nothing changes shape; no enumerator renumbers.

## Impact

`c-abi` gains the discovery requirement. `pyclay` is already ahead here
(`Layer.name` existed Python-side and was exempted in the parity gate as
C-unreachable; the exemption is dropped because `clay_layer_name` now
satisfies it). Docs: `docs/05-claycore-library.md` §11.
