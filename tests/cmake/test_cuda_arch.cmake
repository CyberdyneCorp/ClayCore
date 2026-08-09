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

# Regression: the auto-detection has to survive a RE-configure. Every gate that
# reuses a build directory — tools/release_check.py re-runs `cmake -S -B` on the
# one it is given — hits this path, and it failed silently: enable_language(CUDA)
# writes its default into the cache, the old `if(NOT CMAKE_CUDA_ARCHITECTURES)`
# guard read that back as a user request, and the second configure built the
# toolkit default (sm_52 cubin+PTX) instead of the detected 90-virtual. Nothing
# errored, because compute_52 PTX still JITs on an sm_120 device.
function(expect_user_request label expected)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "CURRENT;DEFAULT" "")
  clay_cuda_arch_is_user_request(actual CURRENT "${arg_CURRENT}" DEFAULT "${arg_DEFAULT}")
  if(actual STREQUAL expected)
    message(STATUS "ok   — ${label}: ${actual}")
  else()
    message(SEND_ERROR "FAIL — ${label}: expected '${expected}', got '${actual}'")
  endif()
endfunction()

# First configure: nothing in the cache yet, so nothing to honour.
expect_user_request("first configure, no -D" FALSE CURRENT "" DEFAULT "")

# Re-configure with no -D: the cache holds enable_language's recorded default,
# which must NOT suppress the auto-detection. This is the regression.
expect_user_request("re-configure, cache holds the default" FALSE
  CURRENT "52" DEFAULT "52")

# A real -D still wins, on the first configure and on every one after it.
expect_user_request("explicit -D on a fresh build dir" TRUE CURRENT "80-real" DEFAULT "")
expect_user_request("explicit -D differing from the default" TRUE
  CURRENT "90-virtual" DEFAULT "52")

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
