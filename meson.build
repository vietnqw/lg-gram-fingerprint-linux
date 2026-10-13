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
  'src/error.c', 'src/proto.c', 'src/tls.c', 'src/device.c', 'src/sensor.c', 'src/hw.c', 'src/provision.c',
  include_directories: core_inc,
  dependencies: [glib, gio, crypto],
  pic: true)
core_dep = declare_dependency(link_with: core, include_directories: core_inc,
                              dependencies: [glib, gio, crypto])

opencv = dependency('opencv4', version: '>= 4.5')
# Vendored SIGFM (LGPL-2.1+): built as-is, without our stricter warnings.
sigfm = static_library('sigfm', 'src/sigfm/sigfm.cpp',
  dependencies: opencv,
  override_options: ['warning_level=0', 'werror=false'],
  pic: true)
match = static_library('geneva-match', 'src/match.cpp',
  include_directories: core_inc,
  dependencies: [glib, opencv],
  link_with: sigfm,
  pic: true)
match_dep = declare_dependency(link_with: [match, sigfm], include_directories: core_inc,
                               dependencies: [glib, opencv])

executable('geneva-fp-tool', 'tools/geneva-fp-tool.c',
  dependencies: core_dep,
  install: true)

if get_option('driver')
  tod = dependency('libfprint-2-tod-1')
  tod_dir = tod.get_variable(pkgconfig: 'tod_driversdir')
  shared_module('geneva', 'src/tod-geneva.c',
    name_prefix: 'libfprint-2-tod-',
    dependencies: [core_dep, match_dep, tod, dependency('libfprint-2')],
    gnu_symbol_visibility: 'hidden',
    link_args: ['-Wl,--exclude-libs,ALL'],
    install: true,
    install_dir: tod_dir)
endif

if get_option('tests')
  subdir('tests')
endif
