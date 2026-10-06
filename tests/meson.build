fs = import('fs')

harness = static_library(
  'harness',
  'harness.c',
  dependencies: poor_loop_dep,
)

extra_sources = {'group': files('shared_group.c')}

foreach component : ['group', 'log', 'loop', 'op', 'timer', 'uring_array']
  source = 'test_' + component + '.c'
  exe = executable(
    'test_' + component,
    [source, extra_sources.get(component, [])],
    link_with: harness,
    dependencies: [poor_loop_dep, dependency('dl'), dependency('threads')],
  )

  names = []
  foreach line : fs.read(source).split('\n')
    if line.startswith('\tTEST(') and line.endswith('),')
      names += line.substring(6, -2)
    endif
  endforeach

  foreach mode : ['default', 'coop', 'defer', 'sqpoll']
    foreach name : names
      test(name, exe, args: [mode, name], suite: [mode, component])
    endforeach
  endforeach
endforeach

compile_cmd = meson.get_compiler('c').cmd_array() + [
  '-std=c23', '-fsyntax-only', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
  '-I' + meson.project_source_root() / 'include',
]
liburing_inc = liburing.get_variable(pkgconfig: 'includedir', default_value: '')
if liburing_inc not in ['', '/usr/include']
  compile_cmd += ['-isystem', liburing_inc]
endif
poor_base_inc = poor_base.get_variable(
  pkgconfig: 'includedir',
  default_value: meson.project_source_root() / 'subprojects' / 'poor_base' / 'include',
)
if poor_base_inc != '/usr/include'
  compile_cmd += '-I' + poor_base_inc
endif

foreach name, expect : {
  'OK': '',
  'BAD_ATTACH_GROUP': 'value must be an unqualified io_group or a pointer to one',
  'CONST_ATTACH_GROUP': 'shared must be an unqualified io_group or a pointer to one',
  'BAD_ATTACH_MEMBER': 'value must be a struct poor_loop_op or struct poor_loop_timer',
  'OP_WITH_TIMER': 'on_timer must be attached to a struct poor_loop_op',
  'TIMER_WITH_OP': 'on_op must be attached to a struct poor_loop_timer',
  'CONST_SQE_GROUP': 'discards',
  'CONST_ARM_GROUP': 'discards',
  'DISCARD_RELEASE': 'ignoring return value',
  'DISCARD_DISARM': 'ignoring return value',
  'CALL_DONE_WRONG_GROUP': 'incompatible pointer type',
}
  test(
    'group_types_' + name.to_lower(),
    find_program('sh'),
    args: [files('compile/run.sh'), expect] + compile_cmd +
          ['-D' + name] + files('compile/group_types.c'),
    suite: 'compile',
  )
endforeach
