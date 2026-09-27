test_loop = executable('test_loop', 'test_loop.c', dependencies: chioloop_dep)

names = []
foreach line : import('fs').read('test_loop.c').split('\n')
  if line.startswith('\tTEST(') and line.endswith('),')
    names += line.substring(6, -2)
  endif
endforeach

foreach mode : ['default', 'coop', 'defer', 'sqpoll']
  foreach name : names
    test(name, test_loop, args: [mode, name], suite: mode)
  endforeach
endforeach
