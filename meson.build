project('SubText', 'c', 'cpp',
    default_options: ['buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'MIT',
    license_files: 'LICENSE',
    meson_version: '>=1.2.3',
    version: '6.0',
)

sources = [
     'src/common.c',
     'src/common.h',
     'src/image.cpp',
     'src/text.c',
     'src/toass.cpp',
     'src/toutf8.c'
]

libass_dep = dependency('libass', version: '>=0.12.0')
libavcodec_dep = dependency('libavcodec')
libavutil_dep = dependency('libavutil')
libavformat_dep = dependency('libavformat')

deps = [libass_dep, libavcodec_dep, libavutil_dep, libavformat_dep]

incdir = include_directories(
    run_command(
        find_program('python', 'python3'),
        '-c',
        'import vapoursynth as vs; print(vs.get_include())',
        check: true,
    ).stdout().strip(),
)

py = import('python').find_installation(pure: false)

shared_module('dmetrics',
    sources,
    dependencies : deps,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    name_prefix: '',
)