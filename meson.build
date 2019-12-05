project('fcft', 'c',
        version: '0.4.0',
        license: 'MIT',
        meson_version: '>=0.47.0',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

# See https://autotools.io/libtool/version.html, it explains the
# problem, but note that the way you tell libtool the library version
# is not what the final so file name will end up having.
#
# libtool calls these CURRENT, REVISION and AGE. But since we don't
# define them in the same way, we don't call them that.
#
# Note that MAJOR is the *only* version number that affects the
# SONAME, and thus actual linking.
so_version = [
  '0',      # MAJOR: increment on non-backward compatible ABI changes
  '0',      # MINOR: increment with backward compatible ABI changes
  '0',      # PATCH: increment with non-ABI affecting changes
]

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

tllist = dependency('tllist', version: '>=1.0.0', fallback: ['tllist', 'tllist'])

fcft_lib = build_target(
  'fcft', 'fcft.c', 'fcft/fcft.h', 'fcft/stride.h', 'log.c', 'log.h',
  target_type: meson.is_subproject() ? 'static_library' : 'library',
  version: '.'.join(so_version),
  dependencies: [math, threads, fontconfig, freetype, pixman, tllist],
  install: not meson.is_subproject())

fcft = declare_dependency(
  include_directories: '.', link_with: fcft_lib, dependencies: [pixman])

if not meson.is_subproject()
  install_headers('fcft/fcft.h', 'fcft/stride.h', subdir: 'fcft')

  pkg = import('pkgconfig')
  pkg.generate(
    fcft_lib,
    description : 'Simple font loading and glyph rasterization library')
endif
