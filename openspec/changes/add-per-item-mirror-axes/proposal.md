## Why
An item's mirror participation was a bool (`Node::mirror`, `clay_item_set_mirror`) and the axes lived on the layer (`clay_set_layer_mirror`). A host that mirrors what is made while symmetry is on, and leaves existing items as they were made, could only do half of it (#664, ClaySpaceDesktop #170 / #277). Turning symmetry on worked: items made with it off opt out. Turning it OFF, or switching X to Y, had to change the layer mirror, and every item made under the old axes changed with it.

Measured through the C ABI on a lone lump of radius 0.25 at (0.6, 0.4, 0), made under a layer mirror X, reading the field at its X twin's centre (-0.6, 0.4, 0) and at its Y reflection's (0.6, -0.4, 0). A copy there reads -0.25:

| item made under X, then the layer mirror is set to | X twin | Y reflection |
|---|---|---|
| X (as made) | -0.25 | 0.55 |
| off, before | 0.95 (twin gone) | 0.55 |
| Y, before | 0.95 (twin gone) | -0.25 (wrong plane) |
| off, item carrying own axes X | -0.25 | 0.55 |
| Y, item carrying own axes X | -0.25 | 0.55 |

The host cannot bake the old mirror either. A reflection has determinant -1, and neither a node transform nor `scale_axes` can express one.

There was also no setter or reader for a PLACED node's participation, so a host could not correct an item saved with the default `mirror = true` without re-adding it, which changes its id and its place in the chain.

## What Changes
- `scene::Node::own_mirror_axes`: the item's own mirror axes (`kMirrorX|Y|Z`, `0` for none) or `kMirrorAxesInherit` (0xFF, the default) to take the layer's. `scene::effective_mirror_axes(item, layer)` is the one definition of which axes an item is reflected through: its own when set, else the layer's when it participates, else none. The compiler (`emit_item`), the geometry and influence bounds, the cull pad (seam term and symmetry multiplicity), picking's selection bound, consolidation's patch test and the Move brush's drag images all read it.
- Own axes replace the layer's outright, whatever the participation flag says. The flag keeps deciding what an INHERITING item takes from the layer, and it alone still decides radial participation. The seam blend (`mirror_k`) and the planes stay the layer's.
- `SetNodeMirrorCmd {layer, node, mirror, own_mirror_axes}`: one undoable command for both values, with a new command tag in the journal.
- C ABI 0.120.1 -> 0.121.0: `CLAY_MIRROR_AXES_INHERIT`, `clay_item_set_mirror_axes` / `clay_item_mirror_axes` for the builder, `clay_layer_set_node_mirror` / `clay_layer_node_mirror` for a placed item. The reader also reports the effective axes. A group is refused, since evaluation never reads a group's mirror.
- Scene / `.clayspace` minor 19 -> 20: one byte appended to the node record. Older documents load with every item inheriting, so they evaluate as saved. Writing at a minor below 20 is refused for a document holding an item with its own axes, following minor 18's precedent: dropping the byte would give the item the layer's copies instead of its own. `layer_blocking_minor` names the layer, and `clay_document_writable_at_minor` / `_save_*_at_minor` give a distinct message for it.
- The Move and magnify drag images for an item carrying its own axes are that item's own reflections. This is the intended behaviour: both sides of an item that kept X move under a drag even with the layer's mirror off, because both sides are the item. An item held at 0 moves on the touched side only. The reach a host is told (`drag_images`, `clay_layer_move_surface`'s invalidation, the live transaction's dirty box) adds the dragged items' own axes.
- pyclay: `Layer.add(..., mirror_axes=)`, `Layer.set_node_mirror`, `Layer.node_mirror`.

## What building it found
- The layer's symmetry multiplicity feeds the cull chain pad, and a per-layer count cannot see an item's own axes: an item with its own X|Y on an unmirrored layer is three contributors, not one. `CullPadTerms` now carries the union of own axes, which only grows, so the incremental cull index keeps its raise-only contract on append. A placed-node change is not an `AddNodeCmd` and takes the general invalidation, as the layer-symmetry setters do.
- `drag_images` is a layer-level function, but the reach it answers now depends on items. Walking the layer on every frame to find the union would add an O(items) pass to the live drag's dirty report. The union is taken from the prepared items instead (`prepared_own_mirror_axes`), since only a reached item can move a copy.
- The downgrade cannot keep the exact cases. An item whose own axes equal what it would inherit could be written at 19 as an inheriting item, but the node writer has no layer: content is shared by instance layers whose mirrors differ. Refusing whenever any item carries its own axes is the rule the writer can apply without being wrong.
- `test_layer_composition`'s byte-count test compared minor 17 against the current minor and assumed only minor 18's block lay between them. It now compares against 18.

## Impact
Every existing document evaluates bit-identically: the default is inherit, and an inheriting item takes the same code path, the same drag image set and the same bounds it took before. Only items that set their own axes behave differently. ABI minor and format minor move. No descriptor struct changes, so `check_c_abi.py`'s struct mirror is untouched.
