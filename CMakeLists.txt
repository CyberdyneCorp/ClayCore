cmake_minimum_required(VERSION 3.24)

project(claycore
  VERSION 0.1.0
  DESCRIPTION "Portable C++20 SDF + voxel engine library"
  LANGUAGES CXX)

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
  src/scene/tape_build.cpp)
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

# Backend hosts land in later task groups (4, 12, 13); the presets and
# options exist now so consumers/CI configure against a stable interface.
foreach(_backend METAL CUDA OPENCL)
  if(CLAY_BACKEND_${_backend})
    message(STATUS "claycore: ${_backend} backend requested — host not implemented yet (see openspec add-claycore-v1 tasks)")
  endif()
endforeach()

if(CLAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
