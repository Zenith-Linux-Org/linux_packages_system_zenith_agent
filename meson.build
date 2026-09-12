project('zenith-agent', 'c',
  version: '0.1.0',
  default_options: ['c_std=c23', 'warning_level=3'])

curl_dep = dependency('libcurl')
executable('zenith', 'zenith-agent.c',
  dependencies: curl_dep,
  c_args: ['-Werror'],
  install: true)
