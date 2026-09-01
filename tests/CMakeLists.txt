add_executable(clay_unit_tests
  unit/test_version.cpp
  unit/test_prim3d.cpp
  unit/test_prim2d.cpp
  unit/test_ops.cpp
  unit/test_suffix_tape.cpp
  unit/test_layer_prefix_tape.cpp
  unit/test_tape_block.cpp
  unit/test_checkpoint_stack_levels.cpp
  unit/test_tape_prefix_reuse.cpp
  unit/test_xform_repeat.cpp
  unit/test_lift_ease.cpp
  unit/test_deform_exactness.cpp
  unit/test_stroke.cpp
  unit/test_field.cpp
  unit/test_math.cpp
  unit/test_scene.cpp
  unit/test_cull_index.cpp
  unit/test_extended_blends.cpp
  unit/test_commands.cpp
  unit/test_parity.cpp
  unit/test_backend_partial.cpp
  unit/test_thread_pool.cpp
  unit/test_backend_residency.cpp
  unit/test_points_batch.cpp
  unit/test_parity_fixture.cpp
  unit/test_brick.cpp
  unit/test_alpha_stamp.cpp
  unit/test_sculpt_layers.cpp
  unit/test_voxel.cpp
  unit/test_voxel_mesh_fixture.cpp
  unit/test_voxel_incremental_mesh.cpp
  unit/test_voxel_mesh_smooth.cpp
  unit/test_voxel_to_field.cpp
  unit/test_voxel_rasterize_mesh.cpp
  unit/test_volume_color.cpp
  unit/test_mask.cpp
  unit/test_stroke_engine.cpp
  unit/test_mesh.cpp
  unit/test_lattice_gizmo.cpp
  unit/test_mesh_lattice.cpp
  unit/test_mesh_sculpt.cpp
  unit/test_mesh_sculpt_parity.cpp
  unit/test_sculpt_kernels.cpp
  unit/test_dynamic_surface.cpp
  unit/test_topology_ops.cpp
  unit/test_topology_delta.cpp
  unit/test_dynamic_bvh.cpp
  unit/test_remesh_local.cpp
  unit/test_dynamic_sculpt.cpp
  unit/test_dynamic_history.cpp
  unit/test_c_dynamic_topology.cpp
  unit/test_dynamic_scale.cpp
  unit/test_subdivide.cpp
  unit/test_surface_frame.cpp
  unit/test_detail_field.cpp
  unit/test_multires.cpp
  unit/test_multires_sculpt.cpp
  unit/test_multires_dirty.cpp
  unit/test_surface_chunks.cpp
  unit/test_chunk_transport.cpp
  unit/test_memory_trim.cpp
  unit/test_preflight_budget.cpp
  unit/test_scratch_arena.cpp
  unit/test_memory_profile.cpp
  unit/test_extreme_poly_scaling.cpp
  unit/test_extreme_poly_exactness.cpp
  unit/test_mesh_project.cpp
  unit/test_multires_history.cpp
  unit/test_multires_io.cpp
  unit/test_c_multires.cpp
  unit/test_c_surface_chunks.cpp
  unit/test_c_maintenance.cpp
  unit/test_mesh_sculpt_layers.cpp
  unit/test_mesh_sculpt_layer_stroke.cpp
  unit/test_mesh_sculpt_layer_history.cpp
  unit/test_c_sculpt_layers.cpp
  unit/test_sculpt_allocation.cpp
  unit/test_brush_arena.cpp
  unit/test_stamp_frame.cpp
  unit/test_dynamic_shared_brush_parity.cpp
  unit/test_multires_shared_brush_parity.cpp
  unit/test_c_shared_brush_runtime.cpp
  unit/test_shared_brush_determinism.cpp
  unit/test_brush_preset.cpp
  unit/test_c_brush_preset.cpp
  unit/test_mesh_deform.cpp
  unit/test_mesh_transfer.cpp
  unit/test_mesh_weld.cpp
  unit/test_voxel_remesh.cpp
  unit/test_c_voxel_remesh.cpp
  unit/test_bench_spread.cpp
  unit/test_bvh_refit.cpp
  unit/test_meshers.cpp
  unit/test_pick.cpp
  unit/test_io.cpp
  unit/test_gltf.cpp
  unit/test_deformers.cpp
  unit/test_lifts.cpp
  unit/test_repetition.cpp
  unit/test_primitives_backfill.cpp
  unit/test_c_item_builder.cpp
  unit/test_c_layer_enum.cpp
  unit/test_c_layer_nodes.cpp
  unit/test_c_node_readback.cpp
  unit/test_c_transform_args.cpp
  unit/test_scale_axes.cpp
  unit/test_c_scale_axes.cpp
  unit/test_c_layer_scale_axes.cpp
  unit/test_c_layer_rename.cpp
  unit/test_c_layer_mirror.cpp
  unit/test_c_layer_radial.cpp
  unit/test_c_voxel.cpp
  unit/test_c_voxel_incremental.cpp
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
  unit/test_swept.cpp
  unit/test_c_swept.cpp
  unit/test_volume.cpp
  unit/test_mesh_import.cpp
  unit/test_c_volume.cpp
  unit/test_relax.cpp
  unit/test_sdf_sculpt.cpp
  unit/test_sdf_prefix_cache.cpp
  unit/test_flatten.cpp
  unit/test_snakehook.cpp
  unit/test_magnify.cpp
  unit/test_noise.cpp
  unit/test_magnify_brush.cpp
  unit/test_move_brush.cpp
  unit/test_move_topological.cpp
  unit/test_c_move_brush.cpp
  unit/test_c_sdf_sculpt.cpp
  unit/test_relief.cpp
  unit/test_mask_brush.cpp
  unit/test_masked_combine.cpp
  unit/test_mask_extrude.cpp
  unit/test_c_mask_brush.cpp
  unit/test_c_pick_eval.cpp
  unit/test_c_eval_excluding.cpp
  unit/test_c_tape_cache.cpp
  unit/test_c_frontier_resume.cpp
  unit/test_c_prefetch_seed.cpp
  unit/test_c_brick.cpp
  unit/test_c_undo_bound.cpp
  unit/test_c_out_descriptors.cpp
  unit/test_c_brick_atlas.cpp
  unit/test_c_brick_lod_mesh.cpp
  unit/test_c_mesh_copy.cpp
  unit/test_c_mesh_validation.cpp
  unit/test_c_memory_io.cpp
  unit/test_session_history.cpp
  unit/test_session_journal.cpp
  unit/test_cancellation.cpp
  unit/test_mask_history.cpp
  unit/test_history_budget.cpp
  unit/test_surface_groups.cpp
  unit/test_surface_groups_visibility.cpp
  unit/test_c_surface_groups.cpp
  unit/test_stroke_input.cpp
  unit/test_procedural_mask.cpp
  unit/test_surface_measure.cpp
  unit/test_c_field_queries.cpp
  unit/test_sculpt_handoff.cpp
  unit/test_c_undo_spans.cpp
  unit/test_document_memory.cpp
  unit/test_document_memory_cpp.cpp
  unit/test_c_tape_export.cpp
  unit/test_c_host_volume_path.cpp
  unit/test_armature.cpp
  unit/test_c_armature.cpp
  unit/test_voxel_levels.cpp
  unit/test_c_voxel_levels.cpp
  unit/test_consolidate.cpp
  unit/test_c_consolidate.cpp
  unit/test_groups.cpp
  unit/test_cull_pad_stability.cpp
  unit/test_node_reach_bound.cpp
  unit/test_mesh_layers.cpp
  unit/test_quad_mesh.cpp
  unit/test_c_quad_mesh.cpp
  unit/test_c_mesh_layer.cpp
  unit/test_c_instance_layers.cpp
  unit/test_c_mesh_sculpt.cpp
  unit/test_grid_batch_device.cpp
  unit/test_vulkan_dispatch.cpp)

# The dispatch-splitting header is backend-internal; the test that keeps it
# honest is built everywhere, so the include path is unconditional.
target_include_directories(clay_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../backends/vulkan)

# Device-dependent Vulkan cases only exist where the backend was built. They
# skip themselves when no runtime is present, so this is about linking the
# backend-internal upload counter, not about having a GPU.
if(CLAY_BACKEND_VULKAN)
  target_sources(clay_unit_tests PRIVATE unit/test_vulkan.cpp unit/test_vulkan_interop.cpp)
  # The interop cases create a Vulkan device the way a HOST would and lend it
  # to claycore, so this file needs the Vulkan headers directly.
  target_link_libraries(clay_unit_tests PRIVATE Vulkan::Vulkan)
endif()

# The Metal interop cases create a Metal device the way a HOST would and lend
# it to claycore, so this file needs metal-cpp directly. The NS/MTL private
# implementations live in the backend's own translation unit inside claycore;
# this file only declares them, so there is no duplicate-symbol hazard. Skips
# itself when no Metal device is present.
if(CLAY_BACKEND_METAL AND APPLE)
  target_sources(clay_unit_tests PRIVATE unit/test_metal_interop.cpp)
  target_include_directories(clay_unit_tests SYSTEM PRIVATE ${metalcpp_SOURCE_DIR})
  # The tape-residency cases reach the backend only through the registry and
  # its two upload counters, so they need no Metal headers of their own.
  target_sources(clay_unit_tests PRIVATE unit/test_metal_tape.cpp)
endif()

target_link_libraries(clay_unit_tests PRIVATE clay::core doctest::doctest)
# the C ABI tests consume the public header as a C consumer would
target_include_directories(clay_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../bindings/c)
# The benchmark spread is shared with tests/unit/test_bench_spread.cpp, which
# gates the property the scattered-material benchmarks depend on (#196).
target_include_directories(clay_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks)
clay_apply_warnings(clay_unit_tests)
clay_apply_sanitizers(clay_unit_tests)

add_test(NAME clay_unit_tests COMMAND clay_unit_tests)

# CUDA architecture selection is pure CMake, so it is tested by script rather
# than by the C++ suite — and needs neither a CUDA toolkit nor a GPU to run
add_test(NAME clay_cuda_arch_selection
  COMMAND ${CMAKE_COMMAND} -DCLAY_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../cmake
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_cuda_arch.cmake)
