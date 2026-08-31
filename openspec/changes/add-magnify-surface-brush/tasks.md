## 1. The resolver

- [x] 1.1 `brush::magnify_brush` resolves a world centre, radius and signed
      strength into one `magnify` per contributing item, in that item's frame
- [x] 1.2 `prepare_magnify` / `resolve_prepared_magnify` split it the way a live
      gesture needs, reusing `PreparedMove` — the prepared half is about a BALL
      over a layer and does not know what the gesture will do with it
- [x] 1.3 The strength crosses every symmetry image untouched, a reflection of a
      radial scale being a radial scale of equal strength

## 2. The chain rule

- [x] 2.1 `continues_gesture` matches by KIND as well as by centre and radius
- [x] 2.2 `order_by_value` leaves move.cpp's anonymous namespace so both
      resolvers share one ordering rule

## 3. The bindings

- [x] 3.1 `clay_magnify_params`, `clay_layer_magnify_surface` and its `_preview`
- [x] 3.2 The apply half factored out of `clay_layer_move_surface` and shared,
      so the reach, the frontier and the undo group have one implementation
- [x] 3.3 `Layer.magnify_surface` and `Layer.magnify_surface_preview`

## 4. Proof

- [x] 4.1 A blended form swells as ONE surface, symmetric about the gesture
- [x] 4.2 The same deformation on one picked item is NOT that — the measurement
      issue #391 asks for
- [x] 4.3 The sign is Magnify versus Pinch, in both directions
- [x] 4.4 The centre is world space, and a layer transform is mapped through
- [x] 4.5 A live gesture replaces its own last frame, and the result is the same
      document a single frame at the final strength produces
- [x] 4.6 A pinch does not swallow a drag's grab over the same ball
- [x] 4.7 The prepared path reproduces the one-shot path bit for bit
- [x] 4.8 Under a mirror, a gesture and its mirror image agree bit for bit, an
      item reached only through its copy is warped, and a straddler's deformers
      come out in the same order whichever side asked
- [x] 4.9 The same claims at the C boundary, including the refusals and preview
- [x] 4.10 The same claims from pyclay

## 5. Documentation

- [x] 5.1 `docs/07-brushes-and-features.md`: the resolver, what it shares with
      Move, what it resolves by less, and why pose is not here
