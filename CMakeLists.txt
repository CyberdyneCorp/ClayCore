cmake_minimum_required(VERSION 3.24)

project(claycore
  VERSION 0.1.0
  DESCRIPTION "Portable C++20 SDF + voxel engine library"
  LANGUAGES C CXX)  # C: ufbx

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CLAY_WERROR          "Treat warnings as errors"                    ON)
option(CLAY_BUILD_TESTS     "Build unit tests"                            ON)
option(CLAY_BUILD_BENCHMARKS "Build performance benchmarks"               OFF)
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
  src/scene/commands.cpp
  src/scene/tape_build.cpp
  src/brick/cache.cpp
  src/voxel/grid.cpp
  src/mesh/marching.cpp
  src/mesh/validate.cpp
  src/mesh/decimate.cpp
  src/pick/pick.cpp
  src/io/clayspace.cpp
  src/io/obj.cpp
  src/io/ply.cpp
  src/io/fbx.cpp
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
  target_compile_options(claycore PRIVATE -fno-exceptions -fno-rtti)
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

foreach(_backend CUDA OPENCL)
  if(CLAY_BACKEND_${_backend})
    message(STATUS "claycore: ${_backend} backend requested — host not implemented yet (see openspec add-claycore-v1 tasks)")
  endif()
endforeach()

# -- clay-cli ----------------------------------------------------------------
add_executable(clay_cli tools/clay-cli/main.cpp)
set_target_properties(clay_cli PROPERTIES OUTPUT_NAME clay)
target_link_libraries(clay_cli PRIVATE claycore)
clay_apply_warnings(clay_cli)
clay_apply_sanitizers(clay_cli)

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
