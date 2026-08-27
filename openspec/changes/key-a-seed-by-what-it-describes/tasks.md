# Tasks

- [x] Establish whether a band mismatch can change a value a caller relies on,
      holding the cull pad still so its gate cannot mask the answer. It can:
      9 of 512 samples, worst 0.105, at a true distance inside the band asked
      for.
- [x] Put the lattice (dims, spacing) and the band in `ResumeKey`, comparing and
      hashing the floats by their bits.
- [x] Drop `spacing`, `dims` and `band` from `ResumeEntry`, and read them off the
      key in `touch_region`.
- [x] Drop `shaped_entry`'s spacing and dims comparisons, now enforced by the
      lookup.
- [x] Regression tests: a seed refused for a wider band and for a narrower one,
      with bit-identical parity against a document that never resumed; a control
      that the same band still resumes; and two caches over one brick coordinate
      both resuming across a stroke.
- [x] Prove each test fails with its fix reverted, and that the revert compiles.
- [x] Update `docs/05-claycore-library.md`.
