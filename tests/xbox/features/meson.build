test_nv2a_webgpu_features = executable(
  'test-nv2a-webgpu-features',
  files(
    'test-nv2a-webgpu-features.c',
    meson.project_source_root() / 'hw/xbox/nv2a/pgraph/webgpu/features.c'
  ),
  dependencies: [qemuutil],
  build_by_default: false,
)

test_env = environment()
if host_os == 'darwin'
  macos_arch = host_machine.cpu() == 'aarch64' ? 'arm64' : host_machine.cpu()
  test_env.prepend('DYLD_FALLBACK_LIBRARY_PATH', meson.project_source_root() / 'macos-libs' / macos_arch / 'opt/local/lib')
  test_env.prepend('DYLD_LIBRARY_PATH', meson.project_source_root() / 'macos-libs' / macos_arch / 'opt/local/lib')
endif

test('test-nv2a-webgpu-features', test_nv2a_webgpu_features,
     args: ['--tap', '-k'],
     protocol: 'tap',
     env: test_env,
     suite: ['xbox'])
