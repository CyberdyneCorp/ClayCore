# Design: rolling up document memory

## Where it lives

`clay/io/memory.h`, and not anywhere else. The rollup must name
`scene::Document`, `voxel::VoxelGrid`, `voxel::MaskField`, `mesh::Mesh` and
`session::History` in one signature, and `io` is the only module the layering
table lets see all five — it is where `ClaySpaceDoc` already binds them
together for exactly the same reason.

Putting it in `session` would fail the layering check outright (`session` does
not see `io`), and putting it in `scene` would fail worse: `scene` deliberately
cannot see `voxel` or `mesh`, which is the invariant that keeps a mask's
presence from changing what a document evaluates to.

Each subsystem grows its own `bytes()` and `io` only sums them. That keeps the
knowledge of what a container holds next to the container — `VoxelGrid` knows
its chunk map is `capacity()`-allocated and `io` should not have to.

## capacity(), not size()

Every figure walks real containers and uses `capacity()` where one
over-allocates, because that is what the allocator is holding. A `std::vector`
that grew to 1000 and was cleared reports its capacity, since the memory is
still checked out from the OS — and a host under pressure needs the true
number, not the logical one.

This makes some figures *larger* than the serialized size of the same content,
which is correct and worth stating in the header: a document that saves to
2 MB can easily hold 6 MB, because the file is RLE- and palette-compressed and
the live grid is not.

`std::unordered_map` is the awkward case: a node-based container's real cost is
buckets plus nodes, and neither is a public number. It is accounted as
`bucket_count() * sizeof(void*)` plus per-entry node size, which is an estimate
of a real allocation rather than a guess at an unknown one, and the header says
so.

## What the breakdown separates, and why those seams

The seams are chosen so each line answers *"can I release this, and what breaks
if I do"* — the only question a memory warning actually asks:

| Line | Droppable? | What it costs to drop |
|---|---|---|
| `edit_list` | no | it *is* the model |
| `voxel_content` | no | it is the model |
| `voxel_sculpt_layers` | yes | voxel undo depth |
| `masks` | no (cheap anyway) | authoring state |
| `mesh_layers` | no | imported geometry, unrecoverable |
| `history` | yes | undo depth — and `set_history_budget` is the lever |
| `passthrough` | yes | a thumbnail, regenerable |

Voxel content and voxel sculpt layers are split for this reason and no other:
they live in the same object, and one is the user's model while the other is
undo for it. A single "voxel" figure would hide the only voxel bytes a host is
allowed to touch.

## In-flight snapshots are reported, because they are transient and large

`MaskField` takes a full copy of its chunk map on the first `touch()` inside a
recorded step. During a mask stroke a mask therefore costs roughly **double**,
and the extra disappears when the step closes.

A host sampling memory mid-gesture sees that spike and would otherwise have no
way to know it is temporary. It is reported as its own field rather than folded
into `masks`, so a host does not act on a number that is about to halve on its
own.

## Per layer

The same struct, for one layer id. Not a different report: a host that shows a
per-layer list and a document total should not have to reconcile two shapes,
and the fields that cannot apply to a single layer (`history`, `passthrough` —
both document-wide) are documented as always zero rather than removed.

**The content lines sum exactly and the edit list does not**, which is worth
stating because the tempting claim — "the layers add up to the document" — is
false, and shipping it as a promise would be a lie a host could hit.

Content sums exactly because every voxel chunk, mask cell and triangle belongs
to exactly one layer id. The edit list does not, for two reasons that are both
deliberate:

- The document-wide figure includes **overhead owned by no layer**: the layer
  vector's capacity, the selection, the `Document` itself.
- **Instances share one `SdfContent`.** The document counts it once, because
  ten instances of one blockout are one allocation and telling a host otherwise
  invites it to free memory that does not exist. Each instance's own report
  counts it in full, because displaying an instance costs an evaluation like
  any other layer and reporting zero would call it free.

So the per-layer edit list is a **ceiling** on that layer's contribution rather
than a partition of it. The header says so; a test pins both halves.

## The C surface

Two entry points, both additive, both versioned descriptors:

```c
clay_result clay_document_memory(const clay_document* doc, clay_memory_report* out);
clay_result clay_layer_memory(const clay_document* doc, clay_layer_id layer,
                              clay_memory_report* out);
```

`clay_memory_report` carries `struct_size` first and is read with `write_desc`,
so a later field costs nothing — which matters because this struct is the one
most likely to grow: every subsystem added after this change is a line in it.

## Testing what a rollup can actually be wrong about

A total is hard to assert and easy to write a vacuous test for — "it returned a
number, and the number was positive" passes against a stub. Three properties
are checkable and are what the tests assert instead:

1. **It moves with the content.** Rasterize a sphere into a voxel layer and
   `voxel_content` must rise; the assertion is a *ratio against the empty
   document*, not an absolute byte count, so it survives a container swap.
2. **The parts sum to the total.** Every field except `total` adds to `total`,
   checked by summing the fields rather than by restating the number — so a
   field added later without being summed fails.
3. **It attributes to the right line.** Painting a mask must move `masks` and
   must **not** move `voxel_content` or `edit_list`. This is the assertion that
   catches a rollup that double-counts or sums into the wrong bucket, which a
   total-only test cannot see.

And one that pins the transient: bytes measured mid-step exceed bytes after the
step closes, on a mask, because the snapshot was released.

## Memory follows chunks, not cells

Found by a fixture that passed for the wrong reason, and worth stating in the
design because it is the single most surprising thing about the figure.

A voxel chunk is 32³ cells allocated whole. So **one voxel costs 32 KiB**, and
32 768 voxels filling that same chunk cost the same 32 KiB. The first version
of the per-layer test filled ±20 against ±3 — 200× apart in occupancy — and
both straddle the origin, touching **exactly the same eight chunks**. The two
layers reported an identical figure, which is correct behaviour and a useless
fixture: the asymmetry the test was about did not exist.

The fixture now differs in **chunk span** rather than cell count, and a second
test pins the quantization directly, because a host presenting `voxel_content`
beside `occupied_count` will otherwise read their independence as a bug. What
grows this number is the *region* an artist has worked in, not how solidly they
filled it.

## Absolute byte counts are not asserted

Deliberately. `sizeof(Node)` changes when a member is added, `bucket_count()`
is implementation-defined, and libstdc++ and libc++ do not agree on either. A
test asserting "an empty document is 312 bytes" would fail on macOS for a
reason that is not a defect. Every assertion is a ratio, a sum, or a
direction of change.
