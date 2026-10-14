specific_ss.add([files(
	'common.c',
	'geom.c',
	'psh.c',
	'shaders.c',
	'vsh.c',
	'vsh-ff.c',
	'vsh-prog.c',
	)])
if libglslang.found()
  specific_ss.add([libglslang, files('spirv.c')])
endif
