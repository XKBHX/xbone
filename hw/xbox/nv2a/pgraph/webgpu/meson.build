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
  if host_os == 'emscripten'
    webgpu_files += files('canvas.c')
  else
    webgpu_files += files('window.c')
  endif
  specific_ss.add([webgpu, webgpu_files])
endif
