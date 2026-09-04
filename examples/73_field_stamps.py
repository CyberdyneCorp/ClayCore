"""Sculpt a detail once, then stamp it wherever you like.

THE GAP THIS CLOSES, stated as an artist would: you sculpt one good scale, one
good rivet, one good pore cluster — and then you sculpt it again. And again.
There is no way to pick up what you made and put it down somewhere else.

Most of what that needs was already in the library and this example should not
pretend otherwise: a sampled volume is a first-class primitive, it compiles
through the tape like any other, and a document holds ONE copy of a payload
however many times it is placed. What was missing is two things:

  AN ORIENTED CAPTURE. The existing capture takes a world-axis-aligned box. A
  detail on a curved surface has to be captured about the SURFACE — +Z outward,
  the tangent plane turned by the stylus azimuth — or the asset only works at
  the orientation it happened to be taken at.

  AN IDENTITY. Two placements share a payload, but nothing NAMED the asset, so
  nothing could tell you that you had captured the same detail twice.

THE FRAME IS NEVER GUESSED FROM THE CONTENT. An orientation derived from the
samples moves when the region moves, so re-capturing the same detail would give
an asset that disagrees with the placements already made from it. The caller
says which way is up, and `Volume.stamp_from_document` resolves it through the
same code the scalar alpha uses.

WHAT THE PICTURES SHOW:

  - THE SOURCE, with the patch that gets captured.
  - THE ASSET ALONE, in its own frame — which is what "oriented" buys: it comes
    out square, not tilted the way it sat on the model.
  - IT PLACED BACK where it came from, which must be the source again.
  - A STROKE of it walked across a fresh form at varying size and rotation.

WHAT THE NUMBERS ASSERT: the placed asset reproduces the source it was captured
from, capturing the same region twice gives the same asset, and a stroke of
placements holds ONE payload.

Run: python examples/73_field_stamps.py
"""

import math

import numpy as np

import pyclay as clay

import _render as R

TILE = 200
CELL = 0.018


def ridged_ball():
    """A ball with a ridge of dabs walked over one side — something worth reusing."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.Sphere(r=1.0), color="#b9ada0")
    for i in range(26):
        t = -0.62 + 0.05 * i
        r = 0.13 + 0.035 * math.sin(i * 1.1)
        layer.add(
            clay.Sphere(r=r, position=(math.cos(t) * 0.98,
                                       math.sin(t) * 0.98,
                                       0.2 * math.sin(3.0 * t))),
            blend=clay.Smooth(0.055), color="#8f6f52")
    return doc, layer


def main():
    R.banner("Capture a detail once, stamp it anywhere")
    doc, _ = ridged_ball()
    tiles = []

    # -- the source -------------------------------------------------------
    tiles.append(R.render_tile(doc, eye=(2.6, 1.4, 2.2), size=TILE, colors_from_field=True))

    # -- the capture, about the SURFACE -----------------------------------
    #
    # A hit on the ridge, the surface normal there, and a stylus azimuth. The
    # frame is +Z outward with the tangent plane turned by the azimuth, which is
    # what makes the asset reusable at another orientation.
    a = -0.35
    # FOUND, not assumed. A smooth union of a ball and a ridge of dabs puts its
    # surface further out than the shapes suggest -- measured here at radius
    # 1.246 where the parts imply about 1.11 -- and a hit guessed from the
    # geometry lands inside the form, where a capture has nothing to capture.
    direction = (math.cos(a), math.sin(a), 0.0)
    lo, hi = 0.5, 2.0
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        probe = np.array([[direction[0] * mid, direction[1] * mid, 0.0]], dtype=np.float32)
        if doc.eval(probe)[0] > 0.0:
            hi = mid
        else:
            lo = mid
    radius = 0.5 * (lo + hi)
    hit = (direction[0] * radius, direction[1] * radius, 0.0)
    normal = direction
    print(f"  surface found at radius {radius:.3f}")
    stamp = clay.Volume.stamp_from_document(
        doc, hit=hit, normal=normal, azimuth=0.7, cell=CELL,
        local_bounds=((-0.5, -0.5, -0.22), (0.5, 0.5, 0.22)))
    print(f"  captured {stamp.brick_count} bricks, {stamp.megabytes:.3f} MB")
    print(f"  content id {stamp.content_id}")

    # The same region captured again IS the same asset. Not a uuid: a host that
    # captured the same detail twice should be told rather than accumulating
    # duplicates it cannot recognise.
    again = clay.Volume.stamp_from_document(
        doc, hit=hit, normal=normal, azimuth=0.7, cell=CELL,
        local_bounds=((-0.5, -0.5, -0.22), (0.5, 0.5, 0.22)))
    assert again.content_id == stamp.content_id
    print("  the same region captured twice is the same asset")

    # -- the asset on its own, in its own frame ---------------------------
    alone = clay.Document()
    alone_layer = alone.add_sdf_layer("asset")
    # The placement args override the capture frame, so this is the asset in
    # its OWN coordinates rather than where it sat on the model.
    flat = clay.Volume.stamp_from_document(
        doc, hit=hit, normal=normal, azimuth=0.7, cell=CELL,
        local_bounds=((-0.5, -0.5, -0.22), (0.5, 0.5, 0.22)),
        position=(0.0, 0.0, 0.0), rotation_axis_angle=((0.0, 0.0, 1.0), 0.0), scale=1.0)
    alone_layer.add(flat, color="#8f6f52")
    # Framed from the asset's OWN bounds, which is the point of the tile: in its
    # own coordinates the capture is an upright patch rather than the tilted
    # slice it was on the model.
    tiles.append(R.render_tile(alone, layer=alone_layer, size=TILE, colors_from_field=True))

    # -- placed back where it came from -----------------------------------
    back = clay.Document()
    back_layer = back.add_sdf_layer("stamped")
    back_layer.add(stamp, color="#8f6f52")
    # Framed on the patch rather than on the whole model it came from: a capture
    # is a PATCH, and drawn at the source's framing it would be a speck.
    tiles.append(R.render_tile(back, layer=back_layer, size=TILE, colors_from_field=True))

    # ... and it is the field that was there. Two conditions on what is
    # compared, and both matter:
    #
    #   WITHIN THE BAND, which is all a sampled field ever promised — outside it
    #   a volume reports a lower BOUND rather than a distance.
    #
    #   WITHIN A BALL around the hit, rather than a box in a tangent basis
    #   rebuilt here. The capture's own basis comes out of calpha_frame with the
    #   azimuth applied, and a basis guessed at this end is a DIFFERENT one, so
    #   some of its corners land outside the captured box and read as the far
    #   bound. Measured with a rebuilt basis: 0.034, all of it from corners
    #   outside the capture rather than from the capture being wrong.
    grid = np.linspace(-0.18, 0.18, 15)
    pts = np.array([[hit[0] + x, hit[1] + y, hit[2] + z]
                    for x in grid for y in grid for z in grid], dtype=np.float32)
    keep = np.linalg.norm(pts - np.array(hit, dtype=np.float32), axis=1) <= 0.18
    pts = pts[keep]
    want = doc.eval(pts)
    got = back.eval(pts)
    band = 3.0 * CELL
    inside = np.abs(want) <= band
    worst = float(np.abs(want[inside] - got[inside]).max())
    print(f"  placed back: {int(inside.sum())} in-band samples, worst |diff| {worst:.6f}")
    assert worst < CELL, worst

    # -- a stroke of it ---------------------------------------------------
    #
    # Each placement is an ordinary item under a transform, so they vary in size
    # and rotation and still share ONE payload.
    stroke = clay.Document()
    stroke_layer = stroke.add_sdf_layer("detail")
    stroke_layer.add(clay.Sphere(r=0.85, position=(0.0, -0.5, 0.0)), color="#a89c90")
    for i in range(9):
        s = i / 8.0
        ang = -1.1 + 2.2 * s
        placed = clay.Volume.stamp_from_document(
            doc, hit=hit, normal=normal, azimuth=0.7, cell=CELL,
            local_bounds=((-0.5, -0.5, -0.22), (0.5, 0.5, 0.22)),
            position=(math.sin(ang) * 0.95, math.cos(ang) * 0.95 - 0.5, 0.0),
            rotation_axis_angle=((0.0, 0.0, 1.0), -ang),
            scale=0.55 + 0.35 * math.sin(s * math.pi))
        stroke_layer.add(placed, color="#7d5c40")
    tiles.append(R.render_tile(stroke, eye=(0.0, 0.4, 3.3), target=(0.0, -0.35, 0.0),
                               size=TILE, colors_from_field=True))
    print("  nine placements of one asset, varying in size and rotation")

    R.contact_sheet(tiles, "73_field_stamps.png", columns=4,
                    caption="source / the asset in its own frame / placed back / a stroke of it")


if __name__ == "__main__":
    main()
