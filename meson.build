project('fcft', 'c',
        version: '2.0.0',
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
  '3',      # MAJOR: increment on non-backward compatible ABI changes
  '0',      # MINOR: increment with backward compatible ABI changes
  '0',      # PATCH: increment with non-ABI affecting changes
]

is_debug_build = get_option('buildtype').startswith('debug')

cc = meson.get_compiler('c')

add_project_arguments(
  ['-D_GNU_SOURCE=200809L'] + (is_debug_build ? ['-D_DEBUG'] : []),
  language: 'c')

# Compute the relative path used by compiler invocations.
source_root = meson.current_source_dir().split('/')
build_root = meson.build_root().split('/')
relative_dir_parts = []
i = 0
in_prefix = true
foreach p : build_root
  if i >= source_root.length() or not in_prefix or p != source_root[i]
    in_prefix = false
    relative_dir_parts += '..'
  endif
  i += 1
endforeach
i = 0
in_prefix = true
foreach p : source_root
  if i >= build_root.length() or not in_prefix or build_root[i] != p
    in_prefix = false
    relative_dir_parts += p
  endif
  i += 1
endforeach
relative_dir = join_paths(relative_dir_parts) + '/'

if cc.has_argument('-fmacro-prefix-map=/foo=')
  add_project_arguments('-fmacro-prefix-map=@0@='.format(relative_dir), language: 'c')
endif

math = cc.find_library('m')
threads = dependency('threads')
freetype = dependency('freetype2')
fontconfig = dependency('fontconfig')
pixman = dependency('pixman-1')
# required on the freebsd platform (maybe even on others)
stdthreads = cc.find_library('stdthreads', required: host_machine.system() == 'freebsd')

tllist = dependency('tllist', version: '>=1.0.0', fallback: ['tllist', 'tllist'])

fcft_lib = build_target(
  'fcft', 'fcft.c', 'fcft/fcft.h', 'fcft/stride.h', 'log.c', 'log.h',
  target_type: meson.is_subproject() ? 'static_library' : 'library',
  version: '.'.join(so_version),
  dependencies: [math, threads, fontconfig, freetype, pixman, tllist, stdthreads],
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
