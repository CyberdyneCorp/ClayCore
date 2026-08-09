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

# Whether a CMAKE_CUDA_ARCHITECTURES value is a real user request or just
# enable_language(CUDA)'s own default read back out of the cache.
#
# enable_language writes its default into the CACHE, so from the SECOND
# configure of a build directory onwards the variable is always set and a bare
# `if(NOT CMAKE_CUDA_ARCHITECTURES)` can no longer tell a user's -D from that
# default. The auto-detection then silently stops running and the build drops
# to the toolkit default arch — which still loads, because the default is built
# with PTX the driver can JIT, so nothing fails loudly. Comparing against the
# default recorded on the first configure is what keeps the two apart.
#
#   clay_cuda_arch_is_user_request(<out_var> CURRENT <value> DEFAULT <default>)
#
# DEFAULT is empty when none has been recorded yet — a first configure, or a
# build directory whose first configure already carried an explicit -D. Any
# non-empty CURRENT is a user request in that case.
function(clay_cuda_arch_is_user_request out_var)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "CURRENT;DEFAULT" "")

  if(NOT arg_CURRENT)
    set(${out_var} FALSE PARENT_SCOPE)
  elseif(arg_DEFAULT AND arg_CURRENT STREQUAL arg_DEFAULT)
    set(${out_var} FALSE PARENT_SCOPE)
  else()
    set(${out_var} TRUE PARENT_SCOPE)
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
