"""Pressing Stop on the most expensive verb in the library.

This library has three budget classes. The first two are frames — a brush stamp
is 5.8 ms, a drag 0.1 ms — and the third is not:

    mask_extrude      4403 ms  on the reference iPad
    consolidate        661 ms
    volume_relax       358 ms

Every one of those used to be a synchronous call a host entered and could not
leave, and the threading contract closed the obvious workaround: calls on one
handle must be serialized, CONST READERS INCLUDED, so a host could not even
read the document from another thread to draw a progress bar.

A cancellation token fixes both halves. `cancel()` is the one call in the whole
library that is safe from another thread, and the token carries progress the
host polls rather than a callback the engine fires.

This script cancels the 4403 ms one and then proves the thing that makes a
cancel usable: the document is BYTE-IDENTICAL afterwards. A cancel is a
discard, so a host never has to undo one.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.02
EYE, TARGET = (2.2, 1.5, 2.2), (0.0, 0.0, 0.0)


def scene():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.Sphere(r=0.5), color="#c8b28c")
    layer.add(clay.Sphere(r=0.34, position=(0.5, 0.0, 0.0)), color="#c8b28c")
    mask = doc.add_mask("body", cell_size=CELL)
    mask.fill(((-0.6, -0.1, -0.6), (0.6, 0.6, 0.6)), 1.0)
    return doc, layer, mask


def main():
    R.banner("61 stopping a long operation")

    doc, layer, mask = scene()
    R.render(doc, "61_before.png", eye=EYE, target=TARGET,
             caption="a masked region, about to be extruded")

    probes = np.array([[0.0, 0.0, 0.0], [0.5, 0.0, 0.0], [3.0, 3.0, 3.0]], dtype=np.float32)
    before_field = doc.eval(probes)
    before_bytes = doc.to_bytes()

    # --- cancelled -----------------------------------------------------------
    token = clay.CancelToken()
    token.cancel()                       # as if the user pressed Stop
    try:
        doc.mask_extrude(mask, thickness=0.06, cell_size=0.01, token=token)
        raise SystemExit("the extrude should have been cancelled")
    except RuntimeError as e:
        print(f"  cancelled        {e}")

    # The guarantee that makes a cancel usable rather than merely available.
    if doc.to_bytes() != before_bytes:
        raise SystemExit("a cancelled operation must leave the document byte-identical")
    if not np.allclose(doc.eval(probes), before_field):
        raise SystemExit("a cancelled operation must not change the field")
    print("  unchanged        the document is byte-identical — a cancel is a discard, "
          "so you never undo one")

    # A cancel is NOT a failure, and must not read like one. The distinction
    # matters because "nothing to extrude" is a real and different outcome.
    try:
        doc.mask_extrude(mask, thickness=0.0001, cell_size=0.01)
        raise SystemExit("a wall thinner than a cell should be refused")
    except ValueError as e:
        print(f"  vs a refusal     {str(e)[:56]}... — a different error, deliberately")

    # --- reused, and completed ----------------------------------------------
    token.reset()                        # one token per document, not per press
    if token.cancelled:
        raise SystemExit("reset should clear the flag")
    volume = doc.mask_extrude(mask, thickness=0.06, cell_size=CELL, token=token)
    layer.add(volume, color="#8fa6b8")
    print(f"  completed        the extract is a Volume, {token.progress['running'] and 'running' or 'and the token reads idle'}")

    R.render(doc, "61_after.png", eye=EYE, target=TARGET,
             caption="the same extrude, allowed to finish")

    print("\n  the one call in this library that is safe from another thread.")


if __name__ == "__main__":
    main()
