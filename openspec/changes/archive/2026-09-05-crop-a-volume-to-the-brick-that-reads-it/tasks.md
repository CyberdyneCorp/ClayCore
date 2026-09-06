## 1. The narrowing

- [x] 1.1 `FieldVolume::cropped(region)`, in the volume's OWN coordinates
- [x] 1.2 The origin and brick grid are PRESERVED — the first version moved the
      origin and the values differed in the last ulp (-0.0598687 against
      -0.0598688), because the kernel reads through `(p - origin) / cell`
- [x] 1.3 Only `data_` (and its parallel `colors_`) is narrowed; `index_` and
      `far_` keep their full grid. Measured: the samples are 99.73% of the
      payload, so this is essentially the whole win at none of the risk
- [x] 1.4 A dropped brick reads as empty at the volume's own far value rather
      than at whatever the slot held
- [x] 1.5 Refuses — keeping the whole volume — when the region reaches every
      stored brick or none

## 2. The emit site

- [x] 2.1 `tape_build` narrows only when compiling against a cull region
- [x] 2.2 The crop region is the cull's OWN test box, already dilated by the
      band and the chain pad, carried into the item's local frame

## 3. Gates

- [x] 3.1 EXACT equality inside the region and within the band, under identity
      AND under a rotate/move/scale placement — the local-frame transform is
      what a world-aligned crop would get wrong
- [x] 3.2 Colour compared alongside distance
- [x] 3.3 The payload assertion is BLOB FLOATS, not a timing: a count cannot be
      blamed on a shared box. 1,243,861 -> 27,160
- [x] 3.4 A whole-document compile still carries the whole volume
- [x] 3.5 Full unit suite green (2,330 cases)
- [ ] 3.6 CI green
