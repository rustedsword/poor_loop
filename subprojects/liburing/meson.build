# Port of liburing's src/Makefile and the compat.h part of its configure, since upstream has no
# Meson build. On a submodule bump, sync new sources here and compat changes to include/liburing.
project(
  'liburing',
  'c',
  license: '(GPL-2.0-only WITH Linux-syscall-note AND LGPL-2.1-or-later) OR MIT',
  meson_version: '>= 1.4.0',
  default_options: ['warning_level=2'],
)

fs = import('fs')
version = fs.read('liburing/liburing.spec').split('Version:')[1].split('\n')[0].strip()

subdir('include/liburing')

inc = include_directories('include', 'liburing/src/include')
version_script = 'liburing/src/liburing.map'

liburing = library(
  'uring',
  'liburing/src/queue.c',
  'liburing/src/register.c',
  'liburing/src/setup.c',
  'liburing/src/syscall.c',
  'liburing/src/version.c',
  c_args: ['-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64', '-DLIBURING_INTERNAL', '-Wno-unused-parameter'],
  include_directories: inc,
  link_args: '-Wl,--version-script=' + meson.current_source_dir() / version_script,
  link_depends: version_script,
  version: version,
  install: true,
)

liburing_dep = declare_dependency(
  include_directories: inc,
  link_with: liburing,
  version: version,
)
meson.override_dependency('liburing', liburing_dep)

install_subdir(
  'liburing/src/include',
  install_dir: get_option('includedir'),
  strip_directory: true,
  exclude_files: 'liburing/sanitize.h',
)

import('pkgconfig').generate(
  liburing,
  name: 'liburing',
  description: 'io_uring library',
  version: version,
)
