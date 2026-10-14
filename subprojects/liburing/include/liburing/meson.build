cc = meson.get_compiler('c')

compat = {
  'KERNEL_RWF_T': cc.has_type('__kernel_rwf_t', prefix: '#include <linux/fs.h>').to_int(),
  'OPEN_HOW': cc.has_type('struct open_how', prefix: '#include <linux/openat2.h>').to_int(),
  'FUTEX_WAITV': cc.has_type('struct futex_waitv', prefix: '#include <linux/futex.h>').to_int(),
  'IDTYPE_T': cc.has_type('idtype_t', prefix: '#define _GNU_SOURCE\n#include <sys/wait.h>').to_int(),
}

configure_file(
  input: 'compat.h.in',
  output: 'compat.h',
  configuration: compat,
  install: true,
  install_dir: get_option('includedir') / 'liburing',
)

configure_file(
  output: 'io_uring_version.h',
  configuration: {
    'IO_URING_VERSION_MAJOR': version.split('.')[0],
    'IO_URING_VERSION_MINOR': version.split('.')[1],
  },
  install: true,
  install_dir: get_option('includedir') / 'liburing',
)
