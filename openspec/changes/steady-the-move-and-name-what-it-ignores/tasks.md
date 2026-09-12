## 1. The gap

- [x] 1.1 `clay_move_params` reached none of `clay_stroke_preset`'s twelve
      controls, so a host could not offer lazy-mouse on Move
- [x] 1.2 Routing Move through `clay_stroke_resolve` REJECTED: a grab anchors
      its region at press and is one deformation per stroke. Honouring
      `spacing` is what turns one gesture into tens of permanent warps

## 2. The change

- [x] 2.1 `steady` only, appended behind `struct_size`
- [x] 2.2 The other eleven documented beside it with the reason each is absent
- [x] 2.3 `accumulation` called out specifically: it does NOT govern how
      successive gestures compose; `gesture_id` does
- [x] 2.4 Refused, not ignored, on the stateless entry point
- [x] 2.5 Range checked: outside [0, 1) is not a lag

## 3. What building it found

- [x] 3.1 The field was first inserted BEFORE `gesture_id`, which shipped in
      0.107.0. That moves its offset and breaks every caller compiled against
      it — `struct_size` only works by appending. Caught by reading the field
      order back, and now pinned by a test using a descriptor sized to include
      `gesture_id` but not `steady`

## 4. Tests

- [x] 4.1 A lagging drag falls short on one update and CONVERGES when held — a
      lag that never arrived would be a different defect
- [x] 4.2 Zero lag is unchanged
- [x] 4.3 Non-zero lag on the stateless path is refused; zero still passes
- [x] 4.4 1.0 and -0.1 are refused
- [x] 4.5 A truncated descriptor behaves identically, and still carries
      `gesture_id`
- [x] 4.6 Full suite 11/11, c-abi OK, binding parity OK, doc-latency OK

## 5. Still open

- [ ] 5.1 Device gate before any tag carries this
- [ ] 5.2 Snake Hook is a separate question with a different answer: a
      following grab needs a swept warp over the whole polyline, and
      `rotate_along_stroke` is the right home for its Rake parameter
- [ ] 5.3 The Blender evidence is from the bundled manual, not a live session
