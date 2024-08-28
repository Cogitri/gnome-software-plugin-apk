project(
    'gnome-software-plugin-apk',
    ['c'],
    version: '0.13.0',
    meson_version: '>=0.58'
)

gnome_software_dep = dependency(
  'gnome-software',
  version: '>=46.0',
  fallback: ['gnome-software', 'libgnomesoftware_dep'],
  default_options: [
    'malcontent=false',
    'packagekit=false',
    'webapps=false',
    'fwupd=false',
    'hardcoded_foss_webapps=false',
    'hardcoded_proprietary_webapps=false',
    'external_appstream=true',
    'gtk_doc=false',
    'sysprof=disabled',
  ],
)
plugin_install_dir = gnome_software_dep.get_variable(
  'plugindir',
  default_value: '/usr/lib/gnome-software/plugins-21',
)

cargs = [
  '-DG_LOG_DOMAIN="GsPluginApk"',
  '-DI_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE',
  '-DGS_PLUGIN_APK_VERSION="@0@"'.format(meson.project_version()),
]

apk_dep = dependency('apk-polkit-client-2')
glib_dep = dependency('glib-2.0', version: '>=2.60')
gobject_dep = dependency('gobject-2.0')
gio_dep = dependency('gio-2.0')
appstream_dep = dependency('appstream')

plugin_apk_lib = shared_library(
  'gs_plugin_apk',
  sources : 'src/gs-plugin-apk/gs-plugin-apk.c',
  install : true,
  install_dir: plugin_install_dir,
  c_args : cargs,
  dependencies : [ gnome_software_dep, apk_dep, glib_dep, gobject_dep, gio_dep, appstream_dep ],
)

install_data(
  'data/org.gnome.Software.Plugin.Apk.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo'),
)

subdir('tests')
