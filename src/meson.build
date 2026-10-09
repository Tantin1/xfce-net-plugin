project('xfce-net-plugin', 'c',
  # Versión: única fuente en el archivo VERSION (también lo lee CMakeLists.txt
  # y llega al diálogo "Acerca de" como PACKAGE_VERSION).
  version : files('VERSION'),
  license : 'GPL-2.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'prefix=/usr',
  ],
  meson_version : '>= 0.59.0',
)

i18n  = import('i18n')
gnome = import('gnome')

# Dependencias
gtk3       = dependency('gtk+-3.0',         version : '>= 3.24')
xfce4panel = dependency('libxfce4panel-2.0', version : '>= 4.18')
intl       = dependency('intl')
libnotify  = dependency('libnotify',         version : '>= 0.7', required : false)
libqrencode = dependency('libqrencode',                                required : false)

# Ruta de instalación del plugin.
# Auto, en este orden:
#   1. <libdir del panel>/xfce4/panel/plugins, donde libdir lo informa la propia
#      biblioteca del panel (pkg-config). Sirve en cualquier arquitectura
#      (x86_64, ARM, etc.) y en distros con o sin carpetas por arquitectura.
#   2. /usr/lib/xfce4/panel/plugins si existe.
#   3. Si ninguna carpeta existe todavía, la del punto 1 (o prefix/lib).
# El panel encuentra el plugin por la ruta escrita en el .desktop, que se
# completa con esta misma ruta, así que cualquiera de las tres funciona.
# Forzar: meson setup build src -Dplugin_dir=/ruta/forzada
plugin_dir = get_option('plugin_dir')
if plugin_dir == ''
  panel_libdir = xfce4panel.get_variable(pkgconfig : 'libdir', default_value : '')
  candidates = []
  if panel_libdir != ''
    candidates += panel_libdir / 'xfce4/panel/plugins'
  endif
  candidates += '/usr/lib/xfce4/panel/plugins'
  foreach c : candidates
    if plugin_dir == '' and run_command('test', '-d', c, check : false).returncode() == 0
      plugin_dir = c
    endif
  endforeach
  if plugin_dir == ''
    plugin_dir = panel_libdir != '' ? panel_libdir / 'xfce4/panel/plugins' : get_option('prefix') / 'lib/xfce4/panel/plugins'
  endif
endif
message('Plugin install dir: ' + plugin_dir)

gettext_package = 'xfce-net-plugin'
locale_dir = get_option('prefix') / 'share/locale'

# Defines globales
add_project_arguments(
  '-DGETTEXT_PACKAGE="' + gettext_package + '"',
  '-DLOCALEDIR="' + locale_dir + '"',
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',
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
if libqrencode.found()
  plugin_deps += libqrencode
  add_project_arguments('-DHAVE_LIBQRENCODE', language : 'c')
  message('libqrencode: encontrada, códigos QR activados')
else
  message('libqrencode: NO encontrada, códigos QR desactivados')
endif

# Recurso embebido con los íconos propios del plugin (candado fallback)
lock_icons_resource = gnome.compile_resources(
  'lock-icons-resource',
  'icons/lock-icons.gresource.xml',
  source_dir : 'icons',
  c_name     : 'lock_icons',
)

# El módulo del plugin.
# CRÍTICO: name_prefix : 'lib' para que el .so se llame libxfce-net-plugin.so
# (sin esto, meson lo nombra xfce-net-plugin.so y Xfce no lo carga).
shared_module('xfce-net-plugin',
  [
    'plugin.c',
    'popup.c',
    'popup-rows.c',
    'popup-sections.c',
    'nm-dbus.c',
    'secret-agent.c',
    lock_icons_resource,
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

# Traducciones (.po → .mo automático, instalación en share/locale/<lang>/LC_MESSAGES)
subdir('po')
