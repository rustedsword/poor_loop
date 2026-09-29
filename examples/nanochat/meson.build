if liburing.version().version_compare('>=2.8')
  foreach name : ['server', 'client']
    executable(
      'nanochat_' + name,
      name + '.c',
      dependencies: poor_loop_dep,
    )
  endforeach
endif
