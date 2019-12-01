project('fcft', 'c',
        version: '0.0.0',
        license: 'MIT',
        meson_version: '>=0.47.0',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

add_project_arguments(
  ['-D_GNU_SOURCE=200809L'] + (is_debug_build ? ['-D_DEBUG'] : []),
  language: 'c')

cc = meson.get_compiler('c')
math = cc.find_library('m')
threads = dependency('threads')
freetype = dependency('freetype2')
fontconfig = dependency('fontconfig')
pixman = dependency('pixman-1')

tllist = subproject('tllist').get_variable('tllist')
library('fcft', 'font.c', 'font.h', 'log.c', 'log.h',
        dependencies: [math, threads, fontconfig, freetype, pixman, tllist],
        install: true)
