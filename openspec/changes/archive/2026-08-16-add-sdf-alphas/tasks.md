# Tasks: add-sdf-alphas

- [x] 1.1 DECIDE the orientation contract: explicit tangent, or derived from the direction
      — DECIDED: an EXPLICIT tangent, re-orthogonalised. A derived-only frame cannot
      express "align this stitch to the seam", and a stamp that cannot be rotated is
      one an artist has to bake the rotation into. The tangent is projected into the
      stamp's plane rather than used raw, so a rough "up" works; one parallel to the
      direction falls back to an axis derived from the direction — deterministic for a
      given normal, so a stamp does not spin when the camera lines up with the surface.
- [x] 1.2 `calpha_offset` in the kernel dialect: bilinear lookup into a blob-carried stamp, under the region falloff `cblob_offset` uses
- [x] 1.3 `cdeform_alpha` opcode, samples in the blob as bend curve and lattice do
      — with a five-float header (w, h, extent, radius, amplitude) ahead of the samples.
      The record is exactly full with the handle, the centre and the frame, and
      dropping the frame to make room would have cost the tangent decided in 1.1.
- [x] 1.4 `cfi_alpha`: the bound from adjacent-sample DIFFERENCES over the texel size, plus the falloff gradient term — not from sample magnitudes
- [x] 1.5 `Deformer::alpha` on the scene side, with the stamp copied into the item
- [x] 1.6 Placement helper in the brush engine, so a stamp from a surface hit does not need the host to rebuild the frame
- [x] 1.7 C ABI: its own entry point (`clay_item_add_alpha`), samples copied; pyclay surface
- [x] 1.8 Tests: an all-zero stamp is EXACTLY the identity; a constant stamp costs no step scale; outside the radius the field is untouched exactly; a steep stamp lowers the step scale and raymarching it does not overshoot; the stamp survives serialization
      — the "constant stamp costs no step scale" claim was CORRECTED while testing: the
      region's own falloff still charges, exactly as it does for `blob`, so what is true
      is that the STAMP contributes nothing. The spec scenario was rewritten to the
      measurable claim rather than the test to the wrong one.
- [x] 1.9 Dialect parity: the new kernel compiles and agrees on CPU, Metal, CUDA, OpenCL and Vulkan
- [x] 1.10 Example with a render, and the docs' Alphas row stops saying "voxel side only"
