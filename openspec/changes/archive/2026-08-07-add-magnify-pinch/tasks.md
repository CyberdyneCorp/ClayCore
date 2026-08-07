# Tasks: add-magnify-pinch

- [x] 1.1 `cmagnify_point`: radial scale about a centre, finite support, eased
- [x] 1.2 `cdeform_magnify` opcode and the scene factory; one signed strength for both
- [x] 1.3 `cfi_magnify`: the stretch a radial scale costs, against the easing slope
- [x] 1.4 `sculpt_magnify` for voxels, as the inverse of `sculpt_pinch`
- [x] 1.5 C ABI and Python bindings
- [x] 1.6 Tests: it swells and it gathers, support really is finite, the declared
      Lipschitz holds, a ray still lands, and the two representations agree
- [x] 1.7 Docs, example, full verification

Found while building:

- [x] 1.8 The centre of a radial scale is its FIXED POINT, which is not obvious
      and caught the tests twice — once on the SDF side and once on the voxel
      side, in the same shape. A probe running straight through the centre
      measures the one place the deformation cannot move, so both tests read
      "no effect" from a deformer that was working. The SDF tests now scale
      about the shape's own centre and check the fixed point deliberately; the
      voxel test measures the mean radius from the brush rather than the reach
      along the axis through it.
- [x] 1.9 The voxel verbs share one walk with the step reversed, rather than
      being written twice. That is what "magnify is the inverse of pinch" has to
      mean in code if the two are to stay inverses as either changes.
