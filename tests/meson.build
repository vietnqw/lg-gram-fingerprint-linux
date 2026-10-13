ssl = dependency('libssl')

foreach t : ['proto', 'tls']
  test(t, executable('test-' + t, 'test-' + t + '.c',
                     dependencies: [core_dep, ssl]))
endforeach
