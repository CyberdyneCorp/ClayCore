# Tasks

## 1. Make the contract uniform

- [x] 1.1 `clay_brick_config_defaults` probes `struct_size` and fills bounded
- [x] 1.2 `clay_stroke_preset_defaults` likewise
- [x] 1.3 `clay_stroke_preset_deserialize` — the ninth site, found only because
      it delegated its fill rather than matching the grep the first sweep used
- [x] 1.4 Factor the preset fill so defaults and deserialize share one bounded
      write instead of one calling the other

## 2. Catch the next one by construction

- [x] 2.1 A gate in `tools/check_c_abi.py` that walks the public header for
      entry points taking a versioned descriptor by mutable pointer and
      requires a bounded fill in each body
- [x] 2.2 Verify the gate reports every site against `main` and none here — a
      gate that has never fired is not known to work
- [x] 2.3 Use the gate before trusting it, and fix what that turned up: a third
      write spelling (`*out = local`) it did not catch, hiding the ABI's
      largest overrun (`clay_mesh_brush_defaults`, 56 bytes) and a tenth site
      (`clay_mesh_sculptor_raycast`)
- [x] 2.4 Bound the descriptor scan to each struct's own body — a match across
      the whole header called the array-element types descriptors, which would
      have failed the gate on correct code
- [x] 2.5 Update the ctypes exercise, which called defaults with a zeroed
      descriptor, and add the case that omitting `struct_size` is refused

## 3. Move the callers

- [x] 3.1 All in-repo call sites (tests, tools, benchmarks, Swift device tests)
- [x] 3.2 The brick-cache sample in `docs/06-host-gpu-previews.md`

## 4. Say it is breaking

- [x] 4.1 `docs/RELEASE.md` — the third breaking release, and the subtlest,
      since nothing about it is visible at compile time
- [x] 4.2 Header docs on all three entry points name the version and the reason
- [x] 4.3 Bump to ABI 0.35.0 in all three places the release checklist names —
      including `pyproject.toml`, which had drifted to 0.30.0 on main and would
      have failed the release gate
- [x] 4.4 Spec delta stating that a defaults-style call is an output descriptor
      like any other

## 5. Pin it

- [x] 5.1 A descriptor declaring no size is refused, and nothing is written
- [x] 5.2 A size below the original layout is refused
- [x] 5.3 An old host's declared layout is filled and not exceeded
- [x] 5.4 A current host still receives the appended fields
- [x] 5.5 Deserialize covered on all four of those axes
- [x] 5.6 Full suite green; `check_c_abi.py` green against a real shared library
