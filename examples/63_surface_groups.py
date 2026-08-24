"""Isolate a region and sculpt it — PolyGroups, on a lattice everything shares.

This library had no concept of a named surface region on ANY representation.
Visibility was per LAYER, so "isolate the head" meant the head had been
authored as its own layer, a decision taken before the artist knew they would
want it. A layer holds exactly one mask, so N named regions could not be faked
with N masks. And scene groups group EDIT-LIST NODES, which says how three
items combine and nothing about which part of the resulting surface is the
head.

ONE WORLD-SPACE LATTICE, asked "which group is this surface point in"
identically whatever the surface is made of. The obvious alternative — a
per-face id on a mesh, a palette channel on a grid, something else for SDF — is
three mechanisms, three sets of semantics for hide/isolate/grow/border, and
they will disagree.

The free answer for SDF, mapping a surface point to the ITEM that produced it,
fails the two cases that matter and fails them for one reason: an artist's
groups do not respect the edit list, because the edit list is how the shape was
BUILT and a group is about what it IS. This script builds exactly that case —
an armour band spanning two items — and names it anyway.

What it costs: the boundary is quantised to the lattice rather than to the
representation, so a group border is a visible edge at that scale.

What it buys, and what this script renders: hiding is not deleting. Nothing is
cut, no hole is closed, the field is untouched — the produced mesh is filtered
— so showing a group again brings back the same triangles rather than a
re-meshed approximation of them.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.04
EYE, TARGET = (2.2, 1.3, 2.2), (0.0, 0.0, 0.0)

HEAD, BAND, BODY = 1, 2, 3


def tri_count(doc):
    return doc.mesh(voxel_size=0.015).triangle_count


def main():
    R.banner("63 naming a region of the model")

    doc = clay.Document()
    sdf = doc.add_sdf_layer("figure")

    # --- a body built from two items, deliberately ------------------------
    # The band below spans BOTH of them, which is the case that kills the
    # "map a surface point to the item that made it" shortcut.
    sdf.add(clay.Sphere(r=0.42, position=(0.0, -0.25, 0.0)),
            blend=clay.Smooth(0.15), color="#b9a184")
    sdf.add(clay.Sphere(r=0.30, position=(0.0, 0.38, 0.0)),
            blend=clay.Smooth(0.15), color="#b9a184")

    groups = doc.groups(cell_size=CELL)
    groups.fill(((-1, 0.22, -1), (1, 1, 1)), HEAD)
    # The band straddles the seam between the two spheres.
    groups.fill(((-1, -0.02, -1), (1, 0.20, 1)), BAND)
    groups.fill(((-1, -1, -1), (1, -0.04, 1)), BODY)

    print(f"  named {len(list(groups.ids))} regions: "
          f"{groups.cell_count(HEAD)} / {groups.cell_count(BAND)} / "
          f"{groups.cell_count(BODY)} cells")

    # The band spans two items, and the lattice does not care.
    band_front = groups.at((0.0, 0.10, 0.40))
    if band_front != BAND:
        raise SystemExit("the band should cover the seam between the two items")
    print(f"  the band spans BOTH items — a point on the seam reads {band_front}, "
          "which\n  no item-derived rule could have said")

    every = tri_count(doc)
    R.render(doc, "63_whole.png", eye=EYE, target=TARGET,
             caption="two items, three named regions")

    # --- isolate, which is hiding the complement --------------------------
    groups.isolate(HEAD)
    isolated = tri_count(doc)
    if not (0 < isolated < every):
        raise SystemExit(
            f"isolating should have removed geometry: {every} -> {isolated}. If "
            "this stopped filtering, hiding is a flag nothing consults again")
    print(f"\n  isolate(head)      {every} -> {isolated} triangles "
          f"({100 * (every - isolated) / every:.0f}% put away)")
    R.render(doc, "63_isolated.png", eye=EYE, target=TARGET,
             caption="isolate(head) — the rest is put away, not deleted")

    # --- sculpt what is left ----------------------------------------------
    # The geometry that is hidden is still THERE: hiding gates what you see and
    # pick, not what you may edit. Masking is the mechanism for "do not edit
    # here", and giving visibility that job too would be two mechanisms for one
    # intent that can disagree.
    sdf.add(clay.Sphere(r=0.12, position=(0.0, 0.62, 0.0)),
            blend=clay.Smooth(0.08), color="#8fa9c0")
    print(f"  sculpted on it     {tri_count(doc)} triangles with the new item")

    # --- show everything again, and check it came back exactly ------------
    #
    # Checked on a SEPARATE document that was not sculpted in between, because
    # the one above deliberately gained an item while the head was isolated —
    # comparing its counts would measure the new item rather than the restore.
    probe = clay.Document()
    probe_sdf = probe.add_sdf_layer("figure")
    probe_sdf.add(clay.Sphere(r=0.42, position=(0.0, -0.25, 0.0)),
                  blend=clay.Smooth(0.15))
    probe_sdf.add(clay.Sphere(r=0.30, position=(0.0, 0.38, 0.0)),
                  blend=clay.Smooth(0.15))
    probe_groups = probe.groups(cell_size=CELL)
    probe_groups.fill(((-1, 0.22, -1), (1, 1, 1)), HEAD)
    exact_before = tri_count(probe)
    probe_groups.isolate(HEAD)
    probe_groups.show_all()
    if tri_count(probe) != exact_before:
        raise SystemExit(
            f"showing must restore EXACTLY: {exact_before} -> {tri_count(probe)}. "
            "Nothing is cut and no hole is closed, so the same triangles have "
            "to come back rather than a re-meshed approximation of them")
    print(f"  hide then show     {exact_before} -> {exact_before} triangles, exactly")
    print("                     nothing was cut, so nothing had to be rebuilt")

    groups.show_all()
    restored = tri_count(doc)
    R.render(doc, "63_restored.png", eye=EYE, target=TARGET,
             caption="show_all() — nothing was cut, so nothing had to be rebuilt")

    # --- grow, and what growing means here --------------------------------
    before = groups.cell_count(BAND)
    body_before = groups.cell_count(BODY)
    claimed = groups.grow(BAND, steps=2)
    print(f"\n  grow(band, 2)      claimed {claimed} ungrouped cells")
    if groups.cell_count(BODY) != body_before:
        raise SystemExit(
            "grow must not eat a neighbouring group — nobody expects 'grow' to "
            "delete a region they named")
    print(f"  the body group is untouched ({body_before} cells) though it is "
          "face-adjacent\n  along the whole seam")

    # VOLUMETRIC, NOT GEODESIC — the honest limitation, stated where an artist
    # would meet it. ZBrush grows a face set ALONG the surface; this dilates in
    # 3D, so where a surface folds back within `steps` cells of itself, growth
    # crosses the gap.
    print("  (volumetric, not geodesic: growth dilates in 3D, so a fold closer\n"
          "   than 2 cells is crossed rather than followed)")

    # --- the border, which is the seam an artist would work ---------------
    rim = groups.border(BAND)
    print(f"\n  border(band)       {len(rim)} cells — the seam to mask, crease "
          "or polish")

    # --- and it all survives a save ---------------------------------------
    groups.set_visible(BODY, False)
    doc.save("output/63_groups.clayspace")
    back = clay.load("output/63_groups.clayspace")
    if not back.has_groups:
        raise SystemExit("the groups should have been written")
    reloaded = back.groups()
    if reloaded.visible(BODY):
        raise SystemExit(
            "a reloaded document that forgot what was hidden would show an "
            "artist geometry they had put away — 'hiding is not deleting' has "
            "to survive a save to mean anything")
    print(f"\n  saved and reloaded: the band still covers the seam "
          f"({reloaded.at((0.0, 0.10, 0.40))}), and the body\n  is still hidden")

    print(f"\n  hiding is not deleting. This document went {every} -> {isolated} "
          f"-> {restored};\n  it ends larger than it started because a new item "
          "was sculpted while the\n  head was isolated, which is the point: "
          "hidden geometry is put away, not\n  taken out of the model.")


if __name__ == "__main__":
    main()
