project(
  'imv',
  'c',
  version: '4.5.0',
  license: 'MIT',
  meson_version: '>= 0.47',
  default_options: ['buildtype=release', 'c_std=c99'],
)

version = '@0@'.format(meson.project_version())
add_project_arguments('-DIMV_VERSION="@0@"'.format(version), language: 'c')

add_project_arguments('-D_XOPEN_SOURCE=700', language: 'c')
add_project_arguments('-DRSVG_DISABLE_DEPRECATION_WARNINGS', language: 'c')

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

unicode = get_option('unicode')
if unicode == 'icu'
  unicode_lib = dependency('icu-io')
  add_project_arguments('-DIMV_USE_ICU', language: 'c')
elif unicode == 'grapheme'
  unicode_lib = cc.find_library('grapheme')
  add_project_arguments('-DIMV_USE_GRAPHEME', language: 'c')
endif

files_main = files('src/main.c')
files_imv = files(
  'src/binds.c',
  'src/bitmap.c',
  'src/canvas.c',
  'src/commands.c',
  'src/console.c',
  'src/image.c',
  'src/imv.c',
  'src/ipc.c',
  'src/ipc_common.c',
  'src/keyboard.c',
  'src/list.c',
  'src/log.c',
  'src/navigator.c',
  'src/source.c',
  'src/viewport.c',
)

deps_imv = [
  dependency('pangocairo'),
  dependency('threads'),
  dependency('xkbcommon'),
  unicode_lib,
  dependency('inih', fallback : ['inih', 'inih_dep']),
  m_dep,
]

window_system = get_option('window_system')

if window_system == 'wayland' or window_system == 'all'
  files_wayland = files('src/wl_window.c', 'src/xdg-shell-protocol.c')
  deps_wayland = [
    dependency('wayland-client'),
    dependency('wayland-cursor'),
    dependency('wayland-egl'),
    dependency('opengl'),
    dependency('egl'),
    cc.find_library('rt'),
  ]
endif

if window_system == 'x11' or window_system == 'all'
  files_x11 = files('src/x11_window.c')
  deps_x11 = [
    dependency('x11'),
    dependency('gl'),
    dependency('glu'),
    dependency('xcb'),
    dependency('xkbcommon-x11'),
  ]
endif

foreach backend : [
  ['freeimage', 'library', 'freeimage'],
  ['libtiff', 'dependency', 'libtiff-4', []],
  ['libpng', 'dependency', 'libpng', []],
  ['libjpeg', 'dependency', 'libturbojpeg', []],
  ['librsvg', 'dependency', 'librsvg-2.0', '>= 2.44'],
  ['libnsgif', 'dependency', 'libnsgif', '1.0.0'],
  ['libheif', 'dependency', 'libheif', []],
  ['libjxl', 'dependency', 'libjxl', []],
]
  backend_name = backend[0]
  dep_type = backend[1]
  dep_name = backend[2]

  if dep_type == 'dependency'
    dep = dependency(dep_name, required: get_option(backend_name), version: backend[3])
  elif dep_type == 'library'
    dep = cc.find_library(dep_name, required: get_option(backend_name))
  endif

  if dep.found()
    deps_imv += dep
    files_imv += files('src/backend_@0@.c'.format(backend_name))
    add_project_arguments('-DIMV_BACKEND_@0@'.format(backend_name.to_upper()), language: 'c')
  endif
endforeach

bindir = get_option('bindir')
if window_system == 'all'
  executable(
    'imv-wayland',
    files_main + files_imv + files_wayland,
    dependencies: deps_imv + deps_wayland,
    install: true,
    install_dir: bindir
  )
  executable(
    'imv-x11',
    files_main + files_imv + files_x11,
    dependencies: deps_imv + deps_x11,
    install: true,
    install_dir: bindir
  )
  install_data(
    files('files/imv'),
    install_dir: bindir,
    install_mode: 'rwxr-xr-x'
  )
elif window_system == 'wayland'
  executable(
    'imv',
    files_main + files_imv + files_wayland,
    dependencies: deps_imv + deps_wayland,
    install: true,
    install_dir: bindir
  )
elif window_system == 'x11'
  executable(
    'imv',
    files_main + files_imv + files_x11,
    dependencies: deps_imv + deps_x11,
    install: true,
    install_dir: bindir
  )
endif

executable(
  'imv-msg',
  files('src/imv_msg.c', 'src/ipc_common.c'),
  install: true,
  install_dir: bindir,
)

desktop_list = ['imv']
man_list = [
  [1, 'imv'],
  [1, 'imv-msg'],
  [5, 'imv'],
]

if get_option('contrib-commands')
  install_data(
    files('contrib/imv-dir'),
    install_dir: bindir,
    install_mode: 'rwxr-xr-x',
  )

  desktop_list += ['imv-dir']
  man_list += [[1, 'imv-dir']]
endif

foreach desktop: desktop_list
  install_data(
    files('files/@0@.desktop'.format(desktop)),
    install_dir: '@0@/applications'.format(get_option('datadir')),
    install_mode: 'rw-r--r--',
  )
endforeach

install_data(
  files('files/imv_config'),
  install_dir: get_option('sysconfdir'),
  install_mode: 'rw-r--r--',
)

prog_a2x = find_program('a2x', required: get_option('man'))
if prog_a2x.found()
  foreach man : man_list
    section = man[0]
    topic = man[1]

    custom_target(
      '@0@(@1@)'.format(topic, section),
      input: 'doc/@0@.@1@.txt'.format(topic, section),
      output: '@0@.@1@'.format(topic, section),
      command: [
        prog_a2x,
        '--no-xmllint',
        '--doctype', 'manpage',
        '--format', 'manpage',
        '--destination-dir', meson.current_build_dir(),
        '@INPUT@'
      ],
      install: true,
      install_dir: '@0@/man@1@'.format(get_option('mandir'), section)
    )
  endforeach
endif


dep_cmocka = dependency('cmocka', required: get_option('test'))
if dep_cmocka.found()
  if window_system == 'all' or window_system == 'wayland'
    dep_gl = dependency('opengl', required: true)
  else
    dep_gl = dependency('gl', required: true)
  endif

  foreach test : ['list', 'navigator']
    test(
      'test_@0@'.format(test),
      executable(
        'test_@0@'.format(test),
        files('test/@0@.c'.format(test), 'src/dummy_window.c') + files_imv,
        include_directories: include_directories('src'),
        dependencies: deps_imv + dep_gl + dep_cmocka,
      )
    )
  endforeach
endif
