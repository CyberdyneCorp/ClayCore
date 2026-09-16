- [x] Add `mask_threshold` to `voxel::BrushParams`, defaulting to 0.
- [x] Apply it at all three `for_each_brush_cell` / walk sites in
      `src/voxel/sculpt.cpp`, refusing at or above and leaving the weight
      unscaled below.
- [x] Append `mask_threshold` to `clay_brush_params` and read it in the C ABI,
      refusing a value outside [0, 1].
- [x] Bump the three version lines.
- [x] Regressions: the default is bit-exact; a threshold refuses at and above;
      a cell below is written as though unmasked; an out-of-range value is
      refused; the boundary is inclusive as documented.
- [x] Record the measured leak before and after.
