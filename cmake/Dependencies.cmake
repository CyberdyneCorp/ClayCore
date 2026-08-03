# Third-party dependencies — permissive licenses only (MIT/BSD/zlib/Apache-2.0).
# Every FetchContent_Declare here MUST have a matching row in
# THIRD_PARTY_LICENSES.md; tools/check_licenses.py enforces that in CI.
# Pins are validated by the CI "deps-pins" job (CLAY_FETCH_ALL_DEPS=ON).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest
  GIT_TAG v2.5.3)

FetchContent_Declare(meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer
  GIT_TAG v1.2)

FetchContent_Declare(xsimd
  GIT_REPOSITORY https://github.com/xtensor-stack/xsimd
  GIT_TAG 14.3.0)

FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark
  GIT_TAG v1.9.5)

FetchContent_Declare(ufbx
  GIT_REPOSITORY https://github.com/ufbx/ufbx
  GIT_TAG v0.23.0)

if(CLAY_BUILD_TESTS OR CLAY_FETCH_ALL_DEPS)
  FetchContent_MakeAvailable(doctest)
endif()

if(CLAY_BUILD_BENCHMARKS OR CLAY_FETCH_ALL_DEPS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# Consumed from later task groups: xsimd (CPU SIMD path, group 4),
# meshoptimizer (decimation, group 7), ufbx (FBX import, group 9).
if(CLAY_FETCH_ALL_DEPS)
  FetchContent_MakeAvailable(xsimd meshoptimizer)
  # ufbx is a plain source pair with no CMake project of its own.
  FetchContent_Populate(ufbx)
endif()
