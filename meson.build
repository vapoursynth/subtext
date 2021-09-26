project('subtext', 'c', 'cpp',
  version : '2',
  default_options : ['warning_level=3'])

add_project_arguments('-ffast-math', language : 'c')

sources = [
     'src/common.c',
     'src/common.h',
     'src/image.cpp',
     'src/text.c',
     'src/toass.cpp',
     'src/toutf8.c'
]

vapoursynth_dep = dependency('vapoursynth', version: '>=55').partial_dependency(compile_args : true, includes : true)
libass_dep = dependency('libass', version: '>=0.12.0')
libavcodec_dep = dependency('libavcodec')
libavutil_dep = dependency('libavutil')
libavformat_dep = dependency('libavformat')

deps = [vapoursynth_dep, libass_dep, libavcodec_dep, libavutil_dep, libavformat_dep]

shared_module('subtext', sources,
  dependencies : deps,
  install : true,
  install_dir : join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth'),
  gnu_symbol_visibility : 'hidden'
)
