# Tasks

## 1. The packed scan

- [x] 1.1 `CullIndex::Chain` keeps `probes`, one box per entry, parallel to `entries`
- [x] 1.2 `probe_for` folds `!local || bound.is_infinite()` into an infinite box
- [x] 1.3 `build_chain` and `append` both maintain the array, so an extended index scans what a rebuilt one scans
- [x] 1.4 `plan` picks the scan once, outside the chain loop, and keeps the predicate for an infinite region
- [x] 1.5 the survivors stay in chain order, so nothing downstream sorts

## 2. Proof

- [x] 2.1 a test holds the plan against the predicate over an empty region, an infinite one, a local one and an empty-handed one
- [x] 2.2 the document carries the three items whose geometry bound is EMPTY — a strokeless stroke, a boneless armature, a payload-less volume — which is the case the fold would get wrong
- [x] 2.3 forcing the packed path fails that test at 16 survivors against 13, and the forced build compiles
- [x] 2.4 the existing byte-identical corpus and the append/rebuild equality tests pass unchanged

## 3. The fixture that can show a slope

- [x] 3.1 `spread_grid` — dabs at a fixed spacing over a growing sheet
- [x] 3.2 `BM_CullPlanLocal{10000,50000}` time `plan` alone, index built outside the loop, survivors counted
- [x] 3.3 `check_bench.py` gates the 50 000 row absolutely and against its 10 000 sibling
- [x] 3.4 the roadmap records what the fixture changed about reading the earlier measurement
