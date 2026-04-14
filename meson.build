project('SubText', 'c', 'cpp',
    default_options: [
        'buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'c_std=c11', 'cpp_std=c++17',
    ],
    license: 'MIT',
    license_files: 'LICENSE',
    meson_version: '>=1.2.3',
    version: '6.0',
)

deps = [
    dependency('libass', version: '>=0.12.0'),
    dependency('libavcodec'),
    dependency('libavformat'),
    dependency('libavutil'),
]

incdir = include_directories(
    run_command(
        find_program('python', 'python3'),
        '-c',
        'import vapoursynth as vs; print(vs.get_include())',
        check: true,
    ).stdout().strip(),
)

link_args = []

py = import('python').find_installation(pure: false)

if meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
    link_args += '-Wl,-Bsymbolic'
endif

shared_module('subtext',
    files('src/common.c', 'src/image.cpp', 'src/text.c', 'src/toass.cpp', 'src/toutf8.c'),
    dependencies: deps,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    link_args: link_args,
    name_prefix: '',
)
