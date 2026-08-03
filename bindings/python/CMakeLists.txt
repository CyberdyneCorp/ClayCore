# pyclay — nanobind extension module (python-bindings spec). Built only when
# CLAY_BUILD_PYTHON=ON (scikit-build-core turns it on for wheels).

find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)

FetchContent_MakeAvailable(nanobind)  # declared in cmake/Dependencies.cmake

# STABLE_ABI targets abi3 on CPython >= 3.12 (matches wheel.py-api in
# pyproject.toml); older interpreters silently get a version-specific build.
nanobind_add_module(pyclay STABLE_ABI pyclay_module.cpp)
target_link_libraries(pyclay PRIVATE clay::core)
clay_apply_warnings(pyclay)

if(SKBUILD)
  install(TARGETS pyclay LIBRARY DESTINATION .)
endif()

if(CLAY_BUILD_TESTS)
  add_test(NAME pyclay_pytest
    COMMAND ${Python_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/tests -q)
  set_tests_properties(pyclay_pytest PROPERTIES
    ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:pyclay>")
endif()
