project('geneva-fp', 'c', 'cpp',
  version: '0.1.0',
  license: 'LGPL-2.1-or-later',
  meson_version: '>= 0.62',
  default_options: ['c_std=gnu11', 'cpp_std=c++17', 'warning_level=2', 'werror=true',
                    'buildtype=debugoptimized'])

add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      language: ['c', 'cpp'])

glib = dependency('glib-2.0', version: '>= 2.68')
gio = dependency('gio-2.0')
crypto = dependency('libcrypto', version: '>= 3.0')

core_inc = include_directories('src')
core = static_library('geneva-core',
  'src/error.c', 'src/proto.c', 'src/tls.c', 'src/device.c', 'src/sensor.c', 'src/hw.c',
  include_directories: core_inc,
  dependencies: [glib, gio, crypto],
  pic: true)
core_dep = declare_dependency(link_with: core, include_directories: core_inc,
                              dependencies: [glib, gio, crypto])

executable('geneva-fp-tool', 'tools/geneva-fp-tool.c',
  dependencies: core_dep,
  install: true)

if get_option('tests')
  subdir('tests')
endif
