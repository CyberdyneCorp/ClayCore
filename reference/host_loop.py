#!/usr/bin/env python3
"""A reference host session: what an application does AROUND the verbs.

The gallery in `examples/` shows what each verb DOES. This shows the ORDER a
host has to put them in, which is the part a header cannot state and a
single-feature page cannot reach: `undo_enabled` means nothing until an edit
has been grouped, `redo_depth` means nothing until something is undone, and
`trim_stroke` means nothing to a stroke that is not still being dragged.

Written to be READ. Every phase says what a host is doing at that point and
what breaks if the order is wrong, so a Swift or Rust host can be written from
this file without running it. Every phase also ASSERTS, because a session that
only printed would rot the way an export-only I/O check does — see
`08_meshing_and_io`, which wrote three formats for months and read none.

Dependency-free on purpose: pyclay, numpy, and the standard library.
No window, no event loop, no rendering.
"""

import pathlib
import re
import sys
import tempfile

import numpy as np

import pyclay as clay

VOXEL = 0.05


def fail(phase, message):
    raise SystemExit(f"reference-host: {phase}: {message}")


# ---------------------------------------------------------------------------
# 1. Open a document
# ---------------------------------------------------------------------------

def open_document():
    """A host opens a document before it can do anything else, and turns undo
    ON before the first edit it intends to be undoable.

    `enable_undo` is off by default so a document that never needs it pays
    nothing — which means an edit made BEFORE the call is not undoable at all.
    A host that enables undo lazily, after the user's first stroke, silently
    loses that stroke from the stack. Enable it when the document is opened.
    """
    doc = clay.Document()
    form = doc.add_sdf_layer("form")
    form.add(clay.Sphere(r=0.6))

    if doc.undo_enabled:
        fail("open_document", "undo must be OFF until enable_undo is called")
    doc.enable_undo()
    if not doc.undo_enabled:
        fail("open_document", "enable_undo did not enable undo")

    print("  1. opened a document, undo enabled before the first edit")
    return doc, form


# ---------------------------------------------------------------------------
# 2. Pick against the surface
# ---------------------------------------------------------------------------

def pick_the_surface(doc, form):
    """Every brush gesture starts as a pick. A host turns a screen ray into a
    point ON the surface, and needs the normal there to orient the dab.

    `snap_to_surface` takes points, not rays, and returns position AND normal
    together — a host that asks for them separately pays the field twice.
    `gradients` is the same field's derivative and is what a normal IS, so it
    is the cross-check rather than a second source of truth.
    """
    probes = np.array([[0.0, 1.2, 0.0], [0.9, 0.0, 0.0]], dtype=np.float32)
    snapped = doc.snap_to_surface(probes)
    if set(snapped) != {"position", "normal"}:
        fail("pick_the_surface", f"expected position and normal, got {sorted(snapped)}")

    on_surface = snapped["position"]
    residual = np.abs(doc.eval(on_surface))
    if residual.max() > 1e-3:
        fail("pick_the_surface",
             f"snapped points are not on the surface (max |d| = {residual.max():.2e})")

    # The returned normal is the normalised gradient. Checked rather than
    # assumed: a host that lights a dab by one and orients it by the other
    # would only find out they disagreed on a curved region.
    grad = doc.gradients(on_surface)
    grad /= np.linalg.norm(grad, axis=1, keepdims=True)
    if not np.allclose(grad, snapped["normal"], atol=1e-3):
        fail("pick_the_surface", "normal and normalised gradient disagree")

    # What the host draws a selection box around.
    lo, hi = form.selection_bounds([1])
    if not (lo[0] < 0.0 < hi[0]):
        fail("pick_the_surface", f"selection bounds do not contain the sphere: {lo} {hi}")

    print(f"  2. picked {len(on_surface)} points, normals agree with the gradient,")
    print(f"     selection bounds {tuple(round(v, 2) for v in lo)} .. "
          f"{tuple(round(v, 2) for v in hi)}")
    return on_surface


def query_the_mesh(doc):
    """A host that has meshed the document can ask insideness and distance of
    the TRIANGLES rather than the field — which is what a hit test against an
    imported model has to use.

    `MeshQuery` builds its tree in the constructor, not per call. A host that
    constructs one per query rebuilds the tree every frame; build it when the
    mesh changes and keep it.
    """
    mesh = doc.mesh(resolution=32)
    query = clay.MeshQuery(mesh)
    points = np.array([[0.0, 0.0, 0.0], [3.0, 3.0, 3.0]], dtype=np.float32)

    inside = query.contains(points)
    if not (inside[0] > 0.5 and inside[1] < 0.5):
        fail("query_the_mesh", f"origin must be inside and (3,3,3) outside, got {inside}")

    signed = query.signed_distance(points)
    if not (signed[0] < 0.0 < signed[1]):
        fail("query_the_mesh", f"signed distance must agree with insideness, got {signed}")

    print(f"  3. mesh query: inside={inside[0]:.0f}/{inside[1]:.0f}, "
          f"signed {signed[0]:.2f} / {signed[1]:.2f}")
    return mesh


# ---------------------------------------------------------------------------
# 3. Edit under undo grouping
# ---------------------------------------------------------------------------

def stroke_with_undo(doc, form):
    """A gesture is MANY edits and ONE undo. Without grouping, a user who
    strokes across a model and presses undo gets one dab back, then another,
    then another — the complaint that undo "does nothing".

    begin/end_undo_group is what makes a drag atomic. The group must be closed
    before the state is queried: `redo_depth` counts undone STEPS, and a step
    is not a step until its group ends.
    """
    probe = np.array([[0.75, 0.0, 0.0]], dtype=np.float32)
    before = float(doc.eval(probe)[0])

    doc.begin_undo_group()
    for i in range(4):
        form.add(clay.Sphere(r=0.22, position=(0.55 + 0.06 * i, 0.0, 0.0)),
                 blend=clay.Smooth(0.08))
    doc.end_undo_group()

    grouped = float(doc.eval(probe)[0])
    if grouped >= before:
        fail("stroke_with_undo", "the four dabs did not change the field")
    if doc.redo_depth != 0:
        fail("stroke_with_undo", f"nothing is undone yet, redo_depth={doc.redo_depth}")

    if not doc.undo():
        fail("stroke_with_undo", "undo reported nothing to undo")
    if abs(float(doc.eval(probe)[0]) - before) > 1e-6:
        fail("stroke_with_undo",
             "ONE undo must take back the whole group, not one dab of it")
    if doc.redo_depth != 1:
        fail("stroke_with_undo", f"one group undone, redo_depth={doc.redo_depth}")

    if not doc.redo():
        fail("stroke_with_undo", "redo reported nothing to redo")
    if abs(float(doc.eval(probe)[0]) - grouped) > 1e-6:
        fail("stroke_with_undo", "redo did not restore the grouped edit exactly")
    if doc.redo_depth != 0:
        fail("stroke_with_undo", "redo must consume the redo stack")

    print("  4. four dabs, one group: a single undo took all four, redo restored them")


def trim_a_live_stroke(doc, form):
    """A placed stroke is still being dragged: the host appends points as the
    pencil moves, and takes them back when it moves backwards or the user
    lifts and re-enters. That is `trim_stroke`, and it is the reason a stroke
    is a NODE with points rather than a bag of stamps.

    `Stroke` is a PRIM: `add_point` authors it, `Layer.add` places it as one
    node, and `trim_stroke` edits that node afterwards. A host confuses the two
    at its peril — the first is geometry being built, the second is a command
    against a document, and only the second is undoable.
    """
    stroke = clay.Stroke()
    for i in range(6):
        stroke.add_point((-0.9 + 0.3 * i, 0.7, 0.0), 0.12)
    node = form.add(stroke)

    tip = np.array([[0.6, 0.7, 0.0]], dtype=np.float32)
    with_tail = float(doc.eval(tip)[0])
    form.trim_stroke(node, 2)
    trimmed = float(doc.eval(tip)[0])
    if trimmed <= with_tail:
        fail("trim_a_live_stroke",
             "trimming the last points must retract the stroke's far end")

    print(f"  5. placed a 6-point stroke and trimmed 2: field at the tip "
          f"{with_tail:.3f} -> {trimmed:.3f}")


# ---------------------------------------------------------------------------
# 4. Manage layers
# ---------------------------------------------------------------------------

def manage_layers(doc):
    """Layer bookkeeping is where a host spends its non-sculpting time, and
    where the ORDER is least obvious.

    The trap: protection is checked BEFORE the operation, and reordering is an
    operation. A ghosted layer refuses `move_layer` exactly as it refuses an
    edit, so a host that ghosts a layer and then rearranges the stack gets an
    error rather than a move. Clear protection first, then reorder.
    """
    scratch = doc.add_sdf_layer("scratch")
    scratch.add(clay.Box(size=(0.3, 0.3, 0.3), position=(0.0, -0.8, 0.0)))

    if doc.layer_protection(scratch.id) != (False, False):
        fail("manage_layers", "a new layer must start unprotected")

    doc.set_layer_protection(scratch.id, ghost=True, locked=True)
    if doc.layer_protection(scratch.id) != (True, True):
        fail("manage_layers", "protection did not stick")

    try:
        scratch.add(clay.Sphere(r=0.1))
    except ValueError:
        pass
    else:
        fail("manage_layers", "a ghosted layer must refuse an edit")

    try:
        doc.move_layer(scratch.id, 0)
    except ValueError:
        pass
    else:
        fail("manage_layers",
             "a ghosted layer must refuse a REORDER too — this is the trap")

    doc.set_layer_protection(scratch.id, ghost=False, locked=False)
    doc.move_layer(scratch.id, 0)
    doc.remove_layer(scratch.id)
    print("  6. protection refuses edits AND reorders; cleared, moved, removed")


def mask_a_region(doc):
    """A mask is attached to a LAYER by name, so the layer must exist first,
    and it is stored in the document rather than beside it — which is why
    removing it is a document call and returns whether there was one.
    """
    mask = doc.add_mask("form", cell_size=VOXEL)
    mask.paint_cell((0, 10, 0), size=6, target=1.0,
                    shape="sphere", falloff="smooth", strength=1.0)

    if not doc.remove_mask("form"):
        fail("mask_a_region", "removing an attached mask must report True")
    if doc.remove_mask("form"):
        fail("mask_a_region", "removing it twice must report False, not raise")
    print("  7. painted a mask cell, removed it, and the second removal said False")


# ---------------------------------------------------------------------------
# 5. Paint, mirror, and the level stack
# ---------------------------------------------------------------------------

def paint_live(doc):
    """The voxel side of a host: a palette, painting with symmetry, erasing a
    selection, and the level stack.

    Two traps here, both of which cost a host a debugging session.

    `paint_mirrored` RECOLOURS, and only occupied cells. A host expecting it to
    deposit material sees the count never move and concludes the call is
    broken; it is a colour operation, so the asserts below check colour.

    And the mirror is about the ORIGIN PLANE, not the origin cell: cells
    straddle x=0, so the partner of cell 3 is cell **-4**, by `m.x = -1 - m.x`.
    A host that mirrors to `-x` writes one cell off, symmetrically, and the
    seam only shows up as a one-cell ridge down the middle of the model.
    """
    grid = doc.add_voxel_layer("blockout", voxel_size=VOXEL)
    ink = grid.palette_add("#c8703a")
    grid.palette_set(ink, (0.20, 0.60, 0.90))

    grid.fill_box((-4, -4, -4), (4, 4, 4), ink)
    filled = grid.occupied_count

    accent = grid.palette_add("#ffffff")
    grid.paint_mirrored((3, 0, 0), accent, axes="x")
    if grid.occupied_count != filled:
        fail("paint_live", "paint_mirrored recolours; it must not add or remove cells")
    if grid.get((3, 0, 0)) != accent:
        fail("paint_live", "paint_mirrored must recolour the cell it was given")
    if grid.get((-4, 0, 0)) != accent:
        fail("paint_live", "the mirror of cell 3 is cell -4 (-1 - x), not -3")
    if grid.get((-3, 0, 0)) == accent:
        fail("paint_live", "cell -3 is NOT the mirror of 3 and must be untouched")

    doomed = np.array([(x, 4, 0) for x in range(-4, 5)], dtype=np.int32)
    grid.erase_many(doomed)
    if grid.occupied_count != filled - len(doomed):
        fail("paint_live", "erase_many must clear exactly the cells it was given")

    # The level stack: add_level subdivides, drop_level takes the finest away.
    # drop_level is False when only one level is left, which is how a host
    # knows to grey out the control rather than by counting.
    coarse = grid.occupied_count
    grid.add_level()
    if not grid.drop_level():
        fail("paint_live", "a stack with two levels must drop the finest")
    if grid.occupied_count != coarse:
        fail("paint_live", "dropping the level just added must restore the solid")
    if grid.drop_level():
        fail("paint_live", "dropping the last remaining level must report False")

    # A build-plane pick: the ray is intersected with a horizontal plane, so a
    # ray with no vertical component cannot hit it and returns None. A host
    # that does not handle None will crash the first time the camera is level.
    cell = grid.build_plane_pick((0.12, 1.0, 0.07), (0.0, -1.0, 0.0), plane_cell=0)
    if cell is None or cell[1] != 0:
        fail("paint_live", f"build plane pick should land on plane 0, got {cell}")
    if grid.build_plane_pick((0.0, 1.0, 0.0), (1.0, 0.0, 0.0)) is not None:
        fail("paint_live", "a ray parallel to the plane must return None, not a cell")

    print(f"  8. palette, mirrored recolour, erase_many, level stack, plane pick "
          f"at {cell}")


def sculpt_the_mesh(mesh):
    """The mesh side: a sculptor holds a ray tree over the vertices, and the
    tree does not rebuild itself as a stroke moves them.

    Measured rather than taken from the docstring, which says raycast "reports
    the surface as it was when the tree was built" — it does not. After a
    stamp the hit follows the moved surface, but through a tree whose BOUNDS
    are stale, so the hit DRIFTS off the ray: casting straight down the Y axis
    returns x,z of ~6e-4 instead of 0. `refresh` rebuilds and puts it back to
    ~1e-9. For a brush that is invisible; for a host aligning a gizmo or
    snapping to a picked point it is the whole error budget, so refresh between
    strokes and not during one.
    """
    sculptor = clay.MeshSculptor(mesh)
    origin, direction = (0.0, 2.0, 0.0), (0.0, -1.0, 0.0)

    first = sculptor.raycast(origin, direction)
    if first is None:
        fail("sculpt_the_mesh", "the ray should hit the form")

    sculptor.stamp("draw", center=first["position"], radius=0.35, strength=0.12)
    during = sculptor.raycast(origin, direction)
    if during is None:
        fail("sculpt_the_mesh", "the ray should still hit after a stamp")
    if during["position"][1] <= first["position"][1]:
        fail("sculpt_the_mesh", "the draw stamp should have raised the surface")

    drift_before = max(abs(during["position"][0]), abs(during["position"][2]))
    sculptor.refresh()
    after = sculptor.raycast(origin, direction)
    drift_after = max(abs(after["position"][0]), abs(after["position"][2]))

    if drift_before <= drift_after:
        fail("sculpt_the_mesh",
             f"refresh must reduce the off-axis drift of a hit "
             f"({drift_before:.2e} -> {drift_after:.2e})")
    if drift_after > 1e-6:
        fail("sculpt_the_mesh",
             f"a refreshed tree should hit on the ray, drift {drift_after:.2e}")

    # A cage that has not been dragged is the identity, and a host uses that to
    # skip the deformation entirely rather than paying for a no-op.
    cage = clay.Lattice(((-1, -1, -1), (1, 1, 1)))
    if not cage.is_identity:
        fail("sculpt_the_mesh", "an untouched lattice must report itself the identity")

    print(f"  9. sculptor hit drift {drift_before:.1e} before refresh, "
          f"{drift_after:.1e} after; untouched lattice is the identity")


# ---------------------------------------------------------------------------
# 6. Save and reload
# ---------------------------------------------------------------------------

def save_and_reload(doc):
    """What a host owes the user at the end. The check is the FIELD, not the
    file size or the layer count: a round trip that keeps the structure and
    changes the surface is the failure worth catching.
    """
    probes = np.random.default_rng(11).uniform(-1.5, 1.5, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)

    with tempfile.TemporaryDirectory() as tmp:
        path = str(pathlib.Path(tmp) / "session.clayspace")
        doc.save(path)
        back = clay.load(path)
        if not np.array_equal(before, back.eval(probes)):
            fail("save_and_reload", "the reloaded document evaluates differently")

    print(f"  10. saved and reloaded: {len(probes)} probes identical")


# ---------------------------------------------------------------------------
# The sequencing surface this file is responsible for
# ---------------------------------------------------------------------------

# Order-dependent entry points: each means nothing on its own and everything in
# sequence, which is why no gallery page reaches them. A curated claim, NOT
# `dir()` — the point is that these are the ORDERING surface, not everything
# the module binds.
SEQUENCING_API = [
    "begin_undo_group", "end_undo_group", "undo_enabled", "redo_depth", "trim_stroke",
    "move_layer", "remove_layer", "remove_mask", "layer_protection",
    "set_layer_protection", "drop_level",
    "build_plane_pick", "snap_to_surface", "selection_bounds", "contains",
    "signed_distance", "gradients",
    "paint_cell", "paint_mirrored", "palette_set", "erase_many", "add_point",
    "refresh", "is_identity",
]


def check_coverage():
    """Every name above is actually called here.

    Read from this file's own source, the same shape as
    `15_voxel_verbs_and_repair.py`: a gate that names a surface has to be able
    to fail, or it certifies whatever it happens to spell.
    """
    src = pathlib.Path(__file__).read_text()
    body = src.split("SEQUENCING_API = [")[0]      # the list itself is not a use
    missing = [n for n in SEQUENCING_API
               if not re.search(rf"\.{n}(?![A-Za-z0-9_])", body)]
    if missing:
        fail("coverage", f"named as sequencing API but never called: {missing}")
    print(f"  covered all {len(SEQUENCING_API)} sequencing entry points")


def main():
    print("=== reference host session — the order a host puts the verbs in ===")
    doc, form = open_document()
    pick_the_surface(doc, form)
    mesh = query_the_mesh(doc)
    stroke_with_undo(doc, form)
    trim_a_live_stroke(doc, form)
    manage_layers(doc)
    mask_a_region(doc)
    paint_live(doc)
    sculpt_the_mesh(mesh)
    save_and_reload(doc)
    check_coverage()
    print("reference host: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
