project('pnt-edge', ['cpp', 'c'],
  version: '0.1.0',
  license: 'Apache-2.0',
  meson_version: '>=1.3',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'buildtype=debugoptimized', 'default_library=static'])

cpp = meson.get_compiler('cpp')
add_project_arguments(cpp.get_supported_arguments('-Wno-unused-parameter'), language: 'cpp')

# The navigation filter: the Cobra C++ port, pinned to a release tag (subprojects/pntos-cpp.wrap).
pntos = subproject('pntos-cpp', default_options: ['tests=false', 'apps=false', 'warning_level=0', 'werror=false'])
cobra_dep = pntos.get_variable('cobra_dep')
thread_dep = dependency('threads')

# Optional integrity-monitoring orchestration: an out-of-tree Cobra plugin placed (symlinked or copied) at
# subprojects/cobra-integrity and enabled with -Dintegrity=enabled. It builds against the same Cobra subproject,
# so the plugin registers with the filter this executable runs; the filter config may then select
# "orchestration": "protected" and the integrity status reaches the NMEA stream and the status file.
extra_deps = []
extra_args = []
if get_option('integrity').enabled() or (get_option('integrity').auto() and import('fs').is_dir('subprojects/cobra-integrity'))
  integrity_proj = subproject('cobra-integrity', default_options: ['tests=false'])
  extra_deps += integrity_proj.get_variable('integrity_dep')
  extra_args += '-DPNT_EDGE_HAS_INTEGRITY'
endif

edge_inc = include_directories('src', 'third_party')
www_install_dir = get_option('datadir') / 'pnt-edge' / 'www'
edge_args = ['-DPNT_EDGE_VERSION="' + meson.project_version() + '"',
             '-DPNT_EDGE_WWW="' + (get_option('prefix') / www_install_dir) + '"']
edge_lib = static_library('pnt-edge-core',
  'src/nmea.cpp', 'src/inputs.cpp', 'src/outputs.cpp', 'src/status.cpp', 'src/edge_config.cpp', 'src/integrity.cpp',
  'src/web/hub.cpp', 'src/web/auth.cpp', 'src/web/server.cpp',
  include_directories: edge_inc, dependencies: [cobra_dep, thread_dep], cpp_args: edge_args)
edge_dep = declare_dependency(link_with: edge_lib, include_directories: edge_inc, dependencies: [cobra_dep, thread_dep])

executable('pnt-edge', 'src/main.cpp', dependencies: [edge_dep] + extra_deps, cpp_args: extra_args + edge_args, install: true)
install_subdir('www', install_dir: get_option('datadir') / 'pnt-edge')

if get_option('tests')
  gtest_proj = subproject('gtest')
  test_exe = executable('test_pnt_edge', 'tests/test_nmea.cpp', 'tests/test_edge.cpp', 'tests/test_web.cpp',
    dependencies: [edge_dep, gtest_proj.get_variable('gtest_main_dep')])
  test('pnt-edge', test_exe, protocol: 'gtest', timeout: 300,
    env: ['PNT_EDGE_SRCDIR=' + meson.project_source_root()])
endif

install_data('configs/pnt-edge.json', install_dir: get_option('sysconfdir') / 'pnt-edge')
install_data('packaging/pnt-edge.service', install_dir: 'lib/systemd/system')
