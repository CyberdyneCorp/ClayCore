# Tasks: fix-cuda-arch-selection

- [x] 1.1 Extract the architecture choice into `cmake/ClayCudaArch.cmake` as a pure function
- [x] 1.2 Apply the result to the `claycore` target instead of `CMAKE_CUDA_ARCHITECTURES`
- [x] 1.3 Capture an explicit `-DCMAKE_CUDA_ARCHITECTURES` before `enable_language(CUDA)` so it still wins
- [x] 1.4 Regression test `tests/cmake/test_cuda_arch.cmake`, registered as `clay_cuda_arch_selection`
- [x] 1.5 Document the fallback in the README build section
- [x] 1.6 Verify `cpu-only`, `cuda`, and `opencl` presets configure, build, and pass
