# Regression test for clay_select_cuda_arch (cmake/ClayCudaArch.cmake).
#
# A GPU newer than the CUDA toolkit used to leave the claycore target's
# CUDA_ARCHITECTURES empty, so configuring the `cuda` preset died with
# "CUDA_ARCHITECTURES is empty for target claycore" and the backend could not
# be built or validated at all. Run with:
#
#   cmake -DCLAY_CMAKE_DIR=<repo>/cmake -P tests/cmake/test_cuda_arch.cmake
#
# Needs no CUDA toolkit and no GPU: the selection is a pure function of the
# native arch and the toolkit's supported list.

cmake_minimum_required(VERSION 3.24)
list(APPEND CMAKE_MODULE_PATH "${CLAY_CMAKE_DIR}")
include(ClayCudaArch)

set(_failures 0)

function(expect_arch label expected)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "NATIVE" "SUPPORTED")
  clay_select_cuda_arch(actual NATIVE "${arg_NATIVE}" SUPPORTED ${arg_SUPPORTED})
  if(actual STREQUAL expected)
    message(STATUS "ok   — ${label}: ${actual}")
  else()
    message(SEND_ERROR "FAIL — ${label}: expected '${expected}', got '${actual}'")
  endif()
endfunction()

# CUDA 12.0 (tops out at 90) against an RTX 5060 (sm_120): the regression.
# Must not come back empty — falls back to the newest known arch as PTX.
expect_arch("GPU newer than toolkit" "90-virtual"
  NATIVE "120-real"
  SUPPORTED 50-real 52-real 60-real 70-real 75-real 80-real 86-real 89-real 90)

# The -real/-virtual suffix on the newest entry must not leak into the result.
expect_arch("newest entry carries a suffix" "90-virtual"
  NATIVE "120-real"
  SUPPORTED 80-real 86-real 90-real)

# Toolkit knows the installed GPU: use it directly, no PTX-only fallback.
expect_arch("GPU supported by toolkit" "89-real"
  NATIVE "89-real"
  SUPPORTED 50-real 70-real 80-real 86-real 89-real 90)

# No GPU present — still must produce a buildable arch rather than an empty
# list. CMake fills NATIVE with prose in this case, not an architecture, so the
# non-numeric form has to be handled as well as the empty one.
expect_arch("no GPU detected" "90-virtual"
  NATIVE ""
  SUPPORTED 50-real 80-real 90)

expect_arch("NATIVE holds CMake's no-device message" "90-virtual"
  NATIVE "No CUDA devices found.-real"
  SUPPORTED 50-real 80-real 90)

foreach(native "120-real" "89" "89-virtual")
  clay_cuda_arch_is_detected(detected "${native}")
  if(detected)
    message(STATUS "ok   — '${native}' reads as a detected arch")
  else()
    message(SEND_ERROR "FAIL — '${native}' should read as a detected arch")
  endif()
endforeach()

foreach(native "" "No CUDA devices found." "No CUDA devices found.-real")
  clay_cuda_arch_is_detected(detected "${native}")
  if(detected)
    message(SEND_ERROR "FAIL — '${native}' should not read as a detected arch")
  else()
    message(STATUS "ok   — '${native}' reads as no GPU")
  endif()
endforeach()
