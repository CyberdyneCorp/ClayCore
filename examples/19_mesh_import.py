"""Importing a mesh as a field.

The library could already *load* a mesh and could already hold a sampled
field. This is the step between: turning a triangle soup into a distance field,
which is what makes an imported model something you can WORK on rather than
only display. Once it is a field it blends, cuts and sculpts like anything the
engine made itself.

The distance is the easy half — a BVH and a closest-point query. **The sign is
the hard half**, and it is what this example is really about.

The obvious methods each fail on the meshes people actually import:

* **Ray parity** counts crossings along a ray. One hole in the surface flips
  the answer for an entire half-space behind it, because a ray leaving through
  the hole crosses an even number of times and reports the interior as outside.
* **The closest triangle's normal** is exact on a clean closed mesh and
  meaningless near an opening: the nearest triangle to a point inside the model
  may be one facing away, when the wall it should have hit is missing.

Real assets are not watertight. They have holes, flipped normals, duplicated
faces and self-intersections, so a method that is correct on clean input and
catastrophic on dirty input is the wrong method — dirty input is the input.

So the sign comes from the **generalized winding number**: the sum over
triangles of the signed solid angle each subtends at the query point, over 4π.
Exactly 1 inside a closed surface and 0 outside, and — the property that
matters — it degrades *continuously* on an open one, passing smoothly through ½
across a hole. The third section walks a probe out through a hole and prints
that curve.

Summed exactly it is linear in the mesh, which a narrow band cannot afford. So
the same BVH carries, per node, the aggregate of the triangles beneath it, and
a node far enough from the query contributes one term instead of being
descended. The fourth section shows that costs almost nothing in accuracy.
"""

import numpy as np

import pyclay as clay

import _render as R


def source_doc():
    """Something with a handle and a hole, so that a sign error is visible
    rather than subtle."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Torus(R=0.62, r=0.2), color="#b0784a")
    layer.add(clay.Sphere(r=0.42), blend=clay.Smooth(0.1), color="#b0784a")
    return doc


def volume_doc(volume, colour="#b0784a"):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(volume, color=colour)
    return doc


def drop_cap(mesh, fraction=0.16):
    """Return the mesh's triangles with a patch removed — an open surface, of
    the kind an import actually meets."""
    idx = np.asarray(mesh.indices).reshape(-1, 3)
    pos = np.asarray(mesh.positions)
    centroids = pos[idx].mean(axis=1)
    # Everything above a plane: a clean circular opening rather than noise.
    cut = np.quantile(centroids[:, 1], 1.0 - fraction)
    return pos, idx[centroids[:, 1] <= cut]


def main():
    R.banner("19 mesh import — a triangle soup becomes a field")

    EYE, TARGET = (2.2, 1.7, 2.4), (0, 0, 0)

    # --- there and back again ------------------------------------------------
    src = source_doc()
    mesh = src.mesh(resolution=96)
    print(f"  meshed the source: {mesh.triangle_count} triangles, "
          f"watertight {mesh.is_watertight()}")

    tiles = [R.render_array(src, eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["the source field"]
    for cell in (0.06, 0.03):
        v = clay.Volume.from_mesh(mesh, cell=cell)
        print(f"    imported at cell {cell:<5} -> {v.brick_count:>4} bricks, "
              f"{v.megabytes:5.2f} MB")
        tiles.append(R.render_array(volume_doc(v), eye=EYE, target=TARGET,
                                    width=205, height=195))
        labels.append(f"imported at {cell}")

    # A default cell size comes from the mesh's own size: one in world units
    # would be far too fine for a building and far too coarse for a bolt.
    auto = clay.Volume.from_mesh(mesh)
    print(f"    a default cell for this mesh is {auto.cell_size:.4f}")
    tiles.append(R.render_array(volume_doc(auto), eye=EYE, target=TARGET,
                                width=205, height=195))
    labels.append(f"default cell {auto.cell_size:.3f}")
    R.contact_sheet(tiles, "19_import_roundtrip.png", columns=4, caption=", ".join(labels))

    # --- the imported field agrees with the one it came from ------------------
    fine = clay.Volume.from_mesh(mesh, cell=0.025)
    rng = np.random.default_rng(11)
    probes = rng.uniform(-1.1, 1.1, size=(6000, 3)).astype(np.float32)
    truth = src.eval(probes)
    got = fine.eval(probes)
    near = np.abs(truth) < 0.1
    stored = np.array([fine.has_samples_at(tuple(p)) for p in probes])
    where = near & stored
    print(f"  near the surface, worst disagreement with the original field: "
          f"{np.abs(got[where] - truth[where]).max():.4f}")
    # The sign is the thing that must not be wrong anywhere, not just nearby.
    disagree = int(((truth > 0.02) & (got < 0)).sum() + ((truth < -0.02) & (got > 0)).sum())
    print(f"  of {len(probes)} probes, {disagree} disagree about which side they are on")
    if disagree:
        raise SystemExit("the imported field has the sign wrong somewhere")

    # --- a hole does not flip a half-space -----------------------------------
    pos, kept = drop_cap(mesh)
    open_mesh = clay.Mesh.from_triangles(pos, kept.reshape(-1))
    print(f"  removed a cap: {open_mesh.triangle_count} triangles, "
          f"watertight {open_mesh.is_watertight()}")

    holed = clay.Volume.from_mesh(open_mesh, cell=0.03)

    # Away from the opening the answer must be unchanged. Ray parity gets this
    # catastrophically wrong; the winding number does not.
    #
    # The probes are chosen by asking the SOURCE, not by eye: this shape is a
    # torus blended with a sphere, so "obviously inside" is easy to get wrong.
    cap_y = np.asarray(mesh.positions)[np.asarray(mesh.indices).reshape(-1, 3)].mean(axis=1)[:, 1]
    below_cap = float(np.quantile(cap_y, 1.0 - 0.16))
    field = src.eval(probes)
    deep = probes[(field < -0.08) & (probes[:, 1] < below_cap - 0.1)][:200]
    far = probes[field > 0.25][:200]
    print(f"  probing {len(deep)} points well inside and {len(far)} well outside, "
          f"all clear of the opening")

    wrong_in = int((holed.eval(deep) > 0).sum())
    wrong_out = int((holed.eval(far) < 0).sum())
    print(f"  with the cap gone: {wrong_in} inside points read as outside, "
          f"{wrong_out} outside points read as inside")
    if wrong_in or wrong_out:
        raise SystemExit("a hole flipped the sign away from the hole")

    R.contact_sheet(
        [R.render_array(volume_doc(clay.Volume.from_mesh(mesh, cell=0.03)),
                        eye=EYE, target=TARGET, width=310, height=290),
         R.render_array(volume_doc(holed, colour="#8d6a4f"),
                        eye=EYE, target=TARGET, width=310, height=290)],
        "19_import_hole.png", columns=2,
        caption="closed, and with a cap removed: the opening closes over plausibly "
                "instead of inverting the interior")

    # --- the winding number is continuous ------------------------------------
    # Straight up through where the cap was. A parity count would step from 1
    # to 0; this passes smoothly through a half, which is why an open mesh is
    # usable at all.
    print("  walking out through the opening, the winding number:")
    open_query = clay.MeshQuery(open_mesh)   # the tree is built here, once
    ys = np.arange(-0.2, 1.05, 0.15)
    line = np.array([[0.0, float(y), 0.0] for y in ys], np.float32)
    values = open_query.winding_number(line)
    for y, w in zip(ys, values):
        bar = "#" * int(round(max(float(w), 0.0) * 28))
        print(f"    y {y:+.2f}  {w:6.3f}  {bar}")
    if np.abs(np.diff(values)).max() > 0.35:
        raise SystemExit("the winding number jumped — it is not degrading continuously")

    # --- summarizing distant geometry ----------------------------------------
    # beta is how far a BVH node must be before it stands in for its triangles.
    # 0 sums every one of them, which is the ground truth to compare against.
    query = clay.MeshQuery(mesh)
    check = rng.uniform(-1.5, 1.5, size=(400, 3)).astype(np.float32)
    exact = query.winding_number(check, beta=0.0)   # every triangle, no shortcuts
    print("  summarizing distant nodes, against summing every triangle:")
    flips_at_default = None
    for beta in (1.0, 2.0, 4.0):
        fast = query.winding_number(check, beta=beta)
        flips = int(((fast > 0.5) != (exact > 0.5)).sum())
        if beta == 2.0:
            flips_at_default = flips
        print(f"    beta {beta:<4} worst difference {np.abs(fast - exact).max():.4f}, "
              f"{flips} points on the wrong side")
    if flips_at_default:
        raise SystemExit("the summarization changed which side a point is on")

    # --- an import is an ordinary item ---------------------------------------
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.95, 0.5, 0.95)), color="#7f8a94", rounding=0.05)
    layer.add(clay.Volume.from_mesh(mesh, cell=0.025, position=(0, 0.12, 0)),
              op=clay.Op.SUBTRACT)
    R.render(doc, "19_import_carved.png", eye=(2.3, 1.8, 2.3), target=TARGET,
             caption="an imported mesh subtracted from a box — once it is a field "
                     "it is just another item")

    R.export_model(volume_doc(clay.Volume.from_mesh(mesh, cell=0.025)),
                   "19_import.ply", resolution=96, decimate=0.1)


if __name__ == "__main__":
    main()
