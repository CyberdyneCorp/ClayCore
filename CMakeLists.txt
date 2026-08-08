cmake_minimum_required(VERSION 3.24)

project(claycore
  VERSION 0.22.1
  DESCRIPTION "Portable C++20 SDF + voxel engine library"
  LANGUAGES C CXX)  # C: ufbx

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# static claycore objects also link into libclay_shared (ctypes FFI check)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CLAY_WERROR          "Treat warnings as errors"                    ON)
option(CLAY_BUILD_TESTS     "Build unit tests"                            ON)
option(CLAY_BUILD_BENCHMARKS "Build performance benchmarks"               OFF)
option(CLAY_BUILD_PYTHON    "Build the pyclay nanobind module"            OFF)
option(CLAY_BACKEND_METAL   "Build the Metal backend (Apple platforms)"   OFF)
option(CLAY_BACKEND_CUDA    "Build the CUDA backend"                      OFF)
option(CLAY_BACKEND_OPENCL  "Build the OpenCL backend"                    OFF)
option(CLAY_FETCH_ALL_DEPS  "Fetch every third-party dependency (CI pin validation)" OFF)
set(CLAY_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined (Clang/GCC only)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ClayWarnings)
include(Dependencies)

add_library(claycore
  src/version.cpp
  src/scene/bounds.cpp
  src/scene/curve.cpp
  src/cut/cut.cpp
  src/field/volume.cpp
  src/field/relax.cpp
  src/field/flatten.cpp
  src/field/move_topological.cpp
  src/scene/commands.cpp
  src/scene/tape_build.cpp
  src/brick/cache.cpp
  src/voxel/grid.cpp
  src/voxel/sculpt.cpp
  src/voxel/mask.cpp
  src/voxel/repair.cpp
  src/brush/stroke.cpp
  src/brush/tube.cpp
  src/brush/move.cpp
  src/mesh/marching.cpp
  src/mesh/surface_nets.cpp
  src/mesh/dual_contouring.cpp
  src/mesh/validate.cpp
  src/mesh/decimate.cpp
  src/mesh/bvh.cpp
  src/mesh/to_field.cpp
  src/pick/pick.cpp
  src/io/clayspace.cpp
  src/io/parity_fixture.cpp
  src/io/obj.cpp
  src/io/ply.cpp
  src/io/fbx.cpp
  src/io/gltf.cpp
  ${ufbx_SOURCE_DIR}/ufbx.c
  backends/cpu/cpu_backend.cpp
  bindings/c/clay_c.cpp)

set_source_files_properties(${ufbx_SOURCE_DIR}/ufbx.c PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(claycore SYSTEM PRIVATE ${ufbx_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(claycore PRIVATE Threads::Threads meshoptimizer)
add_library(clay::core ALIAS claycore)

target_include_directories(claycore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_compile_definitions(claycore PRIVATE
  CLAY_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  CLAY_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  CLAY_VERSION_PATCH=${PROJECT_VERSION_PATCH})

clay_apply_warnings(claycore)
clay_apply_sanitizers(claycore)

# The core never throws across module boundaries (errors are result values);
# building without exceptions/RTTI on GCC/Clang enforces that at compile time.
# MSVC keeps its defaults for now (the STL fights /EHs-c-).
if(NOT MSVC)
  # CXX only: CUDA device code needs nvcc's own defaults
  target_compile_options(claycore PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()

# -- Metal backend (Apple platforms) -----------------------------------------
if(CLAY_BACKEND_METAL AND APPLE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_program(XCRUN xcrun REQUIRED)
  file(GLOB CLAY_KERNEL_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/clay/kernel/*.h)
  set(CLAY_METAL_AIR ${CMAKE_CURRENT_BINARY_DIR}/clay_kernels.air)
  set(CLAY_METALLIB ${CMAKE_CURRENT_BINARY_DIR}/clay_kernels.metallib)
  set(CLAY_METALLIB_C ${CMAKE_CURRENT_BINARY_DIR}/clay_metallib_embedded.cpp)
  add_custom_command(OUTPUT ${CLAY_METAL_AIR}
    COMMAND ${XCRUN} -sdk macosx metal -c
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/clay_kernels.metal
            -I ${CMAKE_CURRENT_SOURCE_DIR}/include
            -I ${CMAKE_CURRENT_SOURCE_DIR}/backends/metal
            -fno-fast-math -o ${CLAY_METAL_AIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/clay_kernels.metal
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/metal/metal_shared.h
            ${CLAY_KERNEL_HEADERS}
    COMMENT "Compiling claycore kernels to MSL (single-source)")
  add_custom_command(OUTPUT ${CLAY_METALLIB}
    COMMAND ${XCRUN} -sdk macosx metallib ${CLAY_METAL_AIR} -o ${CLAY_METALLIB}
    DEPENDS ${CLAY_METAL_AIR})
  add_custom_command(OUTPUT ${CLAY_METALLIB_C}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py
            ${CLAY_METALLIB} ${CLAY_METALLIB_C} clay_metallib
    DEPENDS ${CLAY_METALLIB} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py)

  set_source_files_properties(${CLAY_METALLIB_C} PROPERTIES COMPILE_OPTIONS "-w")
  target_sources(claycore PRIVATE backends/metal/metal_backend.cpp ${CLAY_METALLIB_C})
  target_compile_definitions(claycore PRIVATE CLAY_HAS_METAL=1)
  target_include_directories(claycore SYSTEM PRIVATE ${metalcpp_SOURCE_DIR})
  target_include_directories(claycore PRIVATE backends/metal)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(claycore PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()

# -- CUDA backend (tier 2) ---------------------------------------------------
if(CLAY_BACKEND_CUDA)
  # only an explicit -DCMAKE_CUDA_ARCHITECTURES survives here; enable_language
  # below sets the variable to its own default, which is not a user request
  set(_clay_cuda_arch "${CMAKE_CUDA_ARCHITECTURES}")
  enable_language(CUDA)
  # the host .cpp is compiled by the C++ compiler and needs cuda_runtime.h
  # plus the runtime library; CUDA::cudart carries both
  find_package(CUDAToolkit REQUIRED)
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  if(NOT _clay_cuda_arch)
    include(ClayCudaArch)
    clay_select_cuda_arch(_clay_cuda_arch
      NATIVE "${CMAKE_CUDA_ARCHITECTURES_NATIVE}"
      SUPPORTED ${CMAKE_CUDA_ARCHITECTURES_ALL})
    clay_cuda_arch_is_detected(_clay_gpu_detected "${CMAKE_CUDA_ARCHITECTURES_NATIVE}")
    if(NOT _clay_gpu_detected)
      message(STATUS "CUDA: no GPU detected; building ${_clay_cuda_arch} PTX "
                     "for the driver to JIT")
    elseif(NOT _clay_cuda_arch STREQUAL CMAKE_CUDA_ARCHITECTURES_NATIVE)
      message(STATUS "CUDA: GPU arch ${CMAKE_CUDA_ARCHITECTURES_NATIVE} is newer than this "
                     "toolkit; building ${_clay_cuda_arch} PTX for the driver to JIT")
    endif()
  endif()
  # claycore is created before enable_language(CUDA), so its CUDA_ARCHITECTURES
  # property was initialized empty — set it on the target, not the variable
  set_target_properties(claycore PROPERTIES CUDA_ARCHITECTURES "${_clay_cuda_arch}")
  target_sources(claycore PRIVATE backends/cuda/clay_kernels.cu backends/cuda/cuda_backend.cpp)
  target_compile_definitions(claycore PRIVATE CLAY_HAS_CUDA=1)
  target_include_directories(claycore PRIVATE backends/cuda)
  # parity requires IEEE arithmetic: no fast-math, no FMA contraction
  set_source_files_properties(backends/cuda/clay_kernels.cu PROPERTIES
    COMPILE_OPTIONS "-fmad=false")
  set_target_properties(claycore PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  target_link_libraries(claycore PRIVATE CUDA::cudart)
endif()

# -- OpenCL backend (tier 3, best-effort) ------------------------------------
if(CLAY_BACKEND_OPENCL)
  find_package(OpenCL REQUIRED)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  file(GLOB CLAY_KERNEL_HEADERS_CL ${CMAKE_CURRENT_SOURCE_DIR}/include/clay/kernel/*.h)
  set(CLAY_CL_EMBED ${CMAKE_CURRENT_BINARY_DIR}/clay_cl_source_embedded.cpp)
  add_custom_command(OUTPUT ${CLAY_CL_EMBED}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate_cl.py
            clay/kernel/tape.h ${CLAY_CL_EMBED} clay_cl_source
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/opencl/clay_kernels.cl.in
    DEPENDS ${CLAY_KERNEL_HEADERS_CL}
            ${CMAKE_CURRENT_SOURCE_DIR}/backends/opencl/clay_kernels.cl.in
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/amalgamate_cl.py
    COMMENT "Amalgamating claycore kernels for OpenCL (single-source)")
  set_source_files_properties(${CLAY_CL_EMBED} PROPERTIES COMPILE_OPTIONS "-w")
  target_sources(claycore PRIVATE backends/opencl/opencl_backend.cpp ${CLAY_CL_EMBED})
  target_compile_definitions(claycore PRIVATE CLAY_HAS_OPENCL=1)
  target_link_libraries(claycore PRIVATE OpenCL::OpenCL)
  if(APPLE)  # Apple's OpenCL 1.2 headers are deprecated but functional
    set_source_files_properties(backends/opencl/opencl_backend.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-deprecated-declarations")
  endif()
endif()

# -- clay-cli ----------------------------------------------------------------
add_executable(clay_cli tools/clay-cli/main.cpp)
set_target_properties(clay_cli PROPERTIES OUTPUT_NAME clay)
target_link_libraries(clay_cli PRIVATE claycore)
clay_apply_warnings(clay_cli)
clay_apply_sanitizers(clay_cli)

if(CLAY_BUILD_PYTHON)
  if(CLAY_BUILD_TESTS)
    enable_testing()
  endif()
  add_subdirectory(bindings/python)
endif()

if(CLAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)

  # C11 consumer smoke test (c-abi spec)
  add_executable(clay_c_smoke tests/c_api/smoke.c)
  target_include_directories(clay_c_smoke PRIVATE bindings/c)
  set_target_properties(clay_c_smoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_link_libraries(clay_c_smoke PRIVATE claycore)
  clay_apply_sanitizers(clay_c_smoke)
  add_test(NAME clay_c_smoke COMMAND clay_c_smoke)

  add_test(NAME clay_cli_selftest COMMAND clay_cli selftest)

  # shared library for the cross-language FFI check (ctypes)
  add_library(clay_shared SHARED bindings/c/clay_c.cpp)
  target_link_libraries(clay_shared PRIVATE claycore Threads::Threads meshoptimizer)
  clay_apply_sanitizers(clay_shared)
  set_target_properties(clay_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  if(CLAY_BACKEND_METAL AND APPLE)
    target_link_libraries(clay_shared PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
  endif()
endif()

if(CLAY_BUILD_BENCHMARKS)
  add_executable(clay_bench benchmarks/bench_main.cpp)
  target_link_libraries(clay_bench PRIVATE claycore benchmark::benchmark)
  clay_apply_warnings(clay_bench)
endif()
