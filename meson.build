project('SubText', 'cpp',
    default_options: [
        'buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'cpp_std=c++17',
    ],
    license: 'MIT',
    license_files: 'LICENSE',
    meson_version: '>=1.2.3',
    version: '7.0',
)

add_project_arguments('-DVS_USE_API_42', language: 'cpp')

deps = [
    dependency('libass', version: '>=0.17.0'),
    # FFmpeg 8.0 or newer
    dependency('libavcodec', version: '>=62'),
    dependency('libavformat', version: '>=62'),
    dependency('libavutil', version: '>=60'),
]

py = import('python').find_installation(pure: false)

incdir = include_directories(
    run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
)

link_args = []

if meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
    link_args += '-Wl,-Bsymbolic'
endif

shared_module('subtext',
    files('src/common.cpp', 'src/image.cpp', 'src/text.cpp', 'src/toass.cpp', 'src/toutf8.cpp'),
    dependencies: deps,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    link_args: link_args,
    name_prefix: '',
)
