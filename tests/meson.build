subdir('bench')
subdir('qemu-iotests')

test_qapi_outputs = [
  'qapi-builtin-types.c',
  'qapi-builtin-types.h',
  'qapi-builtin-visit.c',
  'qapi-builtin-visit.h',
  'test-qapi-commands-sub-sub-module.c',
  'test-qapi-commands-sub-sub-module.h',
  'test-qapi-commands.c',
  'test-qapi-commands.h',
  'test-qapi-emit-events.c',
  'test-qapi-emit-events.h',
  'test-qapi-events-sub-sub-module.c',
  'test-qapi-events-sub-sub-module.h',
  'test-qapi-events.c',
  'test-qapi-events.h',
  'test-qapi-features.c',
  'test-qapi-features.h',
  'test-qapi-init-commands.c',
  'test-qapi-init-commands.h',
  'test-qapi-introspect.c',
  'test-qapi-introspect.h',
  'test-qapi-types-sub-sub-module.c',
  'test-qapi-types-sub-sub-module.h',
  'test-qapi-types.c',
  'test-qapi-types.h',
  'test-qapi-visit-sub-sub-module.c',
  'test-qapi-visit-sub-sub-module.h',
  'test-qapi-visit.c',
  'test-qapi-visit.h',
]

# Problem: to generate trace events, we'd have to add the .trace-events
# file to qapi_trace_events like we do in qapi/meson.build.  Since
# qapi_trace_events is used by trace/meson.build, we'd have to move
# subdir('tests') above subdir('trace') in the top-level meson.build.
# Can't, because it would break the dependency of qga on qemuutil (which
# depends on trace_ss).  Not worth solving now; simply suppress trace
# event generation instead.
test_qapi_files = custom_target('Test QAPI files',
                                output: test_qapi_outputs,
                                input: files('qapi-schema/qapi-schema-test.json',
                                             'qapi-schema/include/sub-module.json',
                                             'qapi-schema/sub-sub-module.json'),
                                command: [ qapi_gen, '-o', meson.current_build_dir(),
                                           '-b', '-p', 'test-', '@INPUT0@',
                                           '--suppress-tracing' ],
                                depend_files: qapi_gen_depends)

# meson doesn't like generated output in other directories
# perhaps change qapi_gen to replace / with _, like Meson itself does?
subdir('include')

test_qapi_sources = []
test_qapi_headers = []
i = 0
foreach o: test_qapi_files.to_list() + test_qapi_files_extra.to_list()
  if o.full_path().endswith('.h')
    test_qapi_headers += o
  endif
  test_qapi_sources += o
endforeach

libtestqapi = static_library('testqapi', sources: [genh, test_qapi_sources])
testqapi = declare_dependency(link_with: libtestqapi, sources: [genh, test_qapi_headers])

test_deps = {
  'test-qht-par': qht_bench,
}

if have_tools and have_vhost_user and host_os == 'linux'
  executable('vhost-user-bridge',
             sources: files('vhost-user-bridge.c'),
             dependencies: [qemuutil, vhost_user])
endif

subdir('decode')

if 'CONFIG_TCG' in config_all_accel
  subdir('fp')
  subdir('tcg/plugins')
endif

subdir('unit')
subdir('qapi-schema')
subdir('qtest')
subdir('migration-stress')
subdir('functional')
subdir('tracetool')

d3d_hle_contracts = [
  ['d3d-hle-abi-classification', files('d3d_hle_abi_classification_contract.py')],
  ['d3d-hle-async-discovery', files('d3d_hle_async_discovery_contract.py')],
  ['d3d-hle-automatic-refuse-split', files('d3d_hle_automatic_refuse_split_contract.py')],
  ['d3d-hle-automatic-readiness', files('d3d_hle_automatic_readiness_contract.py')],
  ['d3d-hle-complete-section-filter', files('d3d_hle_complete_section_filter_contract.py')],
  ['d3d-hle-forza-omit-partial', files('d3d_hle_forza_omit_partial_contract.py')],
  ['d3d-hle-detector-catalog', files('d3d_hle_detector_catalog_contract.py')],
  ['d3d-hle-direct-bootstrap-capability', files('d3d_hle_direct_bootstrap_capability_contract.py')],
  ['d3d-hle-direct-profile-native-dependency', files('d3d_hle_direct_profile_native_dependency_contract.py')],
  ['d3d-hle-kernel-loader-observe', files('d3d_hle_kernel_loader_observe_contract.py')],
  ['d3d-hle-loader-intercept', files('d3d_hle_loader_intercept_contract.py')],
  ['d3d-hle-loader-lifecycle', files('d3d_hle_loader_lifecycle_contract.py')],
  ['d3d-hle-loader-scan-filter', files('d3d_hle_loader_scan_filter_contract.py')],
  ['d3d-hle-mapping-coverage', files('d3d_hle_mapping_coverage_contract.py')],
  ['d3d-hle-plume-session-lifetime', files('d3d_hle_plume_session_lifetime_contract.py')],
  ['d3d-hle-profile-isolation', files('d3d_hle_profile_isolation_contract.py')],
  ['d3d-hle-push-slice', files('d3d_hle_push_slice_contract.py')],
  ['d3d-hle-forza-attach-holes', files('d3d_hle_forza_attach_holes_contract.py')],
  ['d3d-hle-a8l8-texture', files('d3d_hle_a8l8_texture_contract.py')],
  ['d3d-hle-reactivation', files('d3d_hle_reactivation_contract.py')],
  ['d3d-hle-resolution-scale-option',
   files('d3d_hle_resolution_scale_option_contract.py')],
  ['d3d-hle-hook-policy', files('d3d_hle_hook_policy_contract.py')],
  ['d3d-hle-scanout-surface', files('d3d_hle_scanout_surface_contract.py')],
  ['d3d-hle-surface-download', files('d3d_hle_surface_download_contract.py')],
  ['d3d-hle-name', files('d3d_hle_name_contract.py')],
  ['d3d-hle-output-window', files('d3d_hle_output_window_contract.py')],
  ['d3d-hle-palette', files('d3d_hle_palette_contract.py')],
  ['d3d-hle-refuse-set-fixture', files('d3d_hle_refuse_set_fixture.py')],
  ['d3d-hle-session-identity', files('d3d_hle_session_identity_contract.py')],
  ['d3d-hle-session-teardown', files('d3d_hle_session_teardown_contract.py')],
  ['d3d-hle-async-shader-compile', files('d3d_hle_async_shader_compile_contract.py')],
  ['d3d-hle-shader-constant-mode', files('d3d_hle_shader_constant_mode_contract.py')],
  ['d3d-hle-shared-blocker-coverage', files('d3d_hle_shared_blocker_coverage_contract.py')],
  ['d3d-hle-sparse-snapshot', files('d3d_hle_sparse_snapshot_contract.py')],
  ['d3d-hle-spy-census', files('d3d_hle_spy_census_contract.py')],
  ['d3d-hle-stable-image-timeout', files('d3d_hle_stable_image_timeout_contract.py')],
  ['d3d-hle-switch-texture', files('d3d_hle_switch_texture_contract.py')],
  ['d3d-hle-synthetic-heap', files('d3d_hle_synthetic_heap_contract.py')],
  ['d3d-hle-synthetic-mapping', files('d3d_hle_synthetic_mapping_contract.py')],
  ['d3d-hle-tb-flush', files('d3d_hle_tb_flush_contract.py')],
  ['d3d-hle-vblank-identity', files('d3d_hle_vblank_identity_contract.py')],
  ['d3d-hle-registry-walk-tolerant-reads',
   files('d3d_hle_registry_walk_tolerant_reads_contract.py')],
  ['d3d-hle-format-bpp-oracle', files('d3d_hle_format_bpp_oracle_contract.py')],
  ['d3d-hle-lin-a8-texture', files('d3d_hle_lin_a8_texture_contract.py')],
  ['d3d-hle-snapshot-completeness',
   files('d3d_hle_snapshot_completeness_contract.py')],
  ['d3d-hle-unmapped-pointer-attribution',
   files('d3d_hle_unmapped_pointer_attribution_contract.py')],
  ['d3d-hle-draw-index-resolution',
   files('d3d_hle_draw_index_resolution_contract.py')],
  ['d3d-hle-yuv-texture', files('d3d_hle_yuv_texture_contract.py')],
  ['d3d-hle-tile-state', files('d3d_hle_tile_state_contract.py')],
]

foreach contract : d3d_hle_contracts
  test(contract[0], python,
       args: contract[1],
       workdir: meson.project_source_root(),
       suite: 'd3d-hle')
endforeach
