# Tasks: extend-parity-fixture-coverage

- [x] 1.1 Fixture cases for relief and incise, both directions
- [x] 1.2 A fixture case for the noise deformer
- [x] 1.3 Fixture cases for magnify and pinch, both signs
- [x] 1.4 The probe points must actually reach the new geometry — a case whose
      probes all sit far from its surface asserts nothing
- [x] 1.5 Make the requirement track the kernel set rather than a fixed list
- [x] 1.6 Verify: export stays deterministic, the suite gates the new cases
      against the interpreter and every registered backend, and the count moves

Found while building:

- [x] 1.7 Scoped the requirement as "every combine op, deformer and primitive
      family has a case", which is more than this delivers. Measured by scanning
      the compiled tapes rather than guessing: combine ops are now 16 of 16 —
      relief and incise were the last two missing — but deformers are 5 of 14
      (twist, taper, grab, magnify, noise) and primitives 13 opcodes. Narrowed
      the requirement to the combine ops, which is enforceable now and gated,
      and stated the deformer and primitive gaps in the spec instead of implying
      coverage the fixture does not have.
- [x] 1.8 The first sizing of the new cases reached too few probes to mean
      anything: magnify moved 2 of 32 and relief 6. Both were tuned against a
      measurement — relief's region to 17 of 32, magnify's to 14 — while
      deliberately leaving the rest untouched, because the untouched probes are
      what exercise the FINITE SUPPORT. A case covering every probe could not
      catch a support error.
