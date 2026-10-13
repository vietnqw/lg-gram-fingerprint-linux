ssl = dependency('libssl')

foreach t : ['proto', 'tls']
  test(t, executable('test-' + t, 'test-' + t + '.c',
                     dependencies: [core_dep, ssl]))
endforeach

test('match', executable('test-match', 'test-match.c', dependencies: [core_dep, match_dep]))

executable('match-eval', 'match-eval.c', dependencies: [core_dep, match_dep])

if get_option('driver')
  executable('fp-harness', 'fp-harness.c', dependencies: dependency('libfprint-2'))
endif
