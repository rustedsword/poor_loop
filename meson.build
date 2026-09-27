project(
  'chioloop',
  'c',
  version: '0.1.0',
  license: 'MIT',
  license_files: 'LICENSE',
  meson_version: '>= 1.4.0',
  default_options: ['c_std=c23', 'warning_level=3'],
)

liburing = dependency('liburing', version: '>=2.3', include_type: 'system')
inc = include_directories('include')

chioloop = library(
  'chioloop',
  'src/loop.c',
  'src/op.c',
  'src/timer.c',
  include_directories: inc,
  dependencies: liburing,
  version: meson.project_version(),
  soversion: '0',
  install: true,
)

chioloop_dep = declare_dependency(
  include_directories: inc,
  link_with: chioloop,
  dependencies: liburing,
)
meson.override_dependency('chioloop', chioloop_dep)

install_headers(
  'include/chioloop.h',
  'include/chio_list.h',
  'include/chio_loop.h',
  'include/chio_op.h',
  'include/chio_timer.h',
)

import('pkgconfig').generate(
  chioloop,
  description: 'Minimal io_uring event loop',
  requires: liburing,
)

if not meson.is_subproject()
  subdir('tests')
  subdir('tools')
endif
