# Tasks: add-loft-opcode

- [x] 1.1 `cop_loft` in lift.h, replacing `cop_extrude_to`'s two-profile-only signature
- [x] 1.2 `ctape_loft` opcode bracketing among N profile records in the blob
- [x] 1.3 `PrimType::Loft`; an item carries a profile list with per-profile polygons
- [x] 1.4 Compiler emits it; bounds from the union of the profiles' extents
- [x] 1.5 Exactness: not exact, and a Lipschitz accounting for the axial lerp
- [x] 1.6 Serialization; a loft with fewer than two profiles is refused
- [x] 1.7 Both bindings; parity corpus row so all four backends are verified
- [x] 1.8 Tests: ends match their profiles, middle interpolates, three-profile bracketing, inexact, step scale drops with differing profiles, round trip, existing extrusions bit-identical, refusal, C-vs-Python
- [x] 1.9 Docs, example, ABI 0.18.0, full verification
