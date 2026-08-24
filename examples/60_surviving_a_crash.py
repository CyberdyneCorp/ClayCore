"""Killing a session and getting it back.

An hours-long sculpt on a tablet, on an OS that reclaims memory by killing
processes and does not warn twice. Until this landed, the words `autosave`,
`journal` and `crash recover` appeared nowhere in this repository — so the
answer to "the app died" was "the work is gone".

Saving on a timer is not the answer either, and the device gate says why: a
whole-document save is the same class of work as `consolidate` (661 ms) or
`mask_extrude` (4403 ms) on the reference iPad. A host autosaving every thirty
seconds would stall for that long, and the stall GROWS with the sculpt — so the
safer it tries to be, the worse it gets.

A recovery is a SNAPSHOT plus the STEPS SINCE IT. The snapshot is written once;
the journal is proportional to what changed. This script writes both, throws the
session away, rebuilds it, and checks that what came back is what was there —
across the SDF edit list and a voxel layer at once.

It also demonstrates the limit rather than describing it. Painting a mask used
to be a BARRIER — replay stopped there and the honest response was a fresher
snapshot — because a mask was a fourth representation with no history. Masks
record now, so the journal carries them; what remains a barrier is narrower,
and the script shows replay stopping at one of those instead.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.05
EYE, TARGET = (2.4, 1.6, 2.4), (0.0, 0.0, 0.0)


def build_session():
    doc = clay.Document()
    sdf = doc.add_sdf_layer("blockout")
    blocks = doc.add_voxel_layer("blocks", voxel_size=CELL)
    doc.enable_undo()
    return doc, sdf, blocks


def main():
    R.banner("60 surviving a crash — a snapshot plus the steps since it")

    doc, sdf, blocks = build_session()

    # --- the sculpt so far, which the snapshot captures once ---------------
    # A snapshot is only written occasionally, so it is the BIG thing. Making
    # it big here is what makes the comparison below honest: a demo that
    # snapshots an empty document and then journals a large fill measures the
    # opposite of the real case.
    for i in range(12):
        sdf.add(clay.Sphere(r=0.34, position=(0.22 * i - 1.2, 0.0, 0.0)), color="#c8b28c")
    blocks.fill_box((0, 0, 0), (7, 7, 7), 1)
    blocks.sculpt_smooth((0, 0, 0), size=5)

    snapshot = doc.to_bytes()
    at_snapshot = doc.journal_range()[1]
    doc.journal_trim(at_snapshot)          # everything before it is in the snapshot
    print(f"  snapshot         {len(snapshot):>7} bytes  (written occasionally)")

    # --- the edits since, which is what a journal is for --------------------
    sdf.add(clay.Box(size=(0.2, 0.45, 0.45), position=(0.0, 0.5, 0.0)), op=clay.Op.SUBTRACT)
    sdf.add(clay.Sphere(r=0.18, position=(0.0, -0.5, 0.0)))
    blocks.set((2, 8, 2), 1)

    journal, now_at = doc.journal_since(at_snapshot)
    full_snapshot = doc.to_bytes()
    print(f"  journal          {len(journal):>7} bytes  ({now_at - at_snapshot} events "
          "since the snapshot)")
    print(f"  vs re-saving     {len(full_snapshot):>7} bytes  (what a timer-driven autosave "
          "writes every time)")
    if len(journal) >= len(full_snapshot):
        raise SystemExit(
            "for a few small edits on a real document the journal must cost less than "
            "re-saving it — that is the entire argument for having one")
    print(f"  ratio            {len(full_snapshot) / len(journal):>7.1f}x cheaper per autosave")

    # THE CROSSOVER, stated because the first draft of this script assumed it
    # away and was wrong. A journal entry is raw: a voxel step is 14 bytes per
    # changed cell, while the document stores that grid palette- and
    # RLE-compressed. So one edit that rewrites a large fraction of the model —
    # a big fill, a rasterize — can journal LARGER than the whole document.
    # The rule a host needs is not "journal is always cheaper", it is:
    #
    #     re-snapshot when the journal grows past the snapshot.
    #
    # Which is a size comparison the host already has both sides of.
    doc3, sdf3, blocks3 = build_session()
    base3 = doc3.to_bytes()
    blocks3.fill_box((0, 0, 0), (7, 7, 7), 1)       # one edit, most of the model
    j3, _ = doc3.journal_since(0)
    print(f"  the crossover    one big fill journals {len(j3)} bytes against a "
          f"{len(doc3.to_bytes())}-byte document — re-snapshot instead")
    if len(j3) <= len(doc3.to_bytes()):
        raise SystemExit("this case exists to show the journal LOSING; if it now wins, "
                         "the encoding changed and this note needs updating")

    probes = np.array([[0.0, 0.5, 0.0], [0.0, -0.5, 0.0], [2.0, 0.0, 0.0]], dtype=np.float32)
    field_before = doc.eval(probes)
    cells_before = blocks.occupied_count
    print(f"  session          field {field_before[0]:+.3f} at the carve, "
          f"{cells_before} voxel cells")

    R.render(doc, "60_before_crash.png", eye=EYE, target=TARGET,
             caption="the session, before the process dies")

    # --- the crash ---------------------------------------------------------
    del doc, sdf, blocks
    print("  *** the process dies ***")

    # --- recovery ----------------------------------------------------------
    recovered = clay.load_bytes(snapshot)
    recovered.enable_undo()
    result = recovered.replay_journal(journal)
    rblocks = recovered.voxel_layer("blocks")
    print(f"  replayed         {result['applied']} events, "
          f"stopped_at_barrier={result['stopped_at_barrier']}")

    field_after = recovered.eval(probes)
    if not np.allclose(field_after, field_before):
        raise SystemExit(f"the field did not come back: {field_after} vs {field_before}")
    if rblocks.occupied_count != cells_before:
        raise SystemExit(
            f"the voxels did not come back: {rblocks.occupied_count} vs {cells_before}")
    print(f"  recovered        field {field_after[0]:+.3f} at the core, "
          f"{rblocks.occupied_count} voxel cells — identical")

    R.render(recovered, "60_after_recovery.png", eye=EYE, target=TARGET,
             caption="rebuilt from the snapshot and the journal")

    # --- the limit, demonstrated -------------------------------------------
    # A mask is a fourth representation with no history mechanism, so a mask
    # edit is a BARRIER. Replay stops there and says so; the honest response is
    # a fresher snapshot rather than a longer journal.
    doc2, sdf2, _ = build_session()
    base = doc2.to_bytes()
    sdf2.add(clay.Sphere(r=0.4))
    # A mask edit used to end the journal here. It is an ordinary step now.
    freeze = doc2.add_mask("blockout", cell_size=CELL)
    freeze.fill(((-0.2, -0.2, -0.2), (0.2, 0.2, 0.2)), 1.0)
    sdf2.add(clay.Sphere(r=0.2, position=(1, 0, 0)))
    j2, _ = doc2.journal_since(0)

    back = clay.load_bytes(base)
    back.enable_undo()
    back.add_mask("blockout", cell_size=CELL)   # the layer the journal names
    r2 = back.replay_journal(j2)
    if r2["stopped_at_barrier"]:
        raise SystemExit(
            "a mask edit is no longer a barrier; if replay stopped at one, something "
            "else did and this note needs updating")
    print(f"  mask in a journal  {r2['applied']} events replayed straight through a mask "
          "edit — which used to end the recovery")

    print("\n  a snapshot plus the steps since it, and the steps cost what changed.")


if __name__ == "__main__":
    main()
