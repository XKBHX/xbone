subdir('interp')

dsp_sources = files('dsp.c', 'dsp_c.c', 'dsp_dma.c') + genh
dsp_deps = []

# The Rust JIT ships as a prebuilt static library for darwin, windows, and
# linux. Emscripten has no such archive, so the browser build uses the C
# interpreter only.
if host_machine.system() != 'emscripten'
  dsp56300_emu_ffi = dependency('dsp56300-emu-ffi',
                                fallback: ['dsp56300', 'dsp56300_dep'])
  dsp_sources += files('dsp_jit.c')
  dsp_deps += dsp56300_emu_ffi
endif

libdsp = static_library(
  'dsp',
  dsp_sources,
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
