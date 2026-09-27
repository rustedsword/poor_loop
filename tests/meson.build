fs = import('fs')

harness = static_library(
  'harness',
  'harness.c',
  dependencies: chioloop_dep,
)

foreach component : ['log', 'loop', 'op', 'timer']
  source = 'test_' + component + '.c'
  exe = executable(
    'test_' + component,
    source,
    link_with: harness,
    dependencies: [chioloop_dep, dependency('dl'), dependency('threads')],
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
