## The fixture

A slab, one solid stroke — constant falloff, full strength, the shape a host
sends when it wants a dab written rather than dithered — across a frozen
half-space whose mask ramps 0 to 1 over six cells. The ramp is the point: a
fully masked cell was never at risk, and the skirt is the whole of the defect.

## Before

    erased unmasked        585
    erased masked          297   (50.8% of unmasked)
    cells at mask >= 0.5   9139
    LEAKED (erased anyway)   25

## After, with `mask_threshold = 0.5`

    erased unmasked        585
    erased gated           282   (48.2% of unmasked)
    cells at mask >= 0.5   9139
    LEAKED                    0

The leak is closed. The gated stroke erases slightly FEWER cells than the
dithered one (282 against 297) rather than more, which is the two halves of the
change working against each other: 25 leaked cells are refused, and the cells
below the threshold are now written unscaled rather than dithered. The totals
being close is a coincidence of this fixture; the distribution is the result.

## Regressions

Three cases in `tests/unit/test_c_mask.cpp`:

- **the leak, and that it is still there at threshold 0.** Leakage is counted
  over a FIXED band — cells the mask calls at least half frozen — so the dimmer
  and the stencil are judged on the same set. An earlier draft counted "at or
  above the threshold", which lets the threshold define its own exam and
  degenerates at 0 to the fully masked cells, which were never the defect. That
  draft passed for the wrong reason and is why the band is fixed.
- **the default is the behaviour that shipped**, compared cell by cell against
  the engine driven with a default-constructed `BrushParams` that cannot see the
  field, with `struct_size` set to the pre-field offset.
- **out of range is refused**, including NaN, with the grid unchanged.

## Mutation

With `mask_allows` reduced to the dimmer, exactly ONE test case in the suite
fails — the new one. Full suite with the fix: 2,784 cases, 17,877,138
assertions. Clean under `-Wshadow -Wall -Wextra -Werror` with GCC.
