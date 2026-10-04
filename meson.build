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

edge_inc = include_directories('src')
edge_lib = static_library('pnt-edge-core',
  'src/nmea.cpp', 'src/inputs.cpp', 'src/outputs.cpp', 'src/status.cpp', 'src/edge_config.cpp',
  include_directories: edge_inc, dependencies: [cobra_dep, thread_dep])
edge_dep = declare_dependency(link_with: edge_lib, include_directories: edge_inc, dependencies: [cobra_dep, thread_dep])

executable('pnt-edge', 'src/main.cpp', dependencies: [edge_dep], install: true)

if get_option('tests')
  gtest_proj = subproject('gtest')
  test_exe = executable('test_pnt_edge', 'tests/test_nmea.cpp', 'tests/test_edge.cpp',
    dependencies: [edge_dep, gtest_proj.get_variable('gtest_main_dep')])
  test('pnt-edge', test_exe, protocol: 'gtest', timeout: 300,
    env: ['PNT_EDGE_SRCDIR=' + meson.project_source_root()])
endif

install_data('configs/pnt-edge.json', install_dir: get_option('sysconfdir') / 'pnt-edge')
install_data('packaging/pnt-edge.service', install_dir: 'lib/systemd/system')
