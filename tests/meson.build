gnome = import('gnome')

test_c_args = [
  '-DLOCALPLUGINDIR="' + meson.project_build_root() + '"',
  '-DSYSTEMPLUGINDIR="' + plugin_install_dir + '"',
]

test = executable(
  'gs-self-test-apk',
  sources : 'gs-self-test.c',
  c_args : [cargs + test_c_args],
  dependencies : [ gnome_software_dep, glib_dep, gobject_dep, gio_dep ],
  link_with : [ plugin_apk_lib ],
)

test_env = [
  'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
  'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
  'MALLOC_CHECK_=2',
  'DBUS_TEST_LOG=@0@/@1@'.format(meson.current_build_dir(), 'dbus-test.log'),
]

test_program = find_program('test.py', dirs : [meson.current_source_dir()])
test('gs-self-test-apk', test_program, env : test_env)
