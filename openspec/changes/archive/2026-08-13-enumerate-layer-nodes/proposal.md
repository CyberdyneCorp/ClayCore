# Proposal: enumerate a layer's nodes

## Why

Nothing lists the nodes a layer holds (#91). `clay_layer_children` enumerates a
GROUP's children, and a layer's root is not a group — it has no node id at all
— so `clay_layer_children(doc, layer, 0, ...)` answers `CLAY_ERROR_NOT_FOUND`
and the layer's own top-level nodes are unreachable.

That leaves the discovery chain broken in the middle. #69 gave a reloaded host
its layers; #77 gave it a readable armature. Between the two there is nothing:
to READ a rig a host must first FIND it, and the only recourse is probing node
ids from 1 upward against `clay_layer_node_prim`, tolerating a run of misses
before giving up. That is the same guess #69 removed one level up. It is a
better guess — `clay_layer_node_prim` is definitive, so a hit is certain and
only a miss is ambiguous — but node ids are not dense and nothing bounds how
long a gap can be, so a rig placed after a long run of removed nodes is simply
invisible, and no value of "long enough" is defensible.

## What

The issue's option 2, the node-level sibling of what #69 landed, purely
additive:

- `clay_layer_node_count(doc, layer, out_count)` and
  `clay_layer_node_at(doc, layer, index, out_node)` — count-then-index over the
  layer's TOP-LEVEL nodes, in the layer's evaluation order, mirroring
  `clay_document_layer_count` / `clay_document_layer_at` call for call. An
  index at or beyond the count is a typed not-found, so a host walks to the end
  without a sentinel; a layer id that is not a layer's is not-found too.

Top level only, and the header says so: this is the sibling of
`clay_layer_children`, which continues to descend, so the whole tree is walked
by pairing the two — enumerate the roots, ask `clay_layer_node_prim` what each
one is, recurse with `clay_layer_children` through the ones it refuses as
groups. Making these enumerate the whole tree would duplicate what
`clay_layer_children` already reports and would lose the nesting, which is
structure a host needs to redraw an outliner.

The issue's option 1 — teaching `clay_layer_children` to accept node 0 as the
layer root — was not taken: 0 is `kNoNode`, and a call that answers for a
sentinel cannot also refuse an id that does not exist, which is the refusal a
host reads to detect a stale id.

## What it does not touch

- **Serialization.** The root list already round-trips in order — the writer
  walks from `roots` and the reader rebuilds it — so this is exposure, not
  plumbing. No format minor moves.
- **The scene model.** `SdfContent::roots` already exists and already IS
  evaluation order; nothing new is stored and evaluation reads nothing new.
- **`clay_layer_children`.** Unchanged in shape and in meaning: it still takes
  a group and still refuses an item.
- **Existing signatures.** Nothing changes shape; nothing renumbers.

## Impact

`c-abi` gains the node-discovery requirement. The parity gate stays green
untouched: it fails on a `pyclay` capability with no C counterpart, and this
adds to the C side. `pyclay`'s `Layer.children` has the same root-level gap and
should grow the same pair, which is its own change with its own tests. Docs:
`docs/05-claycore-library.md` §11 and the placed-node readback paragraph.
