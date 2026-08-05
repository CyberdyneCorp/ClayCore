# CUDA architecture selection.
#
# CMake's `native` expands to the installed GPU's architecture, which nvcc
# rejects when the GPU is newer than the toolkit — CMake drops the unsupported
# value and leaves the list empty, so generation dies with the opaque
# "CUDA_ARCHITECTURES is empty for target ...". When that happens, build the
# newest architecture the toolkit does know as PTX only (`-virtual`, no cubin)
# and let the driver JIT it for the actual device.
#
# Kept as a pure function of its inputs so tests/cmake/test_cuda_arch.cmake can
# exercise it without a CUDA toolkit or a GPU.
#
#   clay_select_cuda_arch(<out_var> NATIVE <arch> SUPPORTED <arch>...)
#
# NATIVE is CMAKE_CUDA_ARCHITECTURES_NATIVE, which on a machine without a GPU
# holds a human-readable excuse ("No CUDA devices found.") rather than an
# architecture — anything that is not a number is treated as "no GPU".
#
# clay_cuda_arch_is_detected(<out_var> <native>) reports whether NATIVE names a
# real architecture, so callers can word the fallback message correctly.

function(clay_cuda_arch_is_detected out_var native)
  if(native MATCHES "^[0-9]+(-real|-virtual)?$")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(clay_select_cuda_arch out_var)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "NATIVE" "SUPPORTED")

  if(NOT arg_SUPPORTED)
    message(FATAL_ERROR "clay_select_cuda_arch: SUPPORTED architecture list is empty")
  endif()

  clay_cuda_arch_is_detected(detected "${arg_NATIVE}")
  list(FIND arg_SUPPORTED "${arg_NATIVE}" native_idx)
  if(detected AND native_idx GREATER_EQUAL 0)
    set(${out_var} "${arg_NATIVE}" PARENT_SCOPE)
    return()
  endif()

  list(GET arg_SUPPORTED -1 newest)
  string(REGEX REPLACE "-(real|virtual)$" "" newest "${newest}")
  set(${out_var} "${newest}-virtual" PARENT_SCOPE)
endfunction()
