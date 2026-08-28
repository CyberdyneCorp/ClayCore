# Proposal: the instance layer nothing could create

## Why

Two contracts in `clay.h` describe instance layers in detail, and no call makes
one.

The memory report (clay.h ~514) says what an instance costs:

> INSTANCE layers share one edit list, which the document counts ONCE (ten
> instances of one blockout are one allocation, and saying otherwise would
> invite you to free memory that does not exist) while each instance reports it
> in full.

The undo dirty-bounds contract (clay.h ~757) says where an edit to one lands:

> an edit to content shared by instanced layers reports the union over every
> layer sharing it.

`bound-an-edit-across-instances` then went and made both ABI dirty-bounds
queries union over the sharers, `scene::node_command_bound` already did, and
`io::layer_memory` already charges shared content once. Grepping the ABI for
`instanc` finds those promises and nothing that produces the state they
describe: `scene::Document::instance_layer` exists in C++ and is reachable only
from the unit tests.

So a host doing the one gesture this is for — *duplicate this subtool*, ten
bolts from one bolt, an earring on the other ear — has to replay or deep-copy
the source layer's whole edit list per copy, paying memory and time
proportional to everything already sculpted. The accounting the memory report
was written to defend is unreachable from outside the library.

**And it would not have survived being reached.** Three things in the tree are
wrong for sharing, and the constructor alone would have shipped all three:

1. **A save/load round trip multiplies the allocation.** `write_layer` writes a
   `has_sdf` bool and then the whole content inline; `read_layer` builds a
   fresh `SdfContent` per layer. Ten instances go in as one allocation and come
   back as ten — which is precisely the "counted ONCE" promise, broken by the
   file rather than by the report. Worse than the memory: after a reload the
   layers are no longer linked, so an edit through one stops appearing in the
   others and nothing says so.
2. **Consolidate rewrites every sharer.** `consolidate_layer` edits
   `layer->sdf` through the command vocabulary, and that content is shared, so
   baking one instance would replace the edit list of every layer instancing it
   — nine subtools silently collapsing into a volume because the artist baked
   the tenth.
3. **Nothing can see the link.** After a reload the only thing distinguishing
   an instance is that two layers happen to point at the same allocation, which
   is not a question the ABI can be asked. A subtool panel cannot draw what it
   cannot query.

## What changes

**The constructor.** `clay_document_instance_layer(doc, source, name,
out_layer)`, through the command vocabulary exactly as `clay_add_sdf_layer` and
`clay_document_add_voxel_layer` do — `reserve_layer_id`, one `AddLayerCmd` —
so an enabled undo stack records the creation as ONE step and a single undo
takes the instance away. The new layer's `sdf` is the SOURCE's `shared_ptr`; no
content is copied, which is the whole point.

**A layer record that names its content instead of repeating it.** At scene
minor 15 a layer record carries a content-source layer id: 0 means "this layer
owns the content that follows", any other id means "share the content of that
layer and read nothing here". Ownership is derived at save time from
`shared_ptr` identity — the FIRST layer in stack order holding a given
`SdfContent` owns it and every later sharer names it — which needs no stored
flag and handles the case where the original source layer was removed while its
instances remain. A second pass on load resolves the names.

The journal is the other half: `AddLayerCmd` gains a `content_source` field, so
an instance creation serialises as a reference the same way. Replay runs
against a document that IS the snapshot, so the source layer is present and its
id resolves; an id that does not resolve refuses the replay rather than
inventing a copy.

**Consolidate SEVERS.** Before baking, a layer whose `SdfContent` has more than
one owner is given a private copy, so the bake affects that layer alone and
every other instance is untouched. The sever is expressed as the
remove-then-add pair `clay_document_move_layer` already uses, inside the group
the bake was already opening — so it is one undo step with the bake, and
undoing it restores the layer carrying its original shared pointer, link
intact.

**A read side.** `clay_layer_info` gains `content_source` and `share_count`,
appended to a descriptor that already negotiates `struct_size`.

## What we decided, and why

**Severing rather than refusing.** Refusing would have been defensible — the
issue offers it — but it makes the two gestures interfere: an artist who
duplicates a subtool can no longer bake either copy, including the original,
and nothing in the UI explains why a command that worked yesterday stopped.
Severing is also what the artist means. A bake says "this shape is finished";
it is a statement about ONE subtool, and there is no reading of it under which
the other nine should turn into volumes. The cost of severing is honest and
local — the layer stops tracking the source, which is what "finished" means —
and it is visible, because `clay_document_layer_info` reports the link's
disappearance.

**Two fields on `clay_layer_info`, not one, and not a dedicated call.** A
dedicated `clay_layer_content_source` would be a second lookup for a property
that belongs beside `representation` and `stack_index` in the call that already
says "everything about one layer in one call". And `content_source` alone
answers only half the panel's question: an instance reports the id it follows,
but the SOURCE of a link reports 0, which is indistinguishable from an ordinary
layer. A panel drawing a link needs both ends, so `share_count` — how many
layers share this layer's content, 1 when nobody does — comes with it.

`content_source` is derived by the SAME first-in-stack-order rule the writer
uses, so what a host is told is exactly what a save would write and the answer
survives a reload unchanged. It follows that removing a source layer does not
orphan anything: the first surviving sharer becomes the owner and reports 0,
and the rest name it.

**An instance of an instance shares the same content**, rather than chaining.
There is one allocation and the relation is "these layers share this edit
list", not a tree; making it a tree would invent a parent whose removal would
then have to mean something.

## Impact

**An ABI addition and an on-disk layout change.** `clay_document_instance_layer`
is a new symbol; `clay_layer_info` grows two fields at the end, which a host
compiled against the older header simply does not receive. Scene minor and the
container minor go to 15 together.

A pre-15 build opening a 15 document FAILS rather than misreads — the layer
record is not length-prefixed, so the new field desynchronises the stream and
the reader's bounds checks reject it. That is the same trade minors 7, 8, 11
and 14 make, and it is why `save_clayspace`'s `minor` parameter exists: written
AT minor 14, every layer's content goes out inline as it always did and an
instance comes back as an independent copy — the shape is right, the share is
gone, and an edit through one no longer reaches the other.

## Non-goals

**A layer-level instance of a VOXEL or MESH layer.** `instance_layer` already
refuses a non-SDF source and this maps that to
`CLAY_ERROR_INVALID_ARGUMENT`. Sharing a grid would need the voxel and mesh
side tables to be reference-counted, which is a different change with a
different memory contract.

**Per-instance overrides beyond what a `Layer` already carries.** An instance
copies the `Layer`, so transform, name, visibility, protection, mirror and
radial start at the source's values and diverge freely from there. Anything
narrower — "share the edit list but override this one item's colour" — is a
different feature and would need per-instance node state the format has no room
for.

**Instancing across documents.** A `shared_ptr` does not cross a
`clay_document`, and a cross-document reference would have to serialise as a
path.
