"""Exporting quads — and what "quads" does and does not mean here.

**This produces a REGULAR QUAD GRID DERIVED FROM A SAMPLING LATTICE. It is NOT
field-aligned retopology.** The quads follow the lattice, not the form: there
are no edge loops running around a limb or a mouth, no poles placed at
features, no denser rows where curvature asks for them, and the result is not
animation-ready — deform it and it pinches wherever the topology disagrees with
the shape, which is everywhere. This is the input a retopology pass *replaces*,
not the output one produces. If you want ZRemesher, QuadRemesher or Instant
Meshes topology, use one of those; the wireframes below show exactly what you
get instead.

What it *is* good for: getting quads into a DCC that prefers them, subdividing
a sculpt, and exporting a voxel model as the box faces it actually is.

Three things this script demonstrates, all of them printed rather than implied:

1. **A quad count is a target the mesher approaches, never one it hits.** The
   only lever is the lattice cell size, so asking for a count is a short search
   over it. Every mesh below prints what was requested against what came out.
2. **OBJ, PLY and FBX carry quads. GLB does not** — glTF 2.0 defines no quad
   primitive mode, so the writer keeps writing the triangulation. That is the
   one surprising outcome of this feature and it belongs in the gallery output
   rather than in a bug report.
3. **A voxel sculpt has a second mode**: one planar quad per exposed voxel
   face, which is the boxes the model actually is.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

# Committed models: `_render.save_model` fails above 400 KiB, and an ASCII OBJ
# of a quad mesh costs roughly 150 bytes a quad. These three are a 5x spread in
# density, which is what the wireframes have to show, and the densest still
# fits.
TARGETS = (400, 1000, 2000)

WIRE_PIXELS = 320
LINE_COLOR = np.array([0.86, 0.88, 0.92], dtype=np.float32)


def sculpt():
    """A form with a flat, a bulge and a concavity, so the lattice has
    something to disagree with — and bounded, so the camera can frame it."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.RoundBox(size=(0.5, 0.3, 0.5), r=0.04), color="#b0784a")
    layer.add(clay.Sphere(r=0.26, position=(0.24, 0.2, 0.22)),
              blend=clay.Smooth(0.06), color="#c98f57")
    layer.add(clay.Sphere(r=0.3, position=(-0.5, -0.1, 0.5)), op=clay.Op.SUBTRACT)
    return doc, layer


# -- drawing the quads themselves --------------------------------------------
#
# The gallery renderer traces the FIELD, which is the right picture of a sculpt
# and the wrong one of a topology: a shaded quad mesh and a shaded triangle
# mesh look identical. What this example is about is the edges, so it draws
# them.


def _project(positions, eye, target, fov_degrees, size):
    """World points -> (pixels, camera-space depth), a plain pinhole camera."""
    eye = np.asarray(eye, dtype=np.float32)
    target = np.asarray(target, dtype=np.float32)
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, np.array([0.0, 1.0, 0.0], dtype=np.float32))
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)

    rel = positions - eye
    x = rel @ right
    y = rel @ up
    z = rel @ forward
    # Behind the camera would project through the origin and draw a line across
    # the frame, so it is pushed just in front instead of dropped: the mesh is
    # convex enough here that nothing is behind, and a silent drop would hide
    # it if that stopped being true.
    z = np.maximum(z, 1e-3)
    scale = (size * 0.5) / np.tan(np.radians(fov_degrees) * 0.5)
    px = size * 0.5 + (x / z) * scale
    py = size * 0.5 - (y / z) * scale
    return np.stack([px, py], axis=1), z


def _facing_camera(positions, quads, eye):
    """Which quads face the viewer, from the cross of their own diagonals.

    Back-face culling rather than depth sorting: without it every wireframe is
    the front and the back of the model at once, and the lattice reads as noise
    instead of as a grid.
    """
    a = positions[quads[:, 0]]
    b = positions[quads[:, 1]]
    c = positions[quads[:, 2]]
    d = positions[quads[:, 3]]
    normal = np.cross(c - a, d - b)
    centre = (a + b + c + d) * 0.25
    return np.sum(normal * (np.asarray(eye, dtype=np.float32) - centre), axis=1) > 0.0


def _background(size):
    """The gallery's own vertical gradient, so these tiles sit beside the
    raycast ones without a seam."""
    t = np.linspace(0.0, 1.0, size, dtype=np.float32)[:, None, None]
    column = R.BACKGROUND_TOP[None, None, :] * t + R.BACKGROUND_BOTTOM[None, None, :] * (1.0 - t)
    return np.repeat(column, size, axis=1)


def quad_wireframe(mesh, eye, target, fov_degrees=32.0, size=WIRE_PIXELS):
    """An (H, W, 3) image of a quad mesh's EDGES over the gallery background."""
    size = max(64, int(size * R.FAST_SCALE)) if R.FAST else size
    supersample = 2
    canvas = size * supersample

    positions = np.asarray(mesh.positions, dtype=np.float32)
    quads = np.asarray(mesh.quads, dtype=np.int64)
    xy, _ = _project(positions, eye, target, fov_degrees, canvas)
    visible = quads[_facing_camera(positions, quads, eye)]

    # Every edge of every visible quad, as endpoint pairs.
    edges = np.concatenate(
        [visible[:, [0, 1]], visible[:, [1, 2]], visible[:, [2, 3]], visible[:, [3, 0]]]
    )
    p0 = xy[edges[:, 0]]
    p1 = xy[edges[:, 1]]

    image = _background(canvas)
    if len(edges):
        # One sample count for every edge, from the longest: a per-edge count
        # would mean a Python loop over tens of thousands of edges, and the
        # cost of over-sampling a short one is writing the same pixel twice.
        longest = float(np.max(np.abs(p1 - p0)))
        steps = int(min(max(longest, 1.0), canvas)) + 1
        t = np.linspace(0.0, 1.0, steps, dtype=np.float32)[None, :, None]
        points = p0[:, None, :] + (p1 - p0)[:, None, :] * t
        px = np.clip(points[..., 0].astype(np.int32), 0, canvas - 1).ravel()
        py = np.clip(points[..., 1].astype(np.int32), 0, canvas - 1).ravel()
        image[py, px] = LINE_COLOR

    image = image.reshape(size, supersample, size, supersample, 3).mean(axis=(1, 3))
    return np.power(np.clip(image, 0.0, 1.0), 1.0 / 2.2)


def main():
    R.banner("44 quad export — a lattice quad grid, NOT retopology")
    print("  These are lattice-derived quads: no edge loops, no feature poles,")
    print("  not animation-ready. A retopology pass REPLACES this, it does not")
    print("  produce it.\n")

    doc, _ = sculpt()

    # --- a count is approached, not hit -----------------------------------
    meshes = []
    for wanted in TARGETS:
        mesh = doc.mesh_quads(target=wanted)
        report = mesh.quad_report
        meshes.append(mesh)
        error = 100.0 * (report["quad_count"] - wanted) / wanted
        print(f"  requested {wanted:6d} quads -> got {report['quad_count']:6d} "
              f"({error:+.1f}%)  cell {report['cell_size']:.4f}  "
              f"{report['iterations']} mesh(es)  "
              f"converged={report['within_tolerance']}  clamped={report['clamped']}")

    # One camera for every picture below, framed on the MESH's own bounds
    # rather than the layer's: a layer's bounds include the sphere that was
    # subtracted, which is empty space here, and these tiles exist to be
    # compared — the only thing that may differ between them is the topology.
    eye, target = R.orbit_camera(meshes[-1].bounds, fov_degrees=32.0, margin=0.82)
    R.render(doc, "44_sculpt.png", eye=eye, target=target, fov_degrees=32.0,
             colors_from_field=True, caption="the sculpt the quads are meshed from")

    R.contact_sheet([quad_wireframe(m, eye, target) for m in meshes],
                    "44_quad_density.png", columns=len(TARGETS),
                    caption=f"the same form at {', '.join(str(t) for t in TARGETS)} quads")

    # --- the formats -------------------------------------------------------
    # OBJ and FBX for every density, because those are what a DCC gets handed;
    # PLY and GLB once, to complete the format story without three more
    # committed binaries.
    for wanted, mesh in zip(TARGETS, meshes):
        for ext in ("obj", "fbx"):
            R.save_model(mesh, f"44_quads_{wanted}.{ext}")

    smallest = meshes[0]
    R.save_model(smallest, f"44_quads_{TARGETS[0]}.ply")
    glb = R.save_model(smallest, f"44_quads_{TARGETS[0]}.glb")
    print(f"  {os.path.basename(glb)} is TRIANGLES: glTF 2.0 defines no quad "
          f"primitive mode, so the")
    print(f"  writer emits the same surface as {smallest.triangle_count} "
          f"triangles. That is not a bug.")

    # An OBJ face line has four corners; counting them is the cheapest proof
    # the quads reached the file rather than being triangulated on the way.
    obj = R.output_path(f"44_quads_{TARGETS[0]}.obj")
    with open(obj, "r", encoding="utf-8") as handle:
        faces = [line for line in handle if line.startswith("f ")]
    corners = {len(line.split()) - 1 for line in faces}
    print(f"  {os.path.basename(obj)}: {len(faces)} faces, {corners} corners each")
    if corners != {4} or len(faces) != smallest.quad_count:
        raise SystemExit("the OBJ did not come out as four-corner faces")

    # --- the voxel side ----------------------------------------------------
    voxel_doc = clay.Document()
    grid = voxel_doc.add_voxel_layer("sculpt", voxel_size=0.07)
    grid.rasterize(doc)

    dual = grid.mesh_quads()
    faces_mesh = grid.mesh_quads(mode="faces")
    print(f"\n  voxel dual  {dual.quad_count:6d} quads  (the rounded form, "
          f"cell {dual.quad_report['cell_size']:.3f})")
    print(f"  voxel faces {faces_mesh.quad_count:6d} quads  (one per exposed "
          f"voxel face — the boxes the model is)")
    # Faces mode welds its corners and carries no vertex normals: a welded
    # corner is shared by faces pointing three ways at once.
    print(f"  faces mode carries {faces_mesh.normals.shape[0]} vertex normals, "
          f"by design:")
    print("  a welded corner is shared by faces pointing three ways and has no "
          "single normal.")
    # The two counts agreeing is an identity, not a coincidence, and it is
    # worth saying so before it reads as a copy-paste bug: a lattice edge
    # changes sign exactly where an occupied voxel meets an empty one, which is
    # exactly an exposed face. Same number of quads, completely different
    # surfaces — which is what the picture shows.
    if dual.quad_count == faces_mesh.quad_count:
        print("  the two counts agree because a sign-changing lattice edge IS an "
              "exposed face;")
        print("  what differs is where the vertices go, not how many quads there are.")

    voxel_eye, voxel_target = R.orbit_camera(faces_mesh.bounds, fov_degrees=32.0,
                                             elevation=28.0, margin=0.78)
    R.contact_sheet([quad_wireframe(dual, voxel_eye, voxel_target),
                     quad_wireframe(faces_mesh, voxel_eye, voxel_target)],
                    "44_voxel_modes.png", columns=2,
                    caption="the rounded dual, then the boxes faces mode is")

    R.save_model(faces_mesh, "44_voxel_faces.obj")


if __name__ == "__main__":
    main()
