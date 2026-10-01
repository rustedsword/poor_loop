project(
  'poor_loop',
  'c',
  version: '0.1.0',
  license: 'MIT',
  license_files: 'LICENSE',
  meson_version: '>= 1.4.0',
  default_options: ['c_std=c23', 'warning_level=3'],
)

liburing = dependency('liburing', version: '>=2.3', include_type: 'system')
poor_base = dependency('poor_base')
inc = include_directories('include')

poor_loop = library(
  'poor_loop',
  'src/log.c',
  'src/loop.c',
  'src/op.c',
  'src/timer.c',
  include_directories: inc,
  dependencies: [liburing, poor_base],
  version: meson.project_version(),
  soversion: '0',
  install: true,
)

poor_loop_dep = declare_dependency(
  include_directories: inc,
  link_with: poor_loop,
  dependencies: [liburing, poor_base],
)
meson.override_dependency('poor_loop', poor_loop_dep)

install_headers(
  'include/poor_loop.h',
  'include/poor_loop_log.h',
  'include/poor_loop_op.h',
  'include/poor_loop_timer.h',
  'include/poor_loop_uring_array.h',
)

import('pkgconfig').generate(
  poor_loop,
  description: 'Minimal io_uring event loop',
  # A string, since Meson leaves a header-only fallback dependency out
  requires: [liburing, 'poor_base'],
)

if get_option('tests')
  subdir('tests')
  subdir('tools')
endif

if get_option('examples')
  subdir('examples/cat')
  subdir('examples/nanochat')
endif
