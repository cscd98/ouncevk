# Copyright © 2022 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT
nvk_files = files(
  'nvk_acceleration_structure.c',
  'nvk_buffer.c',
  'nvk_buffer.h',
  'nvk_buffer_view.c',
  'nvk_buffer_view.h',
  'nvk_cmd_buffer.c',
  'nvk_cmd_buffer.h',
  'nvk_cmd_clear.c',
  'nvk_cmd_copy.c',
  'nvk_cmd_dispatch.c',
  'nvk_cmd_draw.c',
  'nvk_cmd_indirect.c',
  'nvk_cmd_meta.c',
  'nvk_cmd_pool.c',
  'nvk_cmd_pool.h',
  'nvk_debug.h',
  'nvk_descriptor_set.h',
  'nvk_descriptor_set.c',
  'nvk_descriptor_set_layout.c',
  'nvk_descriptor_set_layout.h',
  'nvk_descriptor_table.c',
  'nvk_descriptor_table.h',
  'nvk_descriptor_types.h',
  'nvk_device.c',
  'nvk_device.h',
  'nvk_device_memory.c',
  'nvk_device_memory.h',
  'nvk_edb_bview_cache.c',
  'nvk_edb_bview_cache.h',
  'nvk_event.c',
  'nvk_event.h',
  'nvk_format.c',
  'nvk_format.h',
  'nvk_heap.c',
  'nvk_heap.h',
  'nvk_host_copy.c',
  'nvk_image.c',
  'nvk_image.h',
  'nvk_image_view.c',
  'nvk_image_view.h',
  'nvk_indirect_execution_set.c',
  'nvk_indirect_execution_set.h',
  'nvk_instance.c',
  'nvk_instance.h',
  'nvk_mem_arena.c',
  'nvk_mem_arena.h',
  'nvk_mem_stream.c',
  'nvk_mem_stream.h',
  'nvk_mme.c',
  'nvk_mme.h',
  'nvk_nir_lower_descriptors.c',
  'nvk_nir_lower_mesh_shader.c',
  'nvk_physical_device.c',
  'nvk_physical_device.h',
  'nvk_private.h',
  'nvk_query_pool.c',
  'nvk_query_pool.h',
  'nvk_queue.c',
  'nvk_queue.h',
  'nvk_sampler.c',
  'nvk_sampler.h',
  'nvk_shader.c',
  'nvk_shader.h',
  'nvk_upload_queue.c',
  'nvk_upload_queue.h',
  'nvk_wsi.c',
  'nvk_wsi.h',
  'layers/nvk_app_workarounds.c',
  'layers/nvk_app_workarounds.h',
  'nvkmd/nvkmd.c',
  'nvkmd/nvkmd.h',
)

nvkcl_files = files(
  'cl/nvk_query.cl',
  'cl/nvk_copy_indirect.cl',
)

nvk_common_deps = [
  dep_libdrm,
  idep_nak,
  idep_nil,
  idep_nir,
  idep_mesautil,
  idep_nouveau_mme,
  idep_nouveau_ws,
  idep_nvidia_headers,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
]

# The native game backend calls Switch 2 services only through libounce-nx.
# Build libounce-nx with SWITCH2=1. In the native-headless cross file, its
# headers replace the devkitPro libnx headers.
nvk_native_map_lib = []
nvk_libounce_nx_link_args = []
nvk_libounce_nx_include_dirs = []
nvk_with_native_backing = false
nvk_libounce_nx = get_option('nvk-libounce-nx')
if nvk_libounce_nx != ''
  if host_machine.system() != 'horizon'
    error('nvk-libounce-nx requires the Horizon target')
  endif
  nvk_libounce_nx_include_dirs = [include_directories(
    join_paths(nvk_libounce_nx, 'include'),
  )]
  nvk_native_map_lib = [static_library(
    'nvk_native_map',
    files(
      'nvkmd/nvgpu/nvkmd_nvgpu_native_backing.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_arena_bind.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_mem.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_device_owner.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_chunked.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_views.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_profile_log.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_chunk_reserve.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_host_mem.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_owner_lock.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_pdev.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_ctx.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_progress.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_batch.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_semaphore_probe.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_map.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_channel.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_channel_setup.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_terminal.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_submit.c',
      'nvkmd/nvgpu/nvkmd_nvgpu_native_completion.c',
    ),
    c_args : ['-D__SWITCH2__'],
    include_directories : [inc_include, inc_src] + nvk_libounce_nx_include_dirs,
    dependencies : nvk_common_deps,
    gnu_symbol_visibility : 'hidden',
  )]
  # lib/libounce.a is a linker script that names libounce_malloc.a and
  # libounce_core.a, which the linker finds on the library path.
  nvk_libounce_nx_link_args = ['-L' + join_paths(nvk_libounce_nx, 'lib'),
                               '-lounce']
  nvk_with_native_backing = true
endif

if host_machine.system() == 'horizon'
  nvk_files += files(
    'nvkmd/nvgpu/nvkmd_nvgpu_native_profile.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_profile.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_compute_info.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_compute_info.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_syncobj.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_syncobj.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_release.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_release.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_class_view.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_class_view.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_runtime.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_runtime.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_va.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_va.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_arena.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_native_arena.h',
    'nvkmd/nvgpu/nvkmd_nvgpu_discovery.c',
    'nvkmd/nvgpu/nvkmd_nvgpu_discovery.h',
  )
else
  nvk_files += files(
    'nvk_cubin.c',
    'nvk_cubin.h',
    'nvkmd/nouveau/nvkmd_nouveau.h',
    'nvkmd/nouveau/nvkmd_nouveau_ctx.c',
    'nvkmd/nouveau/nvkmd_nouveau_dev.c',
    'nvkmd/nouveau/nvkmd_nouveau_mem.c',
    'nvkmd/nouveau/nvkmd_nouveau_pdev.c',
    'nvkmd/nouveau/nvkmd_nouveau_va.c',
  )
endif

nvk_entrypoints = custom_target(
  'nvk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['nvk_entrypoints.h', 'nvk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'nvk',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

nvkcl_spv = custom_target(
  input : [nvkcl_files, libnvidia_headers],
  output : 'nvkcl.spv',
  command : [
    prog_mesa_clc, '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', nvkcl_files, '--',
    '-I' + join_paths(meson.project_source_root(), 'src/compiler/libcl'),
    '-I' + join_paths(meson.current_source_dir(), '.'),
    '-I' + join_paths(meson.project_source_root(), 'src'),
    '-I' + join_paths(meson.project_source_root(), 'src/nouveau/headers'),
    '-I' + join_paths(meson.project_source_root(), 'src/nouveau/headers/nvidia/classes'),
    '-I' + join_paths(meson.project_build_root(), 'src/nouveau/headers'),
    cl_args,
    ],
  depfile : 'nvkcl.spv.d',
)

nvkcl = custom_target(
  input : nvkcl_spv,
  output : ['nvkcl.cpp', 'nvkcl.h'],
  command : [prog_vtn_bindgen2, '@INPUT@', '@OUTPUT@'],
)

nvk_drirc = custom_target(
  'nvk_drirc',
  input  : [files('nvk_drirc_gen.py'), drirc_gen, '00-nvk-defaults.conf'],
  output : ['nvk_drirc.c', 'nvk_drirc.h'],
  command : [
    prog_python, '@INPUT0@',
    '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@',
    '--drirc-hdr', '@OUTPUT1@',
    '--validate', '@INPUT2@',
  ],
)

nvk_deps = nvk_common_deps

if host_machine.system() != 'horizon'
  nvk_deps += [dep_elf, idep_nouveau_cubin]
endif

nvk_flags = []
if nvk_with_native_backing
  nvk_flags += '-DNVK_NATIVE_BACKING_ENABLED'
endif

nvk_build_id = get_option('nvk-build-id')
if nvk_build_id != ''
  nvk_flags += '-DNVK_BUILD_ID_OVERRIDE="' + nvk_build_id + '"'
endif

if with_platform_android
  nvk_files += files('nvk_android.c')
endif

# Horizon has no Vulkan loader. A program links the driver statically and
# calls vkCreateInstance and the other API functions by name. This file
# defines them.
nvk_horizon_exports = []
if host_machine.system() == 'horizon'
  nvk_horizon_exports = custom_target(
    'nvk_horizon_exports',
    input : [files('nvk_horizon_exports_gen.py'), vk_api_xml],
    output : 'nvk_horizon_exports.c',
    command : [
      prog_python, '@INPUT0@', '--xml', '@INPUT1@',
      '--import-path', join_paths(dir_source_root, 'src/vulkan/util'),
      '--out', '@OUTPUT@',
    ],
    depend_files : vk_entrypoints_gen_depend_files,
  )
endif

# Horizon has no ELF build-id note to key the shader cache. This header gets
# a new value in every build, so a cache entry never outlives the compiler
# that made it (nvk_instance.c).
nvk_horizon_build_stamp = []
if host_machine.system() == 'horizon'
  nvk_horizon_build_stamp = custom_target(
    'nvk_horizon_build_stamp',
    output : 'nvk_horizon_build_stamp.h',
    command : [
      prog_python, '-c',
      'import sys, time; q = chr(34); open(sys.argv[1], "w").write("#define NVK_HORIZON_BUILD_STAMP " + q + str(time.time_ns()) + q + chr(10))',
      '@OUTPUT@',
    ],
    build_always_stale : true,
  )
endif

libnvk = static_library(
  'nvk',
  [
    nvk_entrypoints,
    nvk_horizon_exports,
    nvk_horizon_build_stamp,
    nvk_drirc,
    cl_generated,
    nvk_files,
    nvkcl,
    sha1_h,
  ],
  include_directories : [inc_include, inc_src] + nvk_libounce_nx_include_dirs,
  dependencies : nvk_deps,
  c_args : [no_override_init_args, nvk_flags],
  gnu_symbol_visibility : 'hidden',
)

libvulkan_nouveau = shared_library(
  'vulkan_nouveau',
  link_whole : [libnvk] + nvk_native_map_lib,
  link_args: [ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections] + nvk_libounce_nx_link_args,
  gnu_symbol_visibility : 'hidden',
  install : true,
)

icd_file_name = libname_prefix + 'vulkan_nouveau.' + libname_suffix

nouveau_icd = custom_target(
  'nouveau_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'nouveau_icd.' + vulkan_manifest_suffix,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'nouveau_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'nouveau_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

if with_tests and not with_platform_android
  test(
    'nvk_mme',
    executable(
      'test_nvk_mme',
      files('test_nvk_mme.c'),
      dependencies : [nvk_deps],
      link_with : [libnvk],
    ),
    suite : ['nouveau'],
  )
endif
