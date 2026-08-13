# Tasks: a colour channel on FieldVolume

## 1. Storage

- [x] 1.1 `FieldVolume` gains an optional packed-RGB8 array parallel to
      `data_`, present or absent as a whole.
- [x] 1.2 `sample()` accepts an optional colour callable; a volume built
      without one carries no colour and costs nothing extra.
- [x] 1.3 Accessors: does this volume carry colour, and what is the colour at a
      point (trilinear, matching the distance's rule).
- [x] 1.4 `to_blob` / `from_blob` carry the section, since that is what the
      tape uploads.

## 2. The tape and the backends

- [x] 2.1 The volume blob header gains a colour offset; zero means none.
- [x] 2.2 `ctape_volume` writes an optional colour out-parameter, trilinear
      over the same eight samples as the distance.
- [x] 2.3 The call site applies it, and every other prim is untouched.
- [x] 2.4 `check_kernel_dialect.py` passes for CPU, CUDA and Metal profiles
      plus the OpenCL and Vulkan amalgamations.
- [x] 2.5 The parity suite compares COLOUR for a coloured volume on every
      registered backend, not only distance.
- [x] 2.6 Re-package the kernels artifact and note the host-visible change in
      `docs/06-host-gpu-previews.md`.

## 3. The format

- [x] 3.1 `FieldVolume::serialize` / `deserialize` carry the section.
- [x] 3.2 `kSceneMinor` and `kClaySpaceMinor` move to 9 together.
- [x] 3.3 A minor-8 document opens and its volumes read as uncoloured.
- [x] 3.4 A minor-9 document is refused by an older reader.
- [x] 3.5 The format notes at the top of `io/clayspace.h` record what minor 9
      carries.

## 4. The producers

- [x] 4.1 `consolidate` writes per-item colour into the baked volume.
- [x] 4.2 The consolidation bit-identity gate re-baselined deliberately, in
      this change, with the reason.
- [x] 4.3 `VoxelGrid::to_field` writes the palette per sample.
- [x] 4.4 `clay_voxel_to_layer` produces one item; the header states the change
      in what a host counts.
- [x] 4.5 Converting a single palette index still works.

## 5. Tests

- [x] 5.1 A coloured volume evaluates its own colour, interpolated.
- [x] 5.2 An uncoloured volume is bit-identical to before, distance and colour.
- [x] 5.3 Colour survives a blob, a serialize round trip AND a document save
      and load; a pre-colour blob reads as uncoloured, and writing at minor 8
      drops only the colour.
- [x] 5.4 A two-colour layer consolidates to a two-colour volume.
- [x] 5.5 Paint still overrides a coloured volume.
- [x] 5.6 Outside the sampled box, the item's colour applies.
- [x] 5.7 Memory: a coloured volume is about twice an uncoloured one and an
      uncoloured one has not grown.

## 6. Docs

- [x] 6.1 `docs/RELEASE.md`: the format minor, the tape change, and the
      host-visible recompile, called out rather than buried.
- [x] 6.2 The gallery shows a consolidated layer keeping its colours, which is
      the change a user sees first.
