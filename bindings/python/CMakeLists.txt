# pyclay — nanobind extension module (python-bindings spec). Built only when
# CLAY_BUILD_PYTHON=ON (scikit-build-core turns it on for wheels).

# Development.SABIModule is what makes CMake define Python_SOSABI. Without it
# nanobind's STABLE_ABI silently degrades to a version-locked build: the wheel
# is still tagged abi3, but the module lands as pyclay.cpython-3XX-<plat>.so,
# which no other interpreter will import. Optional so a Python without the
# limited API (PyPy, some distro builds) still configures.
find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module
             OPTIONAL_COMPONENTS Development.SABIModule)

FetchContent_MakeAvailable(nanobind)  # declared in cmake/Dependencies.cmake

# STABLE_ABI targets abi3 on CPython >= 3.12 (matches wheel.py-api in
# pyproject.toml); older interpreters silently get a version-specific build.
nanobind_add_module(pyclay STABLE_ABI pyclay_module.cpp)
target_link_libraries(pyclay PRIVATE clay::core)
clay_apply_warnings(pyclay)

# nanobind's own headers trip -Wshadow on GCC (clang is quiet). Mark them
# SYSTEM and, since the interface include still reaches the command line as
# -I on some generators, silence shadow for this one thin binding TU.
# The runtime target's name depends on the build: a stable-ABI module links
# nanobind-static-abi3, a version-locked one links nanobind-static.
foreach(_nb_target nanobind-static-abi3 nanobind-static)
  if(TARGET ${_nb_target})
    get_target_property(_nanobind_includes ${_nb_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_nanobind_includes)
      set_target_properties(${_nb_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nanobind_includes}")
    endif()
  endif()
endforeach()
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
