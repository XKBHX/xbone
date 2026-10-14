dsp_srcs = files(['dsp.c', 'dsp_c.c', 'dsp_dma.c'])
dsp_deps = []

# The DSP JIT is a prebuilt native library with no wasm build.
if host_os != 'emscripten'
  dsp_srcs += files('dsp_jit.c')
  dsp_deps += dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'])
endif

subdir('interp')

libdsp = static_library(
  'dsp',
  dsp_srcs + genh,
  dependencies: dsp_deps,
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
  dependencies: dsp_deps,
)

mcpx_ss.add(dsp, files('gp_ep.c'))
