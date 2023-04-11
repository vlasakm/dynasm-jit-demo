project(
  'jit-demo',
  'c',
  version : '0.1',
  default_options : ['c_std=c11', 'warning_level=2'],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

minilua = executable(
  'minilua',
  sources : files(
    'dynasm/minilua.c',
  ),
  dependencies : m_dep,
)

dynasm = generator(
  minilua,
  output : '@BASENAME@-dasm.c',
  arguments : [meson.current_source_dir() + '/dynasm/dynasm.lua', '-o', '@OUTPUT@', '@INPUT@'],
)

fast = executable(
  'demo1',
  sources: dynasm.process('src/demo1.c'),
  include_directories: include_directories(
    'src',
    'dynasm'
  ),
)
