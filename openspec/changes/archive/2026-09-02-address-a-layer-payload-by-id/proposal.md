# Proposal: address a layer's payload by id, not only by its name

## Why

A layer id is the thing this ABI calls stable. `clay_document_set_layer_name`
says so in as many words, and tells a host what to do about it:

> Hold the id from creation or from `clay_document_layer_at` when the lookup has
> to survive a rename — ids are stable across save and load; names are not a key
> anything enforces.

That advice cannot be followed for the two representations that have a payload
to reach. A reopened document has exactly one route back to a voxel layer's
grid, `clay_document_voxel_layer(name)`, and one route back to a mesh layer's
geometry, `clay_document_mesh_layer(name)`. Both key on the name, and because
the name is not a key, both answer with the FIRST layer in stack order carrying
it. The id is stable, held, and useless for reacquisition; the name works and is
documented as not a key.

## What it costs a host today

Two voxel layers sharing a name shadow one another's grid, so an edit lands on
the wrong one — silently, because the lookup SUCCEEDS. There is no error to
check and no way to notice from the return value.

ClaySpaceDesktop closes the hole by refusing the rename
(`crates/clayspace-engine/src/document.rs`):

```rust
// A voxel layer's grid is reachable only by name — the ABI has no
// id-addressed accessor — and the lookup answers with the first layer
// in stack order carrying it. So two voxel layers sharing a name would
// shadow one another's grid, and a stroke would land on the wrong one.
```

That is a uniqueness rule the ABI asks for nowhere else, enforced on voxel
layers only, and it surfaces straight to the artist the moment a subtool panel
exists: every sphere added from the same starting form wants the same default
name, and the second "Esfera" is refused for a reason no artist can
reconstruct.

## What changes

Two accessors, the existing lookups minus the name resolution:

```c
clay_result clay_document_voxel_layer_by_id(clay_document* doc, clay_layer_id layer,
                                            clay_voxel_grid** out_grid);
clay_result clay_document_mesh_layer_by_id(clay_document* doc, clay_layer_id layer,
                                           clay_mesh** out_mesh);
```

The by-name pair stays. It is the convenient call for a host that knows a
document has one "Argila" and does not want to carry an id around, and removing
it would break every caller for the sake of a rule the ABI does not otherwise
keep.

**The three refusals are the whole design.** An id that names no layer, an id
that names a layer of another representation, and an id whose layer holds no
payload entry are all `CLAY_ERROR_NOT_FOUND`, and the second and third are the
ones a naive implementation gets wrong. The grid deliberately OUTLIVES its
layer — undoing `clay_document_add_voxel_layer` removes the layer and keeps the
cells beside the document so a redo can pick them back up — so an accessor that
looked the id up in the side table alone would hand back a grid whose layer is
currently undone. That is not the by-name behaviour and it would be a new hole,
not a new feature.

**`out_grid` and `out_mesh` are required.** The by-name form tolerates a NULL
out pointer because it doubles as an existence probe and still has `out_layer`
to answer with. Here the caller already holds the id and the payload handle is
the only answer the call has, so a NULL out pointer asks nothing —
`CLAY_ERROR_INVALID_ARGUMENT` rather than a success that reports nothing.
`clay_document_layer_info` is the call that answers whether a layer exists and
what representation it is.

**Lifetimes do not move.** A borrowed handle already names its layer rather than
caching a pointer and looks it up again on every call, so these return exactly
what the by-name pair and the create calls return, on exactly the same terms.

## Impact

Purely additive: two new symbols, no signature changed, no struct grown, no
enumerator moved. `.clayspace` does not move — nothing about the file format
changes, and the property these calls rely on (ids are stable across save and
load) is one the format already keeps.

The host-side uniqueness rule comes out, and a layer's name goes back to being
a label, which is what `clay_document_set_layer_name` says it is.

## Non-goals

**Making names unique.** The three create calls have never required it and this
change is the reason they do not have to start.

**An id-addressed accessor for an SDF layer.** An SDF layer's content is already
addressed by layer id everywhere — `clay_add_item`, `clay_layer_node_at`,
`clay_layer_bounds` — and has no borrowed payload handle to hand back.

**Removing the by-name pair, or deprecating it.** It answers the question it is
asked, and its answer is documented.
