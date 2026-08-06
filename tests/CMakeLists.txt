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
  unit/test_mask.cpp
  unit/test_stroke_engine.cpp
  unit/test_mesh.cpp
  unit/test_meshers.cpp
  unit/test_pick.cpp
  unit/test_io.cpp
  unit/test_gltf.cpp
  unit/test_deformers.cpp
  unit/test_lifts.cpp
  unit/test_repetition.cpp
  unit/test_primitives_backfill.cpp
  unit/test_c_item_builder.cpp
  unit/test_c_voxel.cpp
  unit/test_c_mask.cpp
  unit/test_c_stroke.cpp
  unit/test_layer_protection.cpp
  unit/test_curve.cpp
  unit/test_c_curve.cpp
  unit/test_cut.cpp
  unit/test_c_cut.cpp
  unit/test_voxel_repair.cpp
  unit/test_c_voxel_repair.cpp
  unit/test_loft.cpp
  unit/test_c_loft.cpp
  unit/test_c_pick_eval.cpp)

target_link_libraries(clay_unit_tests PRIVATE clay::core doctest::doctest)
# the C ABI tests consume the public header as a C consumer would
target_include_directories(clay_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../bindings/c)
clay_apply_warnings(clay_unit_tests)
clay_apply_sanitizers(clay_unit_tests)

add_test(NAME clay_unit_tests COMMAND clay_unit_tests)

# CUDA architecture selection is pure CMake, so it is tested by script rather
# than by the C++ suite — and needs neither a CUDA toolkit nor a GPU to run
add_test(NAME clay_cuda_arch_selection
  COMMAND ${CMAKE_COMMAND} -DCLAY_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../cmake
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_cuda_arch.cmake)
