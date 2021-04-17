project('fcft', 'c',
        version: '2.3.3',  # Don't forget to update version in man pages
        license: 'MIT',
        meson_version: '>=0.54.0',
        default_options: [
          'c_std=c18',
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
  '3',      # MINOR: increment with backward compatible ABI changes
  '1',      # PATCH: increment with non-ABI affecting changes
]

is_debug_build = get_option('buildtype').startswith('debug')

cc = meson.get_compiler('c')

add_project_arguments(
  ['-D_GNU_SOURCE=200809L'] +
  (is_debug_build ? ['-D_DEBUG'] : []) +
  (cc.has_function('memfd_create') ? ['-DMEMFD_CREATE'] : []) +
  (cc.has_argument('-fvisibility=default') ? ['-DFCFT_EXPORT=__attribute__((visibility("default")))'] : ['-DFCFT_EXPORT=""']),
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
fontconfig = dependency('fontconfig')
freetype = dependency('freetype2')
harfbuzz = dependency('harfbuzz', required: get_option('text-shaping'))
pixman = dependency('pixman-1')
stdthreads = cc.find_library('stdthreads', required: false)

tllist = dependency('tllist', version: '>=1.0.1', fallback: 'tllist')

if harfbuzz.found()
  add_project_arguments('-DFCFT_HAVE_HARFBUZZ', language: 'c')
endif

generate_unicode_precompose_sh = files('generate-unicode-precompose.sh')
unicode_data = custom_target(
  'unicode-data',
  input: 'unicode/UnicodeData.txt',
  output: 'unicode-compose-table.h',
  command: [generate_unicode_precompose_sh, '@INPUT@', '@OUTPUT@'])

generate_version_sh = files('generate-version.sh')
version = custom_target(
  'generate_version',
  build_always_stale: true,
  output: 'version.h',
  command: [generate_version_sh, meson.project_version(), '@CURRENT_SOURCE_DIR@', '@OUTPUT@'])

fcft_lib = build_target(
  'fcft', 'fcft.c', 'fcft/fcft.h', 'fcft/stride.h', 'log.c', 'log.h',
  unicode_data, version,
  target_type: meson.is_subproject() ? 'static_library' : 'library',
  version: '.'.join(so_version),
  dependencies: [math, threads, fontconfig, freetype, harfbuzz, pixman, tllist, stdthreads],
  gnu_symbol_visibility: 'hidden',
  install: not meson.is_subproject())

fcft = declare_dependency(
  include_directories: '.', link_with: fcft_lib, dependencies: [pixman])
meson.override_dependency('fcft', fcft)

if get_option('examples')
  subdir('example')
endif

check = dependency('check', required: false)
if check.found()
  fcft_test = executable('test-fcft', 'test.c', dependencies: [check, fcft])
  test('fcft', fcft_test, args: get_option('test-text-shaping') ? ['--text-shaping'] : [])
endif

if not meson.is_subproject()
  install_headers('fcft/fcft.h', 'fcft/stride.h', subdir: 'fcft')

  pkg = import('pkgconfig')
  pkg.generate(
    fcft_lib,
    description : 'Simple font loading and glyph rasterization library')

  install_data(
    'LICENSE', 'README.md', 'CHANGELOG.md',
    install_dir: join_paths(get_option('datadir'), 'doc', 'fcft'))

  subdir('doc')
endif

summary(
  {
    'Text shaping': harfbuzz.found(),
    'Test text shaping': get_option('test-text-shaping'),
  },
  bool_yn: true
)
