project('xfce-net-plugin', 'c',
  version : '0.1.0',
  license : 'GPL-2.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'prefix=/usr',
  ],
  meson_version : '>= 0.59.0',
)

i18n = import('i18n')

# Dependencias
gtk3       = dependency('gtk+-3.0',         version : '>= 3.24')
xfce4panel = dependency('libxfce4panel-2.0', version : '>= 4.18')
intl       = dependency('intl')
libnotify  = dependency('libnotify',         version : '>= 0.7', required : false)

# Ruta de instalación del plugin
# Auto: si existe el directorio multilib de Debian/Loc OS, lo usa. Si no, /usr/lib/xfce4/panel/plugins.
# Override: meson setup build src -Dplugin_dir=/ruta/forzada
plugin_dir = get_option('plugin_dir')
if plugin_dir == ''
  multilib = '/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins'
  test_multilib = run_command('test', '-d', multilib, check : false)
  if test_multilib.returncode() == 0
    plugin_dir = multilib
  else
    plugin_dir = get_option('prefix') / 'lib/xfce4/panel/plugins'
  endif
endif
message('Plugin install dir: ' + plugin_dir)

gettext_package = 'xfce-net-plugin'
locale_dir = get_option('prefix') / 'share/locale'

# Defines globales
add_project_arguments(
  '-DGETTEXT_PACKAGE="' + gettext_package + '"',
  '-DLOCALEDIR="' + locale_dir + '"',
  language : 'c',
)

plugin_deps = [gtk3, xfce4panel, intl]
if libnotify.found()
  plugin_deps += libnotify
  add_project_arguments('-DHAVE_LIBNOTIFY', language : 'c')
  message('libnotify: encontrada, notificaciones activadas')
else
  message('libnotify: NO encontrada, notificaciones desactivadas')
endif

# El módulo del plugin.
# CRÍTICO: name_prefix : 'lib' para que el .so se llame libxfce-net-plugin.so
# (sin esto, meson lo nombra xfce-net-plugin.so y Xfce no lo carga).
shared_module('xfce-net-plugin',
  [
    'plugin.c',
    'popup.c',
    'nm-dbus.c',
  ],
  name_prefix     : 'lib',
  dependencies    : plugin_deps,
  install         : true,
  install_dir     : plugin_dir,
)

# Desktop entry del plugin
desktop_conf = configuration_data()
desktop_conf.set('plugin_dir', plugin_dir)

configure_file(
  input         : 'xfce-net-plugin.desktop.in',
  output        : 'xfce-net-plugin.desktop',
  configuration : desktop_conf,
  install       : true,
  install_dir   : get_option('prefix') / 'share/xfce4/panel/plugins',
)

# Ícono de candado fallback
install_data(
  'icons/nm-secure-lock.svg',
  install_dir : get_option('prefix') / 'share/icons/hicolor/scalable/apps',
)

# Traducciones (.po → .mo automático, instalación en share/locale/<lang>/LC_MESSAGES)
subdir('po')
