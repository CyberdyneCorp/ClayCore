# Tasks

## 1. The duplicate Lipschitz sweep

- [x] 1.1 Remove `out.set_sample_lipschitz(out.measure_sample_lipschitz())` from both sampling overloads of `field::flatten`, leaving the note that says why sampling already did it
- [x] 1.2 Regression test: a flattened volume's declared Lipschitz equals a measurement of its stored samples

## 2. `FieldVolume::resample_region`

- [x] 2.1 Factor `steepest_in_block` out of `measure_sample_lipschitz` so the resample measures by the same rule
- [x] 2.2 Add `ResampleTally` and `resample_region(const Region&, const BrickBlockFill&)` to `volume.h`, with the precondition stated as `rewrite_region`'s is
- [x] 2.3 Implement: select the bricks meeting the region, fill and classify each with `scan_block`, rebuild `data_`/`index_` compactly, re-derive `far_` signs, rebuild far bounds, raise the Lipschitz floor
- [x] 2.4 Unit tests: a region-limited resample equals the same source resampled whole; empty becomes stored; stored becomes empty; untouched bricks are bit-identical; shared samples agree across brick faces, edges and corners

## 3. Local flatten

- [x] 3.1 `flatten(const FieldVolume&, const FlattenSettings&)` resamples the brush ball rather than sampling `v.bounds()`, with a fill that prefers the volume's stored sample to `eval`
- [x] 3.2 `BrickGrid::sample_cell`, so the fill can ask for a stored sample by global coordinate through the same arithmetic `sample_position` uses
- [x] 3.3 Settings that describe no flatten return the volume unchanged rather than a resampled copy

## 4. Tests

- [x] 4.1 Parity against the exact-source flatten near the facet, for TwoSided, CutOnly and FillOnly
- [x] 4.2 Samples beyond the region and its taper are bit-identical to the input's
- [x] 4.3 A facet lands in bricks that stored nothing; bricks whose surface left the band stop storing
- [x] 4.4 Brush centred inside a brick, on a face, on an edge and on a corner: every copy of a shared sample agrees
- [x] 4.5 Scaling, deterministic: the evaluated-brick count does not move when unrelated model is added
- [x] 4.6 An exact 1-Lipschitz source flattened locally does not declare 14
- [x] 4.7 Strength 0, region radius 0, a zero normal and a fully masked region all leave the volume alone
- [x] 4.8 The existing flatten suite still passes

## 5. Measure and document

- [x] 5.1 Re-run the #300 scaling table before and after; fill in the proposal's Impact
- [x] 5.2 Report stored-brick count and declared Lipschitz before and after
- [x] 5.3 Update `docs/` where flatten's cost or its in-place behaviour is described (07, 05, 09)
