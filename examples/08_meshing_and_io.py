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

    # --- what the validator actually measures ------------------------------
    # The two booleans above are two of eleven quantities the same pass
    # computes. The report is how you find out WHY, which is the only version
    # of this that helps when the answer is no.
    checked = doc.mesh(resolution=64)
    r = checked.validation_report(max_intersection_pairs=20000)
    # euler=0 is the torus in the scene, not a defect: V-E+F is 2 for a sphere
    # and 0 for genus 1. It is a topology readout the two booleans cannot give.
    print(f"  report    {r['triangles']:6d} triangles, {r['vertices']} vertices, "
          f"euler={r['euler_characteristic']} (0 = genus 1, the torus)")
    print(f"            boundary_edges={r['boundary_edges']} "
          f"non_manifold_edges={r['non_manifold_edges']} "
          f"degenerate={r['degenerate_triangles']} "
          f"intersecting_pairs={r['intersecting_pairs']}")
    print(f"            volume={checked.signed_volume:.4f} "
          f"area={checked.surface_area:.4f}  (positive volume = outward normals)")
    if r["boundary_edges"] or not r["clean"]:
        raise SystemExit("a marching-cubes mesh of a bounded document must be clean")
    if checked.signed_volume <= 0:
        raise SystemExit("outward-wound triangles must give a positive signed volume")

    # The pass is OFF by default, and `clean` reads intersecting_pairs — so a
    # default report says clean without having looked. `intersection_budget` is
    # the field that tells the two apart, and it is why it is carried at all.
    default = checked.validation_report()
    if default["intersection_budget"] != 0:
        raise SystemExit("the self-intersection pass should be off by default")
    print(f"            self-intersection checked up to {r['intersection_budget']} pairs; "
          f"a default report checks {default['intersection_budget']}")

    # --- decimation -------------------------------------------------------
    full = doc.mesh(resolution=64)
    for ratio in (0.5, 0.2, 0.05):
        small = doc.mesh(resolution=64, decimate=ratio)
        print(f"  decimate {ratio:<5} {full.triangle_count:6d} -> "
              f"{small.triangle_count:6d} triangles")

    R.export_model(doc, "08_meshed.ply", resolution=64)

    # --- export formats, and reading each one back ------------------------
    # Written AND read, in one loop. Exporting alone proves only that bytes
    # were produced: a writer that emits a file no reader accepts passes an
    # export-only check and fails the user. .glb is here because it is the
    # newest of the four to gain an importer, and was write-only before that.
    mesh = doc.mesh(resolution=48, decimate=0.1)
    for ext in ("obj", "ply", "fbx", "glb"):
        path = R.save_model(mesh, f"08_export.{ext}")
        back = clay.load_mesh(path)
        if back.triangle_count != mesh.triangle_count:
            raise SystemExit(f".{ext} round trip changed the triangle count: "
                             f"{mesh.triangle_count} out, {back.triangle_count} back")
        print(f"    .{ext:<4} read back: {back.triangle_count} triangles, unchanged")

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
