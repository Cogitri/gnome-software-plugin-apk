/*
 * Copyright (C) 2022 Pablo Correa Gomez <ablocorrea@hotmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <gnome-software.h>
#include <stdio.h>

#include <gs-plugin-loader-sync.h>
#include <gs-plugin-loader.h>
#include <gs-test.h>

/* static void */
/* gs_plugin_adopt_app_func (void) */
/* { */
/* } */

/* static void */
/* gs_plugin_refine_func (void) */
/* { */
/* } */

static void
gs_plugins_apk_repo_actions (GsPluginLoader *plugin_loader)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GsAppList) list = NULL;
  GsApp *repo = NULL;
  gboolean rc;

  // Execute get sources action
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_SOURCES, NULL);
  list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_nonnull (list);

  // Verify correctness of result. TODO: for loop and check app name
  g_assert_cmpint (gs_app_list_length (list), ==, 3);
  repo = gs_app_list_index (list, 0);
  g_assert_cmpint (gs_app_get_kind (repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_INSTALLED);
  g_assert_cmpstr (gs_app_get_management_plugin (repo), ==, "apk");
  repo = gs_app_list_index (list, 1);
  g_assert_cmpint (gs_app_get_kind (repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_AVAILABLE);
  g_assert_cmpstr (gs_app_get_management_plugin (repo), ==, "apk");
  repo = gs_app_list_index (list, 2);
  g_assert_cmpint (gs_app_get_kind (repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_INSTALLED);
  g_assert_cmpstr (gs_app_get_management_plugin (repo), ==, "apk");

  // Remove repository
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REMOVE_REPO,
                                   "app", repo,
                                   NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify repo status.
  // TODO: With a more complex DBusMock we could even check the count
  // Alternatively, we should check the logs that DBus got called
  g_assert_cmpint (gs_app_get_kind (repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_AVAILABLE);

  // gs_plugin_install_repo (reinstall it, check it works)
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_INSTALL_REPO,
                                   "app", repo,
                                   NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify repo status
  g_assert_cmpint (gs_app_get_kind (repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_INSTALLED);

  // Refresh repos.
  // TODO: Check logs!
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REFRESH, NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);
}

static void
gs_plugins_apk_updates (GsPluginLoader *plugin_loader)
{
  // This is certainly the most complex test
  // Steps:
  // * Add updates should return upgradable and a downgradable
  //   packages. This could be extended in the future.
  // * We should enable generic updates plugin and verify that
  //   the proxy app is created.
  // * We would like that also some DESKTOP app is created. Either
  //   we hack something or enable the appstream plugin.
  // * Execute update: Verify packages are updated? Needs Mock improvements!
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  GsApp *generic_app = NULL;
  GsApp *desktop_app = NULL;
  GsApp *system_app = NULL;
  g_autoptr (GsApp) foreign_app = NULL;
  g_autoptr (GsAppList) update_list = NULL;
  g_autoptr (GsAppList) updated_list = NULL;
  GsAppList *related = NULL;

  // List updates
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_UPDATES,
                                   "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS,
                                   NULL);
  update_list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_nonnull (update_list);

  g_assert_cmpint (gs_app_list_length (update_list), ==, 2);
  // Check desktop app
  desktop_app = gs_app_list_index (update_list, 0);
  g_assert_nonnull (desktop_app);
  g_assert_cmpint (gs_app_get_state (desktop_app), ==, GS_APP_STATE_UPDATABLE_LIVE);
  // Check generic proxy app
  generic_app = gs_app_list_index (update_list, 1);
  g_assert_nonnull (generic_app);
  g_assert_true (gs_app_has_quirk (generic_app, GS_APP_QUIRK_IS_PROXY));
  related = gs_app_get_related (generic_app);
  g_assert_cmpint (gs_app_list_length (related), ==, 1);
  system_app = gs_app_list_index (related, 0);
  g_assert_cmpint (gs_app_get_state (system_app), ==, GS_APP_STATE_UPDATABLE_LIVE);

  // Execute update!
  foreign_app = gs_app_new ("foreign");
  gs_app_list_add (update_list, foreign_app); // No management plugin, should get ignored!
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_UPDATE,
                                   "list", update_list,
                                   NULL);
  updated_list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_nonnull (updated_list);

  // Check desktop app: TODO: Check logs!
  desktop_app = gs_app_list_index (updated_list, 0);
  g_assert_nonnull (desktop_app);
  g_assert_cmpint (gs_app_get_state (desktop_app), ==, GS_APP_STATE_INSTALLED);
  // Check generic proxy app: TODO: Check logs!
  generic_app = gs_app_list_index (updated_list, 1);
  g_assert_true (gs_app_has_quirk (generic_app, GS_APP_QUIRK_IS_PROXY));
  related = gs_app_get_related (generic_app);
  g_assert_cmpint (gs_app_list_length (related), ==, 1);
  system_app = gs_app_list_index (related, 0);
  g_assert_cmpint (gs_app_get_state (system_app), ==, GS_APP_STATE_INSTALLED);
}

static void
gs_plugins_apk_app_install_remove (GsPluginLoader *plugin_loader)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GsApp) app = NULL;
  gboolean rc;

  // Search for a non-installed app
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_SEARCH,
                                   "search", "apk-test",
                                   // We force refine to take ownership
                                   "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION,
                                   NULL);
  app = gs_plugin_loader_job_process_app (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert (app != NULL);

  // make sure we got the correct app and is managed by us
  g_assert_cmpstr (gs_app_get_id (app), ==, "apk-test-app.desktop");
  g_assert_cmpstr (gs_app_get_management_plugin (app), ==, "apk");
  g_assert_cmpint (gs_app_get_kind (app), ==, AS_COMPONENT_KIND_DESKTOP_APP);
  g_assert_cmpint (gs_app_get_scope (app), ==, AS_COMPONENT_SCOPE_SYSTEM);
  g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);

  // Execute installation action
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_INSTALL,
                                   "app", app,
                                   NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify app is now installed
  g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_INSTALLED);

  // Execute remove action
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REMOVE,
                                   "app", app,
                                   NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify app is now removed
  g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);
}

static void
gs_plugins_apk_timeout (GsPluginLoader *plugin_loader)
{
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GsApp) app = NULL;
  gboolean rc;

  g_autoptr (GsPlugin) plugin = gs_plugin_loader_find_plugin (plugin_loader, "apk");

  app = gs_app_new ("cancel.test");

  /* gs_app_set_management_plugin (app, "apk"); */
  gs_app_set_management_plugin (app, plugin);
  gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
  gs_app_add_source (app, "slow");

  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_INSTALL,
                                   "app", app,
                                   "timeout", 2,
                                   NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, cancellable, &error);

  gs_test_flush_main_context ();
  g_assert_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_TIMED_OUT);
  g_assert_false (rc);
}

int
main (int argc, char **argv)
{
  g_autofree gchar *xml = NULL;
  g_autofree gchar *tmp_root = NULL;
  g_autoptr (GsPluginLoader) plugin_loader = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ret;
  int retval;
  const gchar *allowlist[] = {
    "apk",
    "generic-updates",
    "appstream",
    NULL
  };

  settings = g_settings_new ("org.gnome.software");
  /* Avoid connections to review server during tests */
  g_assert_true (g_settings_set_string (settings, "review-server", ""));
  /* We do not want real data to pollute tests.
   * Might be useful at some point though */
  g_assert_true (g_settings_set_strv (settings, "external-appstream-urls", NULL));

  g_setenv ("GS_XMLB_VERBOSE", "1", TRUE);

  /* Adapted from upstream dummy/gs-self-test.c */
  xml = g_strdup ("<?xml version=\"1.0\"?>\n"
                  "<components origin=\"alpine-test\" version=\"0.9\">\n"
                  "  <component type=\"desktop\">\n"
                  "    <id>apk-test-app.desktop</id>\n"
                  "    <name>apk-test-app</name>\n"
                  "    <summary>Alpine Package Keeper test app</summary>\n"
                  "    <pkgname>apk-test-app</pkgname>\n"
                  "  </component>\n"
                  "  <info>\n"
                  "    <scope>system</scope>\n"
                  "  </info>\n"
                  "</components>\n");
  g_setenv ("GS_SELF_TEST_APPSTREAM_XML", xml, TRUE);
  g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

  /* Needed for appstream plugin to store temporary data! */
  tmp_root = g_dir_make_tmp ("gnome-software-apk-test-XXXXXX", NULL);
  g_assert_true (tmp_root != NULL);
  g_setenv ("GS_SELF_TEST_CACHEDIR", tmp_root, TRUE);

  g_test_init (&argc, &argv,
               G_TEST_OPTION_ISOLATE_DIRS,
               NULL);

  /* only critical and error are fatal */
  g_log_set_fatal_mask (NULL, G_LOG_LEVEL_WARNING |
                                  G_LOG_LEVEL_ERROR |
                                  G_LOG_LEVEL_CRITICAL);

  /* we can only load this once per process */
  plugin_loader = gs_plugin_loader_new ();
  /* g_signal_connect (plugin_loader, "status-changed", */
  /*                   G_CALLBACK (gs_plugin_loader_status_changed_cb), NULL); */
  gs_plugin_loader_add_location (plugin_loader, LOCALPLUGINDIR);
  gs_plugin_loader_add_location (plugin_loader, SYSTEMPLUGINDIR);
  ret = gs_plugin_loader_setup (plugin_loader,
                                (gchar **) allowlist,
                                NULL,
                                NULL,
                                &error);
  g_assert_no_error (error);
  g_assert_true (ret);
  g_assert_true (gs_plugin_loader_get_enabled (plugin_loader, "apk"));
  g_assert_true (gs_plugin_loader_get_enabled (plugin_loader, "generic-updates"));
  g_assert_true (gs_plugin_loader_get_enabled (plugin_loader, "appstream"));

  g_test_add_data_func ("/gnome-software/plugins/apk/repo-actions",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_repo_actions);
  g_test_add_data_func ("/gnome-software/plugins/apk/updates",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_updates);
  g_test_add_data_func ("/gnome-software/plugins/apk/app-install-remove",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_app_install_remove);
  g_test_add_data_func ("/gnome-software/plugins/apk/cancel",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_timeout);
  retval = g_test_run ();

  /* Clean up. */
  gs_utils_rmtree (tmp_root, NULL);

  return retval;
}
