# Proposal: a layer can be renamed

## Why

A layer is named by whichever call creates it — `clay_add_sdf_layer`,
`clay_document_add_voxel_layer`, `clay_document_add_mesh_layer` — and through
0.29.1 nothing could change it (#92). Renaming a layer is ordinary work in the
layer panel of every tool of this kind, so a host keeps its own display name
beside the document's. Then the rename is **lost on save**, because the
document only ever knew the creation name, and the two names drift.

`clay_layer_name` (#69) did not cause that; it made it visible, which is worse
than the old silence. Before it, names were unreadable and every host
regenerated them. Now a reopened document confidently answers with the
creation name, and a host that trusts the ABI shows the artist a name they
changed and believed they had saved. It looks correct.

There is a second consequence the fix has to speak to: `clay_document_voxel_layer`
and `clay_document_mesh_layer` take a NAME, so the creation name is also a
lookup key. Renaming has to say what happens to that key.

## What

One entry point, purely additive:

- `clay_document_set_layer_name(doc, layer, name)` — the setter for the name
  `clay_layer_name` reads back.
- `scene::SetLayerNameCmd` behind it, in the one command vocabulary. A rename
  is therefore one undo step, is refused on a ghosted or locked layer, and its
  inverse is the previous name, exact by construction. A rename that escaped
  undo would be a new inconsistency, not a fix — the mirror setter was
  corrected for exactly that reason in 0.27.

## Decisions

- **Duplicates are allowed, and what a duplicate means is documented.** The
  three create calls each accept a name another layer already carries, so
  refusing one here would buy a uniqueness the document has never had: a host
  could still shadow a lookup by creating two layers with the same name. A
  half-guarantee is worse than a stated rule, so the rule is stated —
  `clay_document_voxel_layer` / `clay_document_mesh_layer` answer with the
  FIRST layer in stack order carrying the name — and hosts are told to hold
  the id, which is stable across save and load, when a lookup must survive a
  rename. `clay_document_voxel_layer_by_id` is explicitly out of scope (#92).
- **NULL and the empty string are refused.** Empty is what a cleared text
  field submits, and the document's name is the only one left to lose.
- **No length limit.** The saved layer record length-prefixes the name with a
  `uint32` and `clay_layer_name` is a size query, so a long name costs the
  reader a bigger buffer rather than a truncation. A cap here would also be
  one the create calls do not apply.

## What it does not touch

- **The file format.** `write_layer` has always recorded the name; the rename
  changes what is written, not how. No format minor moves.
- **The command tape's existing tags.** `SetLayerName` is appended to the tag
  enumeration and to the `Command` variant; no tag renumbers.
- **Existing signatures.** Nothing changes shape, no enumerator's value moves.

## Impact

`c-abi` gains the rename requirement. `pyclay` exposes `Layer.name` read-only
and stays that way for now — the parity gate is unaffected, since it fails on
pyclay capabilities C cannot reach, not the reverse. Docs:
`docs/05-claycore-library.md` §11.
