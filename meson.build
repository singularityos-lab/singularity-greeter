project('singularity-greeter', 'c',
  version: '0.1.0',
  meson_version: '>= 0.59.0',
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

wayland_scanner = find_program('wayland-scanner')

wl_client_dep = dependency('wayland-client')
xkb_dep       = dependency('xkbcommon')
glib_dep      = dependency('glib-2.0')
gio_dep       = dependency('gio-2.0')
json_dep      = dependency('json-glib-1.0')
fc_dep        = dependency('fontconfig')

loginui_dep = dependency('singularity-loginui',
  fallback: ['singularity-loginui', 'loginui_dep']
)

xdg_c = custom_target('xdg-shell-protocol.c',
  output: 'xdg-shell-protocol.c', input: 'protocols/xdg-shell.xml',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
xdg_h = custom_target('xdg-shell-client-protocol.h',
  output: 'xdg-shell-client-protocol.h', input: 'protocols/xdg-shell.xml',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

ls_c = custom_target('wlr-layer-shell-unstable-v1-protocol.c',
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
ls_h = custom_target('wlr-layer-shell-unstable-v1-client-protocol.h',
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

executable('singularity-greeter',
  ['src/greeter_main.c', xdg_c, xdg_h, ls_c, ls_h],
  dependencies: [wl_client_dep, xkb_dep, glib_dep, gio_dep, json_dep, loginui_dep, fc_dep],
  install: true,
)

install_data('data/dev.sinty.greeter.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas'
)
