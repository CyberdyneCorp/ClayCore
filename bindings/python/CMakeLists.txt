# pyclay — nanobind extension module (python-bindings spec). Built only when
# CLAY_BUILD_PYTHON=ON (scikit-build-core turns it on for wheels).

find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)

FetchContent_MakeAvailable(nanobind)  # declared in cmake/Dependencies.cmake

# STABLE_ABI targets abi3 on CPython >= 3.12 (matches wheel.py-api in
# pyproject.toml); older interpreters silently get a version-specific build.
nanobind_add_module(pyclay STABLE_ABI pyclay_module.cpp)
target_link_libraries(pyclay PRIVATE clay::core)
clay_apply_warnings(pyclay)

# nanobind's own headers trip -Wshadow on GCC (clang is quiet). Mark them
# SYSTEM and, since the interface include still reaches the command line as
# -I on some generators, silence shadow for this one thin binding TU.
get_target_property(_nanobind_includes nanobind-static INTERFACE_INCLUDE_DIRECTORIES)
if(_nanobind_includes)
  set_target_properties(nanobind-static PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nanobind_includes}")
endif()
if(NOT MSVC)
  target_compile_options(pyclay PRIVATE -Wno-shadow)
endif()

if(SKBUILD)
  install(TARGETS pyclay LIBRARY DESTINATION .)
endif()

if(CLAY_BUILD_TESTS)
  add_test(NAME pyclay_pytest
    COMMAND ${Python_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/tests -q)
  set_tests_properties(pyclay_pytest PROPERTIES
    ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:pyclay>")
endif()
