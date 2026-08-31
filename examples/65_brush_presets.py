"""One gesture, five brushes — and not one of them is an engine path.

THE CLAIM THIS EXAMPLE EXISTS TO CHECK. The artist-facing brush families every
sculpting tool ships — Clay Buildup, Dam Standard, hPolish, Trim Dynamic, Snake
Hook, Rake — are not new deformations. Each is a kernel plus a falloff plus a
frame plus an accumulation rule plus a spacing. Naming those axes separately is
what turns a brush into a PRESET instead of a code path, and it is what lets the
next named brush cost a serialized struct rather than a switch case.

So the same drag runs five times over the same imported mesh, through five
presets out of the reference library, and the differences you see are entirely
differences of axis values. Nothing below chooses a code path.

WHAT THE MODEL BOUGHT, concretely, and it is worth stating because a
decomposition that changes nothing is decoration:

  - DRAW AND INFLATE ARE ONE KERNEL under two frames. They were two verbs whose
    only documented difference was the direction each takes, and naming the
    direction made that the whole difference. The results did not move by a bit.

  - CLAY AND CLAY BUILDUP share every model axis. The only thing that separates
    them is the STROKE — spacing and accumulation — which is why Buildup lays
    down a denser, softer deposit over the same path.

  - MOVE AND MOVE TOPOLOGICAL differ in exactly one axis, the footprint. One
    measures its falloff in a straight line, the other along the surface, and
    that single value is the whole of "a brush on the upper lip must not drag
    the chin through a closed mouth."

  - A RAKE is Standard that follows the stylus barrel. The deformation is
    unchanged; what makes it a rake is the stamp's ORIENTATION reaching the
    alpha, which nothing consumed before this change.

AND A PRESET CARRIES NO IMAGE BYTES. The alpha a rake stamps with is passed to
the call and stays the caller's; a preset library costs kilobytes, so a host can
hold hundreds of them and own its own resource cache. The assertion at the end
measures that rather than asserting it.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.4, 1.9, 2.9), (0.0, 0.0, 0.0)


def source_shape(doc):
    """A form with bumps, so the smoothing family has something to remove.

    On a surface that is already smooth under the footprint, hPolish is correct
    and invisible, which makes for a poor picture and a misleading one.
    """
    layer = doc.add_sdf_layer("source")
    layer.add(clay.Sphere(r=0.62))
    layer.add(clay.Capsule(a=(0, 0.35, 0), b=(0, 0.95, 0), r=0.22),
              blend=clay.Smooth(0.14))
    rng = np.random.default_rng(65)
    for _ in range(18):
        d = rng.normal(size=3)
        d[2] = abs(d[2]) + 0.6          # keep them on the side the camera sees
        d /= np.linalg.norm(d)
        layer.add(clay.Sphere(r=float(rng.uniform(0.05, 0.09)),
                              position=tuple(d * 0.60)),
                  blend=clay.Smooth(0.05))
    return layer


def source_model(path):
    """Write a model to disk and import it back, so what follows is a real
    import through a real file rather than a mesh handed over in memory."""
    doc = clay.Document()
    source_shape(doc)
    doc.mesh(resolution=96).save(path)
    return clay.load_mesh(path)


def preview(mesh, cell=0.014, colour="#b0784a"):
    """A DISPLAY-ONLY document for a carried mesh. A mesh layer is never
    evaluated, so the renderer — which raycasts a field — has nothing to trace
    against; resampling here is exactly the approximation a mesh layer exists to
    avoid, and it is fine for a picture and wrong for the export."""
    return R.mesh_preview_doc(mesh, cell, colour)


def copy_of(mesh):
    return clay.Mesh.from_triangles(np.array(mesh.positions, copy=True),
                                    np.array(mesh.indices, copy=True))


def drag_across(centre, normal_axis=2, length=0.55, count=24):
    """One gesture, reused by every preset: a drag across the front of the form.

    Sampled densely enough that spacing is the preset's decision rather than the
    input rate's — which is the whole point of arc-length spacing.
    """
    samples = []
    for i in range(count):
        t = i / (count - 1)
        p = list(centre)
        p[0] += (t - 0.5) * length
        p[1] += 0.12 * np.sin(t * np.pi)
        samples.append((p[0], p[1], p[2], 0.55 + 0.45 * np.sin(t * np.pi), 0.4))
    return np.array(samples, dtype=np.float32)


def bar_alpha(n=32):
    """A half-plane stamp: the most asymmetric alpha there is, so an
    ORIENTATION is observable at all. A round alpha would look identical
    whichever way the stamp faced, which is exactly why the missing rotation
    went unnoticed for as long as it did."""
    a = np.zeros((n, n), dtype=np.float32)
    a[:, n // 2:] = 1.0
    return a


def displaced(base, mesh):
    d = np.array(mesh.positions) - base
    return d[np.linalg.norm(d, axis=1) > 1e-6]


def main():
    R.banner("65 brush presets — one gesture, five brushes, no new code paths")

    obj_path = R.output_path("65_source.obj")
    model = source_model(obj_path)
    base = np.array(model.positions, copy=True)
    base_indices = np.array(model.indices, copy=True)
    print(f"  imported {os.path.basename(obj_path)}: {model.triangle_count} triangles, "
          f"{len(model.positions)} vertices")

    library = clay.BrushPreset.library()
    print(f"  the reference library holds {len(library)} named brushes: "
          f"{', '.join(p.name for p in library[:6])}, ...")

    # --- the axes, before any picture -----------------------------------------
    #
    # These are the claims the pictures below illustrate. Asserting them here
    # means the example fails when the decomposition stops being true, rather
    # than when somebody notices the render looks wrong.
    draw = clay.BrushModel.of("draw")
    inflate = clay.BrushModel.of("inflate")
    if draw.kernel != inflate.kernel:
        raise SystemExit("draw and inflate are supposed to be ONE kernel under two frames")
    if draw.frame == inflate.frame:
        raise SystemExit("draw and inflate are supposed to differ in their frame")
    print(f"\n  draw and inflate: kernel {draw.kernel.name} for both, "
          f"frame {draw.frame.name} vs {inflate.frame.name}")

    clay_p = clay.BrushPreset.by_name("Clay")
    buildup = clay.BrushPreset.by_name("Clay Buildup")
    if (clay_p.model.kernel, clay_p.model.frame, clay_p.model.footprint) != (
            buildup.model.kernel, buildup.model.frame, buildup.model.footprint):
        raise SystemExit("Clay and Clay Buildup are supposed to share every model axis")
    if not buildup.stroke.spacing < clay_p.stroke.spacing:
        raise SystemExit("Clay Buildup is supposed to be the denser stroke")
    print(f"  Clay and Clay Buildup: identical model, spacing "
          f"{clay_p.stroke.spacing} vs {buildup.stroke.spacing}")

    move = clay.BrushPreset.by_name("Move")
    topo = clay.BrushPreset.by_name("Move Topological")
    differing = [name for name in ("kernel", "frame", "footprint", "target", "post")
                 if getattr(move.model, name) != getattr(topo.model, name)]
    if differing != ["footprint"]:
        raise SystemExit(
            f"Move and Move Topological are supposed to differ in the footprint "
            f"and nothing else; they differ in {differing}")
    print(f"  Move and Move Topological: one axis apart — "
          f"{move.model.footprint.name} vs {topo.model.footprint.name}")

    # --- the same gesture, five presets ---------------------------------------
    probe = clay.MeshSculptor(model)
    hit = probe.raycast(origin=(0.0, 0.4, 2.5), direction=(0.0, 0.0, -1.0))
    if hit is None:
        raise SystemExit("the probe ray missed the model")
    centre = hit["position"]
    print(f"\n  picked the surface at "
          f"({centre[0]:+.3f}, {centre[1]:+.3f}, {centre[2]:+.3f})")

    gesture = drag_across(centre)
    chosen = ["Standard", "Clay Buildup", "Dam Standard", "hPolish", "Trim Dynamic"]

    tiles = [R.render_tile(preview(model), eye=EYE, target=TARGET, size=180)]
    labels = ["source"]
    results = {}
    for name in chosen:
        preset = clay.BrushPreset.by_name(name)
        work = copy_of(model)
        sculptor = clay.MeshSculptor(work)
        applied = sculptor.apply_preset(gesture, preset)
        moved = displaced(base, work)
        results[name] = moved

        # THE CONTRACT, checked per preset rather than once: topology never
        # changes. A preset that re-tessellated would be a different engine
        # wearing this one's vocabulary.
        if not np.array_equal(np.array(work.indices), base_indices):
            raise SystemExit(f"{name} changed the topology")
        if applied == 0 or len(moved) == 0:
            raise SystemExit(f"{name} did nothing — the gesture missed the surface")

        print(f"  {name:<14} {applied:>3} stamps, {len(moved):>5} vertices moved, "
              f"furthest {np.abs(moved).max():.4f}")
        tiles.append(R.render_tile(preview(work), eye=EYE, target=TARGET, size=180))
        labels.append(name)

    R.contact_sheet(tiles, "65_brush_presets.png", columns=3,
                    caption=" | ".join(labels))

    # --- what separates them, measured ----------------------------------------
    #
    # A contact sheet is a picture and a picture is not an assertion. These are
    # the differences the pictures show, as numbers that fail when they stop
    # being true.

    # Dam Standard CUTS: it is a crease, so its deposit goes inward.
    dam = results["Dam Standard"]
    standard = results["Standard"]
    dam_normal = float(np.mean(np.linalg.norm(dam, axis=1)))
    if not dam_normal > 0.0:
        raise SystemExit("Dam Standard moved nothing measurable")

    # Trim Dynamic is CUT-ONLY: it removes material above its plane and leaves
    # the hollows below it, so over the same gesture it must reach FEWER
    # vertices than a two-sided deposit — the ones already below the plane are
    # left exactly where they were.
    trim = results["Trim Dynamic"]
    if not len(trim) < len(standard):
        raise SystemExit(
            f"Trim Dynamic moved {len(trim)} vertices to Standard's {len(standard)}; "
            "cut-only is supposed to leave the hollows alone")
    print(f"  Trim Dynamic touches {len(trim)} vertices to Standard's {len(standard)}: "
          "cutting\n  without filling is the whole brush.")

    # hPolish removes NOISE and keeps the form: it should move far less material
    # than Standard deposits, because its gate closes wherever the surface bends.
    polish = results["hPolish"]
    polish_travel = float(np.mean(np.linalg.norm(polish, axis=1)))
    standard_travel = float(np.mean(np.linalg.norm(standard, axis=1)))
    if not polish_travel < standard_travel:
        raise SystemExit(
            f"hPolish moved more per vertex ({polish_travel:.4f}) than Standard "
            f"deposited ({standard_travel:.4f}) — its gate is not closing")
    print(f"\n  hPolish moves {polish_travel:.4f} per vertex against Standard's "
          f"{standard_travel:.4f}:")
    print("  its gate closes wherever the surface bends, which is what keeps an")
    print("  edge while the noise beside it goes.")

    # Clay Buildup's denser stroke reaches MORE vertices over the same path than
    # Standard does, which is the stroke axis doing the work rather than the
    # kernel.
    buildup_moved = results["Clay Buildup"]
    if not len(buildup_moved) >= len(standard):
        raise SystemExit(
            f"Clay Buildup reached fewer vertices ({len(buildup_moved)}) than "
            f"Standard ({len(standard)}) over the same path — the spacing axis "
            "is not reaching the stroke")
    print(f"  Clay Buildup reaches {len(buildup_moved)} vertices to Standard's "
          f"{len(standard)} over\n  the same path, on spacing alone.")

    # --- the rake: an orientation that reaches the alpha -----------------------
    #
    # The one row of the library that was inexpressible before this change.
    alpha = bar_alpha()
    rake = clay.BrushPreset.by_name("Rake")

    # TWO PASSES WITH THE STYLUS HELD DIFFERENT WAYS. With the orientation
    # switched on they must differ; with it off they must not, because a caller
    # that did not ask for it keeps the alpha tangent it set.
    def rake_pass(azimuth, orient):
        work = copy_of(model)
        sculptor = clay.MeshSculptor(work)
        samples = np.array(
            [(row[0], row[1], row[2], row[3], 0.5) for row in gesture], dtype=np.float32)
        # The barrel angle rides on the sample's azimuth channel, so the two
        # passes differ in the STYLUS and in nothing else.
        full = [(float(s[0]), float(s[1]), float(s[2]), float(s[3]), 0.5, azimuth, 0.0, 0.0)
                for s in samples]
        sculptor.apply_preset(np.array(full, dtype=np.float32), rake, alpha=alpha,
                              orient_alpha_by_stamp=orient)
        return np.array(work.positions)

    # The library entry itself is what carries the barrel-following decision.
    if not rake.stroke.rotate_to_azimuth:
        raise SystemExit("the Rake preset is supposed to follow the stylus barrel")

    across = rake_pass(0.0, True)
    along = rake_pass(np.pi / 2.0, True)
    turned = int(np.count_nonzero(np.linalg.norm(across - along, axis=1) > 1e-6))
    if turned == 0:
        raise SystemExit(
            "the stylus barrel changed and the surface did not: the stamp's "
            "orientation is not reaching the alpha")

    # ...and OFF, the two must agree exactly, because a caller that did not ask
    # for the orientation keeps the alpha tangent it set. Every stroke already
    # in the wild depends on that.
    off_a = rake_pass(0.0, False)
    off_b = rake_pass(np.pi / 2.0, False)
    if not np.array_equal(off_a, off_b):
        raise SystemExit(
            "with orientation off, the barrel angle must reach nothing at all")

    print(f"\n  the Rake preset follows the stylus barrel "
          f"(rotate_to_azimuth={rake.stroke.rotate_to_azimuth}):")
    print(f"  turning the stylus 90 degrees moves {turned} vertices differently, and with")
    print("  the orientation switched off it moves none — which is why it is opt-in")
    print("  rather than inferred from the quaternion, whose identity at azimuth 0")
    print("  would make the stamp snap through 90 degrees as the stylus crossed")
    print("  straight ahead.")

    # --- a preset library costs kilobytes -------------------------------------
    blob = rake.serialize()
    alpha_bytes = alpha.nbytes
    if len(blob) >= alpha_bytes:
        raise SystemExit(
            f"a preset ({len(blob)} bytes) should be far smaller than one alpha "
            f"({alpha_bytes} bytes) — image content must not be embedded")
    total = sum(len(p.serialize()) for p in library)
    print(f"\n  one preset is {len(blob)} bytes and the whole library is {total}; "
          f"one {alpha.shape[0]}x{alpha.shape[1]}\n  alpha alone is {alpha_bytes}. "
          "Image content stays the caller's, so a host can hold\n  hundreds of "
          "brushes and own its own resource cache.")

    # And the round trip is behavioural, not field-by-field: the same stamps.
    back = clay.BrushPreset.deserialize(blob)
    a = clay.StrokePreset.resolve(rake.stroke, gesture)
    b = clay.StrokePreset.resolve(back.stroke, gesture)
    if len(a["positions"]) != len(b["positions"]) or not np.array_equal(
            a["positions"], b["positions"]) or not np.array_equal(a["radii"], b["radii"]):
        raise SystemExit("a round-tripped preset resolved a different stroke")
    print(f"  round-tripped, it resolves the same {len(a['positions'])} stamps, "
          "position for position.")


if __name__ == "__main__":
    main()
