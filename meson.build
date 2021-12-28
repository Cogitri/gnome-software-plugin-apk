project(
    'gnome-software-plugin-apk',
    ['c'],
    version: '0.9.0',
    meson_version: '>=0.51'
)

gnome_software_dep = dependency('gnome-software', version: '>=40.0')
plugin_install_dir = gnome_software_dep.get_pkgconfig_variable('plugindir')

cargs = ['-DG_LOG_DOMAIN="GsPluginApk"', '-DI_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE']

apk_dep = dependency('apk-polkit-client-1')
glib_dep = dependency('glib-2.0')
gobject_dep = dependency('gobject-2.0')
gio_dep = dependency('gio-2.0')

shared_library(
  'gs_plugin_apk',
  sources : 'src/gs-plugin-apk/gs-plugin-apk.c',
  install : true,
  install_dir: plugin_install_dir,
  c_args : cargs,
  dependencies : [ gnome_software_dep, apk_dep, glib_dep,  gobject_dep, gio_dep ],
)

install_data(
  'data/org.gnome.Software.Plugin.Apk.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo'),
)
