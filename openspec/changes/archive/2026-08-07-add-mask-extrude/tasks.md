# Tasks: add-mask-extrude

- [x] 1.1 `mask_to_field`: EDT over the masked region, stored as a narrow-band
      `FieldVolume` — 1-Lipschitz, so it can enter a field expression
- [x] 1.2 `mask_extrude` on a field: shell of the source intersected with the
      masked region, outward / inward / centred, with a rounded rim
- [x] 1.3 `mask_extrude` on a voxel grid: the same verb in cell space, carrying
      the source palette
- [x] 1.4 Refusals: empty mask, mask that misses the surface, non-positive
      thickness, mask coarser than the requested sampling
- [x] 1.5 C ABI and Python bindings
- [x] 1.6 Tests: thickness measures right, each mode sits on the right side, the
      rim rounds, the two representations agree, a ray still lands, the declared
      Lipschitz holds, and every refusal returns nothing rather than crashing
- [x] 1.7 `examples/25_mask_extrude.py` plus its `CAPABILITY_EXAMPLES` entry;
      docs, roadmap, full verification

Found while building:

- [x] 1.8 `mask_to_field` is public, but the extrude does NOT use it. A
      `FieldVolume` reports a flat BOUND outside its band, and composing that
      into the intersection would bake the seam between bound and distance into
      the extract — the defect flatten's header warns about. The extrude uses
      the dense signed distance directly, which is a real distance everywhere in
      its box; the public conversion samples the same thing into a volume.
- [x] 1.9 A wall thinner than a cell is refused up front. Without that, "no
      sample landed inside" cannot distinguish a mask that missed the surface
      from a plate too thin for the sampling to carry, and the refusal that
      matters would fire for the wrong reason.
- [x] 1.10 Outward does NOT include the seed cells and inward DOES. The seeds
      are the source's own surface cells, half a voxel inside it, so they are
      the first layer of the -t..0 band and none of the 0..t one. Getting this
      backwards is what makes the two representations disagree by a voxel.
