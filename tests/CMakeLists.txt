add_executable(clay_unit_tests
  unit/test_version.cpp
  unit/test_prim3d.cpp
  unit/test_prim2d.cpp
  unit/test_ops.cpp
  unit/test_xform_repeat.cpp
  unit/test_lift_ease.cpp
  unit/test_deform_exactness.cpp
  unit/test_stroke.cpp
  unit/test_field.cpp
  unit/test_math.cpp
  unit/test_scene.cpp
  unit/test_extended_blends.cpp
  unit/test_commands.cpp
  unit/test_parity.cpp
  unit/test_brick.cpp
  unit/test_voxel.cpp
  unit/test_mesh.cpp
  unit/test_meshers.cpp
  unit/test_pick.cpp
  unit/test_io.cpp)

target_link_libraries(clay_unit_tests PRIVATE clay::core doctest::doctest)
clay_apply_warnings(clay_unit_tests)
clay_apply_sanitizers(clay_unit_tests)

add_test(NAME clay_unit_tests COMMAND clay_unit_tests)
