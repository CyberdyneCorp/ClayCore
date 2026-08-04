"""Meshing, validation, and file I/O — SDF and voxels in one document.

Shows the three meshers side by side, what decimation costs, that the SDF
meshes are watertight, and the round trip through `.clayspace` plus the export
formats. Also rasterizes an SDF into a voxel layer, which is the bridge
between the two halves of the library.
"""

import numpy as np

import pyclay as clay

import _render as R


def scene():
    """A shape with enough detail to tell the meshers apart."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.RoundBox(size=(1.2, 1.2, 1.2), r=0.15), color="#5b8fb9")
    layer.add(clay.Sphere(r=0.85, position=(0.55, 0.55, 0.55)),
              blend=clay.Smooth(0.2), color="#b96f5b")
    layer.add(clay.Cylinder(r=0.35, h=1.6), op=clay.Op.SUBTRACT)
    return doc, layer


def main():
    R.banner("08 meshing and I/O — meshers, validation, round trip")

    doc, layer = scene()
    eye, target = R.layer_camera(layer)
    R.render(doc, "08_scene.png", eye=eye, target=target, colors_from_field=True,
             caption="the scene the meshers are compared on")

    # --- mesher comparison ------------------------------------------------
    for mesher in ("marching", "nets"):
        mesh = doc.mesh(resolution=64, mesher=mesher)
        print(f"  {mesher:9s} {mesh.triangle_count:6d} triangles  "
              f"watertight={mesh.is_watertight()}  manifold={mesh.is_manifold()}")

    # Dual contouring ships behind an experimental flag.
    dc = doc.mesh(resolution=64, mesher="dual_contouring", experimental=True)
    print(f"  {'dual_c':9s} {dc.triangle_count:6d} triangles  "
          f"watertight={dc.is_watertight()}  manifold={dc.is_manifold()}  (experimental)")

    # --- decimation -------------------------------------------------------
    full = doc.mesh(resolution=64)
    for ratio in (0.5, 0.2, 0.05):
        small = doc.mesh(resolution=64, decimate=ratio)
        print(f"  decimate {ratio:<5} {full.triangle_count:6d} -> "
              f"{small.triangle_count:6d} triangles")

    R.export_model(doc, "08_meshed.ply", resolution=64)

    # --- export formats ---------------------------------------------------
    mesh = doc.mesh(resolution=48, decimate=0.1)
    for ext in ("obj", "ply", "glb"):
        R.save_model(mesh, f"08_export.{ext}")

    # --- document round trip ---------------------------------------------
    path = R.output_path("08_scene.clayspace")
    doc.save(path)
    reloaded = clay.load(path)

    probes = np.random.default_rng(7).uniform(-2, 2, size=(2048, 3)).astype(np.float32)
    before = doc.eval(probes)
    after = reloaded.eval(probes)
    identical = np.array_equal(before, after)
    print(f"  .clayspace round trip reproduces the field exactly: {identical}")
    if not identical:
        raise SystemExit("round trip changed the field")

    # --- SDF rasterized into voxels ---------------------------------------
    # The bridge between the two halves: sample the SDF into a voxel layer.
    combined = clay.Document()
    sdf_layer = combined.add_sdf_layer("sdf")
    sdf_layer.add(clay.Torus(R=0.8, r=0.3))
    voxels = combined.add_voxel_layer("voxels", voxel_size=0.08)
    voxels.rasterize(combined)
    cells = voxels.bounds()
    print(f"  rasterized SDF occupies cells {cells[0]} .. {cells[1]}")

    eye, target = R.voxel_camera(voxels, 0.08, elevation=32.0)
    R.render_voxels(voxels, "08_rasterized.png", eye=eye, target=target,
                    caption="an SDF torus sampled into a voxel layer")

    # A document holding both layer kinds round-trips as one file.
    both = R.output_path("08_combined.clayspace")
    combined.save(both)
    back = clay.load(both)
    print(f"  combined document reloaded: "
          f"{back.eval(np.zeros((1, 3), dtype=np.float32))[0]:.3f} at origin")


if __name__ == "__main__":
    main()
