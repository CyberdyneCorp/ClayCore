"""Answering the question iOS asks, which is not "how big" but "which part".

`didReceiveMemoryWarning` arrives with no argument and expects a response
within a frame or two. To decide what to release a host must know what it is
holding, and until `roll-up-document-memory` this library could not say.

Every subsystem accounted for itself and NOTHING ROLLED UP. The history
reported its bytes, one grid reported its sculpt layers, the brick cache
reported a cache the document does not own — and the edit list, the voxel chunk
storage those sculpt layers sit beside, masks, mesh layers and the passthrough
blobs reported nothing at all. The rest is where the memory is.

The breakdown is the feature and the total is not, because the three things a
host might drop have three different consequences:

    the brick cache      -> a stall. Recomputed, bit-identical.
    the history          -> undo depth. The user notices immediately.
    voxel or mesh content-> the user's work. Never.

A single number cannot separate those, and the cheapest thing to release was
precisely the one nobody could measure.

This script builds a document the way an artist would — block out in SDF,
rasterize to voxels, paint a mask, record some history — and after each step
asks what it costs and where the cost went. Then it attributes the total to one
layer, which is what turns "your document is large" into something actionable.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.02
EYE, TARGET = (2.6, 1.8, 2.6), (0.0, 0.0, 0.0)

# What may be released, and what it costs to release it. The whole point of the
# breakdown: a host reads this table, not the total.
DROPPABLE = {
    "history": "undo depth",
    "voxel_sculpt_layers": "voxel undo depth",
    "passthrough": "a thumbnail; regenerable",
}
PARTS = ("edit_list", "voxel_content", "mesh_layers", "masks",
         "voxel_sculpt_layers", "history", "passthrough", "transient")


def kb(n):
    return f"{n / 1024:8.1f} KiB"


def report(doc, what):
    m = doc.memory
    # The parts must account for the whole, or the breakdown is decoration.
    total = sum(m[k] for k in PARTS)
    if total != m["total"]:
        raise SystemExit(
            f"the parts sum to {total} and the total says {m['total']} — a "
            "breakdown that does not add up is worse than no breakdown")
    print(f"\n  {what}")
    print(f"    {'total':<22}{kb(m['total'])}")
    for k in PARTS:
        if not m[k]:
            continue
        note = f"   <- droppable: costs {DROPPABLE[k]}" if k in DROPPABLE else ""
        print(f"    {k:<22}{kb(m[k])}{note}")
    return m


def main():
    R.banner("62 what this document costs, and which part")

    doc = clay.Document()
    sdf = doc.add_sdf_layer("blockout")
    report(doc, "an empty document")

    # --- 1. a blockout, in SDF -------------------------------------------
    for i in range(60):
        t = i / 59.0
        sdf.add(clay.Sphere(r=0.22 - 0.12 * t,
                            position=(0.0, -0.5 + 1.1 * t, 0.0)),
                blend=clay.Smooth(0.12), color="#c8b28c")
    after_sdf = report(doc, f"a 60-item SDF blockout")

    # --- 2. rasterized to voxels, which is where the memory actually is ---
    blocks = doc.add_voxel_layer("blocks", voxel_size=CELL)
    blocks.rasterize(doc, ((-0.5, -0.7, -0.5), (0.5, 0.7, 0.5)))
    if blocks.occupied_count < 10000:
        raise SystemExit(
            "the rasterize should have filled real cells — measuring an empty "
            "grid would compare zeros and prove nothing")
    after_voxels = report(doc, f"+ rasterized ({blocks.occupied_count} cells)")

    # THE HEADLINE. One voxel layer against a 60-item edit list.
    ratio = after_voxels["voxel_content"] / after_sdf["edit_list"]
    print(f"\n  the voxel layer outweighs the whole edit list by {ratio:.0f}x — "
          "and it was\n  the largest unreported term in the document.")

    R.render(doc, "62_document.png", eye=EYE, target=TARGET,
             caption="a blockout, rasterized — most of its cost is the voxels")

    # --- 3. a mask, and history -------------------------------------------
    freeze = doc.add_mask("blocks", cell_size=CELL)
    freeze.fill(((-0.3, -0.2, -0.3), (0.3, 0.2, 0.3)), 1.0)
    doc.enable_undo()
    for i in range(20):
        sdf.add(clay.Sphere(r=0.05, position=(0.3, -0.4 + 0.04 * i, 0.0)))
    final = report(doc, "+ a mask and 20 undoable edits")

    # --- what a warning would actually free -------------------------------
    freeable = sum(final[k] for k in DROPPABLE)
    print(f"\n  on a memory warning this document can give back "
          f"{kb(freeable).strip()} of\n  {kb(final['total']).strip()} "
          f"({100 * freeable / final['total']:.0f}%) without losing any of the "
          "artist's work.")

    # --- 4. attribute it to a layer ---------------------------------------
    # "Your document is large" is not actionable. "The blocks layer is 94% of
    # it" is.
    print("\n  per layer:")
    for name in ("blockout", "blocks"):
        m = doc.layer_memory(name)
        share = 100 * m["total"] / final["total"]
        print(f"    {name:<22}{kb(m['total'])}  ({share:4.1f}% of the document)")
        if m["history"] or m["passthrough"]:
            raise SystemExit("history and passthrough are document-wide and "
                             "must read zero for a layer")

    # Content partitions exactly across layers: every chunk, cell and triangle
    # belongs to exactly one layer id. The EDIT LIST does not, and that is
    # deliberate — see the note in clay.h.
    per_layer = sum(doc.layer_memory(n)["voxel_content"]
                    for n in ("blockout", "blocks"))
    if per_layer != final["voxel_content"]:
        raise SystemExit(
            f"content must partition across layers: {per_layer} != "
            f"{final['voxel_content']}")
    print("\n  content partitions exactly across the layers, so a per-layer "
          "list and the\n  document total never disagree about where the "
          "memory went.")

    # --- the one property that will surprise you --------------------------
    # Memory follows CHUNKS, not cells. A chunk is 32^3 cells allocated whole.
    probe = clay.Document()
    one = probe.add_voxel_layer("one cell", voxel_size=CELL)
    packed = probe.add_voxel_layer("packed", voxel_size=CELL)
    one.set((0, 0, 0), 1)
    packed.fill_box((1, 1, 1), (30, 30, 30), 1)   # the SAME chunk, filled
    a = probe.layer_memory("one cell")["voxel_content"]
    b = probe.layer_memory("packed")["voxel_content"]
    if a != b:
        raise SystemExit(
            "two layers touching the same single chunk must cost the same — "
            "if this changed, the chunk is no longer the allocation unit and "
            "the note in clay.h needs updating")
    print(f"\n  and one that will surprise you: {one.occupied_count} voxel and "
          f"{packed.occupied_count} voxels cost the\n  SAME {kb(a).strip()} — "
          "memory follows CHUNKS, not cells. What grows the\n  number is the "
          "REGION an artist has worked in, not how solidly they filled it.")


if __name__ == "__main__":
    main()
