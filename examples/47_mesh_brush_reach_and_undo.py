"""The three things a mesh brush needs before it is usable on a real model.

`45` has the primitives and `46` the compositions. None of that is workable on
an actual asset without these:

**Reach measured along the surface.** A brush is a ball in space, and a model
folds. The inside of one prong of a fork is a centimetre from the inside of the
other and a long way from it *along the surface* — press on one and the other
must not move. That is the Move Topological rule, and this example is the case
where getting it wrong is obvious.

**A mask.** The same stroke over a half-frozen region moves the free half and
leaves the other bit-identical. One rule — each vertex's weight is scaled by
`1 - mask` — reaches all eleven verbs with no per-verb code, exactly as it does
for the voxel verbs.

**Undo.** A mesh stroke is destructive vertex displacement, not an edit item, so
it cannot undo through the document's command stack the way an SDF stroke does.
`VertexDeltas` records the vertices a gesture actually reached — sparse, and
coalesced, so one stroke is one step — and puts them back bit-exactly.
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (1.35, 1.30, 1.85), (0.0, 0.30, 0.0)
# Straight into the gap: a bulge on either inner face shows as the gap closing.
GAP_EYE, GAP_TARGET = (0.30, 0.55, 1.55), (0.0, 0.42, 0.0)


def fork(cell=0.014):
    """Two prongs a narrow gap apart, joined at the bottom: a closed mouth,
    with the geometry that makes the point visible from any angle.

    The inside of the left prong is close to the inside of the right one in
    SPACE and far from it along the SURFACE — you have to go down one prong,
    round the base and up the other."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("fork")
    layer.add(clay.Capsule(a=(-0.26, -0.35, 0), b=(0.26, -0.35, 0), r=0.14))
    layer.add(clay.Capsule(a=(-0.26, -0.35, 0), b=(-0.26, 0.85, 0), r=0.14),
              blend=clay.Smooth(0.10))
    layer.add(clay.Capsule(a=(0.26, -0.35, 0), b=(0.26, 0.85, 0), r=0.14),
              blend=clay.Smooth(0.10))
    return doc.mesh(voxel_size=cell)


def preview(mesh, cell=0.012, colour="#b0784a"):
    """Display only: a mesh layer is never evaluated, so the renderer has
    nothing to trace until something resamples it. See 36_mesh_layers."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def copy_of(mesh):
    return clay.Mesh.from_triangles(np.array(mesh.positions, copy=True),
                                    np.array(mesh.indices, copy=True))


def main():
    R.banner("47 mesh brushes — reach along the surface, masks, and undo")

    model = fork()
    base = np.array(model.positions, copy=True)
    print(f"  fork: {model.triangle_count} triangles, {len(model.positions)} vertices; "
          f"the prongs are {0.52 - 2 * 0.14:.2f} apart")

    # --- reach along the surface ---------------------------------------------
    # Press on the INSIDE of the left prong, hard, with a radius that crosses
    # the gap. The right prong is well inside that radius in space.
    probe = clay.MeshSculptor(model)
    hit = probe.raycast(origin=(0.0, 0.45, 0.0), direction=(-1.0, 0.0, 0.0))
    if hit is None:
        raise SystemExit("the probe ray missed the inside of the left prong")
    centre, seed = hit["position"], hit["seed_class"]
    print(f"  pressing on the inside of the left prong at x={centre[0]:+.3f}")

    def press(geodesic):
        work = copy_of(model)
        clay.MeshSculptor(work).stamp("draw", center=centre, radius=0.35, strength=0.9,
                                      seed_class=seed, geodesic=geodesic)
        moved = np.linalg.norm(np.array(work.positions) - base, axis=1) > 1e-5
        return work, moved

    walked, walked_moved = press(True)
    straight, straight_moved = press(False)

    right_prong = base[:, 0] > 0.12
    print(f"  straight-line reach: {int(straight_moved.sum())} vertices moved, "
          f"{int((straight_moved & right_prong).sum())} of them on the OTHER prong")
    print(f"  along the surface:   {int(walked_moved.sum())} vertices moved, "
          f"{int((walked_moved & right_prong).sum())} of them on the OTHER prong")
    if (walked_moved & right_prong).any():
        raise SystemExit("the surface walk crossed the gap")

    R.side_by_side(
        R.render_array(preview(walked), eye=GAP_EYE, target=GAP_TARGET, width=250, height=250),
        R.render_array(preview(straight), eye=GAP_EYE, target=GAP_TARGET, width=250, height=250),
        "47_reach.png",
        caption="the same press, measured along the surface (left) and in a straight "
                "line — the straight one dents both prongs")

    # --- a mask freezes half of it -------------------------------------------
    # One stroke down the front of the left prong, with its TOP HALF frozen. The
    # mask is world-addressed, so it is painted where the geometry is rather
    # than in anybody's cells — see 11_masks.
    FREEZE_ABOVE = 0.50
    mask = clay.MaskField(cell_size=0.02)
    mask.paint((-0.26, 0.85, 0.0), size=34, shape="cube", falloff="constant", strength=1.0)
    print(f"\n  froze the top of the left prong (above y={FREEZE_ABOVE:.2f}): "
          f"{mask.painted_count} mask cells")

    preset = clay.StrokePreset(radius=0.16, strength=0.55, spacing=0.12)
    samples = np.array([[-0.26, 0.82 - 0.06 * i, 0.13] for i in range(11)], dtype=np.float32)

    masked = None
    for verb in ("draw", "smooth"):
        work = copy_of(model)
        applied = clay.MeshSculptor(work).apply_stroke(samples, preset, verb, mask=mask)
        moved = np.linalg.norm(np.array(work.positions) - base, axis=1) > 0
        frozen = (base[:, 1] > FREEZE_ABOVE + 0.06) & (base[:, 0] < 0.0)
        print(f"  {verb:<7} {applied:>2} stamps: {int(moved.sum()):>4} vertices moved, "
              f"{int((moved & frozen).sum())} of them under the mask")
        if (moved & frozen).any():
            raise SystemExit(f"the mask did not hold for {verb}")
        if verb == "draw":
            masked = work

    unmasked = copy_of(model)
    clay.MeshSculptor(unmasked).apply_stroke(samples, preset, "draw")
    R.side_by_side(
        R.render_array(preview(masked), eye=EYE, target=TARGET, width=250, height=250),
        R.render_array(preview(unmasked), eye=EYE, target=TARGET, width=250, height=250),
        "47_masked_stroke.png",
        caption="one stroke down the left prong with its top half frozen (left), "
                "and the same stroke with no mask")

    # --- undo -----------------------------------------------------------------
    work = copy_of(model)
    sculptor = clay.MeshSculptor(work)
    deltas = clay.VertexDeltas()
    drag = np.array([[-0.26, 0.90 + 0.05 * i, 0.0] for i in range(12)], dtype=np.float32)
    pull_preset = clay.StrokePreset(radius=0.20, strength=1.0, spacing=0.3)
    applied = sculptor.apply_stroke(drag, pull_preset, "snakehook", deltas=deltas)
    before_undo = R.render_array(preview(work), eye=EYE, target=TARGET, width=250, height=250)
    print(f"\n  pulled a tendril off the left prong: {applied} stamps, "
          f"{deltas.vertex_count} vertices recorded out of {len(base)}")

    deltas.revert(sculptor)
    exact = np.array_equal(np.array(work.positions), base)
    print(f"  reverted: bit-identical to the pre-stroke mesh: {exact}")
    if not exact:
        raise SystemExit("the revert was not exact")

    R.side_by_side(
        before_undo,
        R.render_array(preview(work), eye=EYE, target=TARGET, width=250, height=250),
        "47_undo.png",
        caption="a snakehook pull, and the same mesh after reverting its vertex deltas — "
                "byte for byte the model it started as")


if __name__ == "__main__":
    main()
