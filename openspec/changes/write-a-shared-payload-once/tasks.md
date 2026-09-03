## 1. The format

- [x] 1.1 A payload table on `Writer`, keyed on POINTER IDENTITY — which is what
      sharing is here, and matches how a shared edit list is deduplicated
- [x] 1.2 A document-wide id, NOT a NodeId: node ids are per-layer and every
      layer numbers from 1, so an id would name a different node elsewhere
- [x] 1.3 The matching table on `Reader`, so the loaded nodes share ONE
      `shared_ptr` rather than one copy each
- [x] 1.4 An id that does not ascend from 1 is REFUSED rather than tolerated: no
      writer emits one
- [x] 1.5 Covers `volume` AND `gate` — the same member type with the same defect

## 2. Compatibility

- [x] 2.1 Minor 17, in both narration blocks, saying what makes it a different
      SHAPE of change from 7/8/11/14/15/16: it rewrites a field rather than
      appending one
- [x] 2.2 Writing at minor 16 restores the per-node shape exactly
- [x] 2.3 A pre-17 build reading a 17 document FAILS on its existing bounds
      checks rather than misreading a length as an id

## 3. Gates

- [x] 3.1 MEASURED, not asserted: 8 placements of one capture, before and after
      DONE. 1,499,457 -> 189,117 bytes, against 187,535 for a single placement,
      so each extra placement costs about 198 bytes
- [x] 3.2 The reload keeps one payload — a re-save is the same size
- [x] 3.3 The minor-16 downgrade produces four unrelated copies of equal size,
      and minor 17 produces one shared object, in one test so the two cannot
      drift apart
- [x] 3.4 Full unit suite green
- [ ] 3.5 CI green
