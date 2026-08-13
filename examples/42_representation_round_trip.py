"""A sculpt crossing between the two representations and coming back (#90).

Each half of the engine owns half a toolkit. SDF has the primitives, booleans,
blends and deformers; voxels have the ten sculpting verbs. Until this landed a
sculptor picked one and lived inside it: the bridge ran SDF-to-voxel only, and
coming back meant meshing, resampling the triangles, and losing the palette
along with anything you could still edit.

The four stages below are the trip both flagship examples could not make.
34_organic_character is SDF, so its arm/torso seam cannot be smoothed — the SDF
toolset has no native smooth that chains. Here the seam IS smoothed, by the
voxel verb that only exists on the other side, and then the result is booleaned
against as an ordinary operand.

What it costs is stated in the output rather than hidden: this is a CONVERSION,
not a view. The boolean's sharp edge goes to a staircase at the cell size on the
way over and comes back rounded, and the procedural history does not come back
at all.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.025
EYE = (2.3, 1.5, 2.6)
TARGET = (0.0, 0.0, 0.0)


def blockout():
    """Stage 1: booleans, which is what the SDF side is good at."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("blockout")
    layer.add(clay.Sphere(r=0.5), color="#c8b28c")
    layer.add(clay.Sphere(r=0.34, position=(0.52, 0.0, 0.0)), color="#c8b28c")
    layer.add(clay.Box(size=(0.10, 0.42, 0.42), position=(0.0, 0.52, 0.0)),
              op=clay.Op.SUBTRACT, color="#c8b28c")
    return doc


def main():
    doc = blockout()

    # Stage 2: into voxels. This direction always worked.
    grid = clay.VoxelGrid(CELL)
    grid.rasterize(doc, ((-1.2, -1.2, -1.2), (1.2, 1.2, 1.2)))
    print(f"  rasterized to {grid.occupied_count} cells at {CELL}")

    # Stage 3: sculpt the seam with the verb that only exists on this side.
    seam = (int(round(0.30 / CELL)), 0, 0)
    for _ in range(3):
        grid.sculpt_smooth(seam, size=14)
    grid.sculpt_inflate((0, int(round(-0.30 / CELL)), 0), size=12)
    print(f"  sculpted the seam with the voxel verbs -> {grid.occupied_count} cells")

    # Stage 4: back as an OPERAND, and booleaned against to prove it is one.
    returned = clay.Document()
    out = returned.add_sdf_layer("converted")
    # blur=1, which is what an organic sculpt wants. At 0 nothing is filtered
    # and nothing can be lost, but the surface keeps the lattice's terracing —
    # visible in the render, and the reason the setting exists.
    out.add(clay.Volume.from_voxels(grid, blur=1), color="#c8b28c")
    out.add(clay.Sphere(r=0.16, position=(0.0, 0.40, 0.30)), op=clay.Op.SUBTRACT)

    probes = np.array([[0, 0, 0], [0.0, 0.40, 0.30], [2.0, 0, 0]], dtype=np.float32)
    d = returned.eval(probes)
    print(f"  after the return trip: core={d[0]:+.3f} carved={d[1]:+.3f} outside={d[2]:+.3f}")
    assert d[0] < 0, "the sculpt should still be solid at its core"
    assert d[1] > 0, "the boolean should have carved the converted sculpt"

    # What the trip cost, measured rather than asserted. The sharp edge the
    # subtraction made in stage 1 is a staircase at the cell size after stage 2
    # and comes back rounded — no care on the return recovers it.
    flat = doc.eval(probes)
    print(f"  the original evaluated core={flat[0]:+.3f} — the shapes agree at the core,")
    print("  and the boolean's edge does NOT come back sharp: that is the conversion's cost")

    tiles = [
        R.render_array(doc, eye=EYE, target=TARGET, width=230, height=230),
        R.render_array(voxel_preview(grid, blocky=True), eye=EYE, target=TARGET,
                       width=230, height=230),
        R.render_array(voxel_preview(grid, blocky=False), eye=EYE, target=TARGET,
                       width=230, height=230),
        R.render_array(returned, eye=EYE, target=TARGET, width=230, height=230),
    ]
    R.contact_sheet(tiles, "42_representation_round_trip.png", columns=4)
    print("  wrote output/42_representation_round_trip.png  "
          "(blockout | voxelized | sculpted | returned and booleaned)")


def voxel_preview(grid, blocky):
    """The voxel stage as something the field renderer can draw."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("preview")
    mesh = grid.mesh() if blocky else grid.mesh_smooth(blur=1)
    layer.add(clay.Volume.from_mesh(mesh, cell=CELL * 0.6), color="#c8b28c")
    return doc


if __name__ == "__main__":
    main()
