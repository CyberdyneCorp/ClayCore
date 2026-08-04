"""Repetition: finite grids, infinite grids and radial arrays.

The interesting part is not that copies appear, it is what the library knows
about them. A finite grid still has a finite bound and can be culled; an
infinite grid reports infinite influence and is never culled. This example
prints both so the difference is visible, not just asserted.
"""

import numpy as np

import pyclay as clay

import _render as R


def main():
    R.banner("04 repetition — arrays and their bounds")

    # Finite grid: copies only within the cell range.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.28).repeat_grid(spacing=0.9, counts=(2, 1, 1)))
    print(f"  finite grid bounds: {layer.bounds()}")
    eye, target = R.layer_camera(layer)
    R.render(doc, "04_repeat_grid_finite.png", eye=eye, target=target,
             caption="finite grid, 5 x 3 x 3 cells")

    # Radial array: N copies about the Y axis at a radius.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.RoundBox(size=(0.35, 0.5, 0.18), r=0.06).repeat_radial(count=9, offset=1.1),
              blend=clay.Smooth(0.05))
    print(f"  radial array bounds: {layer.bounds()}")
    eye, target = R.layer_camera(layer, elevation=38.0)
    R.render(doc, "04_repeat_radial.png", eye=eye, target=target,
             caption="9 copies about the axis")

    # Infinite grid: unbounded, so it needs an explicit camera. The repeated
    # element is blended with a slab so the render has something to end on.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.32).repeat_grid(spacing=0.85))
    layer.add(clay.Box(size=(4.0, 4.0, 4.0)), op=clay.Op.INTERSECT)
    R.render(doc, "04_repeat_grid_infinite.png", eye=(3.4, 2.6, 4.2),
             target=(0, 0, 0), caption="infinite grid clipped by an intersect")

    # The bound really is infinite — that is what stops culling dropping it.
    infinite = clay.Document()
    inf_layer = infinite.add_sdf_layer("l")
    inf_layer.add(clay.Sphere(r=0.32).repeat_grid(spacing=0.85))
    far = infinite.eval(np.array([[40.0, 40.0, 40.0]], dtype=np.float32))[0]
    print(f"  infinite grid still evaluates at (40, 40, 40): d = {far:.3f}")

    # Repetition composes with deformers: the array is of the deformed shape.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.3, 0.9, 0.3)).twist(2.5).repeat_radial(count=7, offset=1.0),
              color="#7fb069")
    eye, target = R.layer_camera(layer, elevation=30.0)
    R.render(doc, "04_repeat_deformed.png", eye=eye, target=target,
             colors_from_field=True, caption="radial array of a twisted box")
    R.export_model(doc, "04_repeat_radial.ply", resolution=64)


if __name__ == "__main__":
    main()
