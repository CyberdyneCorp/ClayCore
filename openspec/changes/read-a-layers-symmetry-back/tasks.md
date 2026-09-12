## 1. The gap

- [x] 1.1 `clay_set_layer_mirror` and `clay_set_layer_radial` had no readers,
      while info, transform, composition and protection all do
- [x] 1.2 The absence was load-bearing in ClaySpaceDesktop #109: a cached
      symmetry, an undo the cache never heard about, and a dab through a mirror
      the artist had turned off — 0.28 world units on the far side

## 2. The change

- [x] 2.1 `clay_document_layer_mirror` and `clay_document_layer_radial`
- [x] 2.2 Axes answer 0/1 rather than the internal mask, so a read value writes
      straight back
- [x] 2.3 Every out-pointer optional, so the call validates a layer on its own
- [x] 2.4 A layer with no symmetry answers OFF; a non-SDF layer is refused

## 3. Tests

- [x] 3.1 Round trip: read, write back, read again, unchanged
- [x] 3.2 THE REGRESSION: set a mirror, undo, read — the reader reports what the
      document now carries, which is what a cache could not do
- [x] 3.3 The same for a radial array
- [x] 3.4 A protected layer reads normally and refuses a set
- [x] 3.5 A missing layer is NOT FOUND, a null document INVALID_ARGUMENT
- [x] 3.6 Full suite 11/11, binding parity OK against a freshly built pyclay

## 4. Still open

- [ ] 4.1 Device gate before any tag carries this
- [ ] 4.2 pyclay coverage for the two readers, if the symmetry setters ever gain it
