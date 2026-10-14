test_nv2a_webgpu_prim = executable(
  'test-nv2a-webgpu-prim',
  files(
    'test-nv2a-webgpu-prim.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/webgpu/prim.c'
  ),
  dependencies: [qemuutil, webgpu.partial_dependency(compile_args: true,
                                                     includes: true)],
  build_by_default: false,
)

test_env = environment()
if host_os == 'darwin'
  macos_arch = host_machine.cpu() == 'aarch64' ? 'arm64' : host_machine.cpu()
  test_env.prepend('DYLD_FALLBACK_LIBRARY_PATH', meson.project_source_root() / 'macos-libs' / macos_arch / 'opt/local/lib')
  test_env.prepend('DYLD_LIBRARY_PATH', meson.project_source_root() / 'macos-libs' / macos_arch / 'opt/local/lib')
endif

test('test-nv2a-webgpu-prim', test_nv2a_webgpu_prim,
     args: ['--tap', '-k'],
     protocol: 'tap',
     env: test_env,
     suite: ['xbox'])
