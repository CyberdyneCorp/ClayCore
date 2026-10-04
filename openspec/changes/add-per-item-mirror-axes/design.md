## Context
Issue #664 offered two asks: per-item axes, or an undoable bake of the layer mirror into reflected copies. This change takes the first. A bake materializes copies, which multiplies what evaluation and every later edit pay. Per-item axes keep the mirror as something evaluation reads, which is what the layer mirror already is.

## Decisions

### One field, one sentinel, a separate name
`Node::own_mirror_axes` is a `uint8_t`: the `kMirrorX|Y|Z` bits, or `kMirrorAxesInherit` (0xFF). Inherit and "own axes 0" must be distinct: 0 is "no copies whatever the layer says", which is how an item made with symmetry off is kept single after symmetry is turned on. It is deliberately not called `mirror_axes`. `Layer::mirror_axes` means a default for every item, and one name for both is how a caller reads the wrong one.

### Own axes replace the layer's outright
`effective_mirror_axes(item, layer)` returns the item's own axes when set, else `layer.mirror_axes` when `item.mirror`, else 0. The alternative was to AND own axes with participation, so that an opted-out item ignores its own axes. That makes setting axes on an opted-out item a silent no-op, and it gives a host two switches to set for one decision. The participation flag keeps its meaning for an inheriting item and for the radial mode, which this change does not touch.

The seam blend and the planes stay the layer's. A per-item seam would be a second knob for something the issue did not ask about.

### The drag images are the item's own copies
Intended behaviour, written into `brush/move.h` and the brush-engine spec: an item carrying its own axes is reached through its own reflections (the drag, one reflection per own axis, then the layer's rotations if it participates). This is the existing rule, "the images are the copies the compiler emits of this item", applied to an item whose copies differ from the layer's. So with the layer's mirror off, a drag moves both sides of an item that kept X. A host that wants a formerly mirrored item to move on one side only sets that item's axes to 0, which is the per-item spelling of the bake.

The resolver builds the layer's image set once per drag exactly as before. An item that inherits reads a prefix of it, so its drag is bit-identical. An item with its own axes reads a set keyed on (axes, radial), built lazily. There are at most 16 per drag, and none on a layer where no item overrides. #663's coincident-image grouping runs per set, so it applies unchanged.

### Format: refuse, don't degrade
Minor 20 appends one byte to the node record and is gated on both sides. Writing at 19 drops the byte, and an item with its own axes would come back taking the layer's copies. That is a different sculpture in a file that opens cleanly, which is minor 18's case. So `layer_blocking_minor` refuses it and names the first layer holding such an item. A document where every item inherits still writes 19's exact bytes, and the test checks those bytes.

### Cull pad
Own axes lengthen the chain. `CullPadTerms::own_mirror_axes` is a raise-only union, and `layer_symmetry_multiplicity(layer, item_axes)` counts `popcount(layer | item_axes)`. It stays conservative in the same way the old count was: it overcounts items that do not carry the union.

## Risks
- A host that keeps passing `clay_item_set_mirror(-1)` to opt an item out, and also sets own axes, gets the own axes. The header states it.
- A journal holding a `SetNodeMirror` command does not replay on a build that predates the tag. Such a build refuses it in `deserialize`'s default arm rather than misreading it, which is the same trade `SetLayerComposition` made.
