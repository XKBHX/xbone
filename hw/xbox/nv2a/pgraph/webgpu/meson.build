if webgpu.found()
  webgpu_files = files(
    '../wgsl/gen.c',
    '../wgsl/translate.c',
    'blit.c',
    'buffer.c',
    'command.c',
    'display.c',
    'draw.c',
    'features.c',
    'instance.c',
    'prim.c',
    'renderer.c',
    'reports.c',
    'shaders.c',
    'surface.c',
    'surface-compute.c',
    'texture.c',
    'vertex.c',
  )
  wgpu_validation_dep = declare_dependency()
  if host_os == 'emscripten'
    webgpu_files += files('canvas.c')
  else
    webgpu_files += files('window.c')
    # DawnInstanceDescriptor is C++20 (std::span). The emulator stays on
    # C++17, so this one file is its own target.
    wgpu_validation = static_library(
      'pgraph_wgpu_validation',
      'validation.cc',
      dependencies: webgpu,
      override_options: ['cpp_std=c++20'],
      pic: true,
    )
    wgpu_validation_dep = declare_dependency(link_whole: wgpu_validation)
  endif
  specific_ss.add([webgpu, wgpu_validation_dep, webgpu_files])
endif
