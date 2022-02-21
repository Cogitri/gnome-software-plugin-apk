/*
 * Copyright (C) 2022 Pablo Correa Gomez <ablocorrea@hotmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <gnome-software.h>

#include <gs-plugin-loader-sync.h>
#include <gs-plugin-loader.h>
#include <gs-test.h>

/* static void */
/* gs_plugin_add_updates_func (void) */
/* { */
/* } */

// Update app list
/* static void */
/* gs_plugin_update_func (void) */
/* { */
/* } */

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
  plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REFRESH, NULL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);
}

static void
gs_plugins_apk_app_install_remove (GsPluginLoader *plugin_loader)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GsApp) app = NULL;
  gboolean rc;

  // Create installable app
  app = gs_app_new ("dev.test");
  gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
  gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
  gs_app_set_metadata (app, "apk::name", "test");
  gs_app_set_management_plugin (app, "apk");

  // Execute installation action
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

int
main (int argc, char **argv)
{
  g_autoptr (GsPluginLoader) plugin_loader = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ret;
  int retval;
  const gchar *allowlist[] = {
    "apk",
    NULL
  };

  settings = g_settings_new ("org.gnome.software");
  /* Avoid connections to review server during tests */
  g_assert_true (g_settings_set_string (settings, "review-server", ""));
  /* We do not want real data to pollute tests.
   * Might be useful at some point though */
  g_assert_true (g_settings_set_strv (settings, "external-appstream-urls", NULL));

  g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

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
  ret = gs_plugin_loader_setup (plugin_loader,
                                (gchar **) allowlist,
                                NULL,
                                NULL,
                                &error);
  g_assert_no_error (error);
  g_assert_true (ret);
  g_assert_true (gs_plugin_loader_get_enabled (plugin_loader, "apk"));

  g_test_add_data_func ("/gnome-software/plugins/apk/repo-actions",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_repo_actions);
  g_test_add_data_func ("/gnome-software/plugins/apk/app-install-remove",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_app_install_remove);

  retval = g_test_run ();

  /* Clean up. */ /* Probably not needed */
  /* gs_utils_rmtree (tmp_root, NULL); */

  return retval;
}
