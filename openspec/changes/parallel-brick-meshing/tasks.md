# Tasks: mesh bricks in parallel

## 1. The split

- [x] 1.1 Phase one marches each brick into its own `ShellCollector`, over
      `clay::parallel`. Straddlers are recorded after the brick's own cells,
      where the serial loop emitted them.
- [x] 1.2 Phase two replays the recordings through ONE `Builder`, in key order,
      so `edge_vertex` is called in the sequence the serial loop called it.
- [x] 1.3 The straddler map is bound const for the parallel phase, so it is
      visibly a shared read and not a shared write.
- [x] 1.4 The source states why the welding cannot be sharded, since "bricks are
      independent" is true of the march and false of the weld.

## 2. Evidence

- [x] 2.1 Byte-identity against `main` on the same fixture: same vertex,
      triangle and range counts, same hash of positions + indices + ranges
      (`be5c509cc768daea` both sides).
- [x] 2.2 In-tree determinism test: eight runs byte-identical, so no result
      depends on which thread reached a brick first.
- [x] 2.3 In-tree test that the mesh is still watertight, manifold and oriented,
      and that the ranges still partition it — what sharding the weld would
      break.
- [x] 2.4 In-tree test that meshing from INSIDE a pooled loop gives the same
      mesh, so the nesting guard and this fan-out compose.
- [x] 2.5 ThreadSanitizer over the brick, mesh and pool suites: no races
      (verified the runtime was actually linked and instrumented).
- [x] 2.6 asan/ubsan green.
- [x] 2.7 Before/after measured back to back on the same machine, both binaries
      real: 276 bricks 30.9 ms -> 7.46 ms, 80 bricks with gradients 10.9 ms ->
      4.49 ms, an 8-brick dab 1.37 ms -> 0.98 ms.

## 3. Not done, and named

- [ ] 3.1 A DEVICE measurement. Everything above is a 24-core desktop under
      load; #111's claim is about an iPad, and `docs/RELEASE.md` is explicit
      that desktop numbers must never be compared against the device baseline.
      Needs `tools/run_device_bench.sh` and a case for it.
- [ ] 3.2 Straddler collection is still serial. It runs once per subset call and
      was not the measured cost, so it is left rather than folded in silently.
