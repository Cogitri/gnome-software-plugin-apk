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
/* gs_plugin_adopt_app_func (void) */
/* { */
/* } */

static void
gs_plugins_apk_repo_actions (GsPluginLoader *plugin_loader)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GsAppList) list = NULL;
  g_autoptr (GsAppQuery) query = NULL;
  GsApp *del_repo = NULL;
  gboolean rc;

  // Get apps which are sources
  query = gs_app_query_new ("is-source", GS_APP_QUERY_TRISTATE_TRUE, NULL);
  plugin_job = gs_plugin_job_list_apps_new (query, GS_PLUGIN_LIST_APPS_FLAGS_NONE);
  list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_nonnull (list);

  g_assert_cmpint (gs_app_list_length (list), ==, 3);
  for (int i = 0; i < gs_app_list_length (list); i++)
    {
      GsApp *repo = gs_app_list_index (list, i);
      const gchar *url = gs_app_get_url (repo, AS_URL_KIND_HOMEPAGE);
      g_autoptr (GsPlugin) plugin = GS_PLUGIN (gs_app_dup_management_plugin (repo));
      g_assert_cmpint (gs_app_get_kind (repo), ==, AS_COMPONENT_KIND_REPOSITORY);
      g_assert_cmpstr (gs_plugin_get_name (plugin), ==, "apk");
      if (g_str_equal (url, "https://pmos.org/pmos/master"))
        {
          g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_AVAILABLE);
        }
      else
        {
          g_assert_cmpint (gs_app_get_state (repo), ==, GS_APP_STATE_INSTALLED);
          del_repo = repo;
        }
    }

  // Remove repository
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_manage_repository_new (del_repo, GS_PLUGIN_MANAGE_REPOSITORY_FLAGS_REMOVE);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify repo status.
  // TODO: With a more complex DBusMock we could even check the count
  // Alternatively, we should check the logs that DBus got called
  g_assert_cmpint (gs_app_get_kind (del_repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (del_repo), ==, GS_APP_STATE_AVAILABLE);

  // gs_plugin_install_repo (reinstall it, check it works)
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_manage_repository_new (del_repo, GS_PLUGIN_MANAGE_REPOSITORY_FLAGS_INSTALL);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify repo status
  g_assert_cmpint (gs_app_get_kind (del_repo), ==, AS_COMPONENT_KIND_REPOSITORY);
  g_assert_cmpint (gs_app_get_state (del_repo), ==, GS_APP_STATE_INSTALLED);

  // Refresh repos.
  // TODO: Check logs!
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_refresh_metadata_new (G_MAXUINT64, GS_PLUGIN_REFRESH_METADATA_FLAGS_NONE);
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
  // * We would like that also some DESKTOP app is created. Do so
  //   by returning the package from the hard-coded desktop app in the
  //   updates.
  // * Execute update: Verify packages are updated? Needs Mock improvements!
  g_autoptr (GError) error = NULL;
  g_autoptr (GsAppQuery) query = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  GsApp *generic_app = NULL;
  GsApp *desktop_app = NULL;
  GsApp *system_app = NULL;
  g_autoptr (GsApp) foreign_app = NULL;
  g_autoptr (GsAppList) update_list = NULL;
  gboolean ret;
  GsAppList *related = NULL;

  // List updates
  query = gs_app_query_new ("is-for-update", GS_APP_QUERY_TRISTATE_TRUE,
                            "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS,
                            NULL);
  plugin_job = gs_plugin_job_list_apps_new (query, GS_PLUGIN_LIST_APPS_FLAGS_NONE);
  update_list = gs_plugin_loader_job_process (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_nonnull (update_list);

  g_assert_cmpint (gs_app_list_length (update_list), ==, 2);
  // Check desktop app
  desktop_app = gs_app_list_index (update_list, 0);
  g_assert_nonnull (desktop_app);
  g_assert_false (gs_app_has_quirk (desktop_app, GS_APP_QUIRK_IS_PROXY));
  g_assert_cmpstr (gs_app_get_name (desktop_app), ==, "ApkTestApp");
  g_assert_cmpint (gs_app_get_state (desktop_app), ==, GS_APP_STATE_UPDATABLE_LIVE);
  // Check generic proxy app
  generic_app = gs_app_list_index (update_list, 1);
  g_assert_nonnull (generic_app);
  g_assert_true (gs_app_has_quirk (generic_app, GS_APP_QUIRK_IS_PROXY));
  related = gs_app_get_related (generic_app);
  g_assert_cmpint (gs_app_list_length (related), ==, 1);
  system_app = gs_app_list_index (related, 0);
  g_assert_cmpint (gs_app_get_state (system_app), ==, GS_APP_STATE_UPDATABLE_LIVE);

  // Add app that shouldn't be updated
  foreign_app = gs_app_new ("foreign");
  gs_app_set_state (foreign_app, GS_APP_STATE_UPDATABLE_LIVE);
  gs_app_list_add (update_list, foreign_app); // No management plugin, should get ignored!
  // Execute update!
  g_object_unref (plugin_job);
  plugin_job = gs_plugin_job_update_apps_new (update_list,
                                              GS_PLUGIN_UPDATE_APPS_FLAGS_NO_DOWNLOAD);
  ret = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (ret);

  // Check desktop app: TODO: Check logs!
  g_assert_cmpint (gs_app_get_state (desktop_app), ==, GS_APP_STATE_INSTALLED);
  // Check generic proxy app: TODO: Check logs!
  g_assert_true (gs_app_has_quirk (generic_app, GS_APP_QUIRK_IS_PROXY));
  g_assert_cmpint (gs_app_get_state (generic_app), ==, GS_APP_STATE_INSTALLED);
  related = gs_app_get_related (generic_app);
  g_assert_cmpint (gs_app_list_length (related), ==, 1);
  system_app = gs_app_list_index (related, 0);
  g_assert_cmpint (gs_app_get_state (system_app), ==, GS_APP_STATE_INSTALLED);
  // Check foreign app: As it was!
  g_assert_cmpint (gs_app_get_state (foreign_app), ==, GS_APP_STATE_UPDATABLE_LIVE);
}

static void
gs_plugins_apk_app_install_remove (GsPluginLoader *plugin_loader)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GsApp) app = NULL;
  g_autoptr (GsAppList) list = gs_app_list_new ();
  g_autoptr (GsPlugin) plugin = NULL;
  g_autoptr (GsAppQuery) query = NULL;
  const char *keywords[2] = { "apk-test", NULL };
  gboolean rc;

  // Search for a non-installed app
  query = gs_app_query_new ("keywords", keywords,
                            // We force refine to take ownership
                            "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION,
                            NULL);
  plugin_job = gs_plugin_job_list_apps_new (query, GS_PLUGIN_LIST_APPS_FLAGS_NONE);
  app = gs_plugin_loader_job_process_app (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert (app != NULL);
  plugin = GS_PLUGIN (gs_app_dup_management_plugin (app));

  // make sure we got the correct app and is managed by us
  g_assert_cmpstr (gs_app_get_id (app), ==, "apk-test-app.desktop");
  g_assert_cmpstr (gs_plugin_get_name (plugin), ==, "apk");
  g_assert_cmpint (gs_app_get_kind (app), ==, AS_COMPONENT_KIND_DESKTOP_APP);
  g_assert_cmpint (gs_app_get_scope (app), ==, AS_COMPONENT_SCOPE_SYSTEM);
  g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);

  // execute installation action
  g_object_unref (plugin_job);
  gs_app_list_add (list, app);
  plugin_job = gs_plugin_job_install_apps_new (list,
                                               GS_PLUGIN_INSTALL_APPS_FLAGS_NONE);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify app is now installed
  g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_INSTALLED);

  // Execute remove action
  gs_app_list_remove_all (list);
  g_object_unref (plugin_job);
  gs_app_list_add (list, app);
  plugin_job = gs_plugin_job_uninstall_apps_new (list,
                                                 GS_PLUGIN_UNINSTALL_APPS_FLAGS_NONE);
  rc = gs_plugin_loader_job_action (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert_true (rc);

  // Verify app is now removed
  g_assert_cmpint (gs_app_get_state (app), ==, GS_APP_STATE_AVAILABLE);
}

static void
gs_plugins_apk_refine_app_missing_source (GsPluginLoader *plugin_loader)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GsPluginJob) plugin_job = NULL;
  g_autoptr (GsApp) app = NULL;
  g_autoptr (GsAppQuery) query = NULL;
  const char *keywords[2] = { "no-source", NULL };
  g_autoptr (GsPlugin) plugin = NULL;

  // Search for a non-installed app. Use a refine flag not being used
  // to force the run of the refine, but only fix the missing source
  query = gs_app_query_new ("keywords", keywords,
                            "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_KUDOS,
                            NULL);
  plugin_job = gs_plugin_job_list_apps_new (query, GS_PLUGIN_LIST_APPS_FLAGS_NONE);
  app = gs_plugin_loader_job_process_app (plugin_loader, plugin_job, NULL, &error);
  gs_test_flush_main_context ();
  g_assert_no_error (error);
  g_assert (app != NULL);
  plugin = GS_PLUGIN (gs_app_dup_management_plugin (app));
  g_assert_nonnull (plugin);

  // make sure we got the correct app, is managed by us and has the source set
  g_assert_cmpstr (gs_app_get_id (app), ==, "no-source-app.desktop");
  g_assert_cmpstr (gs_plugin_get_name (plugin), ==, "apk");
  g_assert_nonnull (gs_app_get_source_default (app));
}

int
main (int argc, char **argv)
{
  g_autofree gchar *xml = NULL;
  g_autofree gchar *tmp_root = NULL;
  g_autoptr (GsPluginLoader) plugin_loader = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GDBusConnection) bus_connection = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ret;
  int retval;
  const gchar *allowlist[] = {
    "apk",
    "generic-updates",
    "appstream",
    NULL
  };

  gs_test_init (&argc, &argv);

  settings = g_settings_new ("org.gnome.software");
  /* We do not want real data to pollute tests.
   * Might be useful at some point though */
  g_assert_true (g_settings_set_strv (settings, "external-appstream-urls", NULL));

  g_setenv ("GS_XMLB_VERBOSE", "1", TRUE);

  /* Adapted from upstream dummy/gs-self-test.c */
  xml = g_strdup ("<?xml version=\"1.0\"?>\n"
                  "<components origin=\"alpine-test\" version=\"0.9\">\n"
                  "  <component type=\"desktop\">\n"
                  "    <id>apk-test-app.desktop</id>\n"
                  "    <name>ApkTestApp</name>\n"
                  "    <summary>Alpine Package Keeper test app</summary>\n"
                  "    <pkgname>apk-test-app</pkgname>\n"
                  "  </component>\n"
                  "  <component type=\"desktop\">\n"
                  "    <id>no-source-app.desktop</id>\n"
                  "    <name>NoSourceApp</name>\n"
                  "    <summary>App with missing source in metadata</summary>\n"
                  "    <info>\n"
                  "      <filename>/usr/share/apps/no-source-app.desktop</filename>\n"
                  "    </info>\n"
                  "  </component>\n"
                  "  <info>\n"
                  "    <scope>system</scope>\n"
                  "  </info>\n"
                  "</components>\n");
  g_setenv ("GS_SELF_TEST_APPSTREAM_XML", xml, TRUE);

  /* Needed for appstream plugin to store temporary data! */
  tmp_root = g_dir_make_tmp ("gnome-software-apk-test-XXXXXX", NULL);
  g_assert_true (tmp_root != NULL);
  g_setenv ("GS_SELF_TEST_CACHEDIR", tmp_root, TRUE);

  bus_connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);
  /* we can only load this once per process */
  plugin_loader = gs_plugin_loader_new (bus_connection, bus_connection);
  /* g_signal_connect (plugin_loader, "status-changed", */
  /*                   G_CALLBACK (gs_plugin_loader_status_changed_cb), NULL); */
  gs_plugin_loader_add_location (plugin_loader, LOCALPLUGINDIR);
  gs_plugin_loader_add_location (plugin_loader, SYSTEMPLUGINDIR);
  ret = gs_plugin_loader_setup (plugin_loader,
                                allowlist,
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
  g_test_add_data_func ("/gnome-software/plugins/apk/app-install-remove",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_app_install_remove);
  g_test_add_data_func ("/gnome-software/plugins/apk/updates",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_updates);
  g_test_add_data_func ("/gnome-software/plugins/apk/missing-source",
                        plugin_loader,
                        (GTestDataFunc) gs_plugins_apk_refine_app_missing_source);
  retval = g_test_run ();

  /* Clean up. */
  gs_utils_rmtree (tmp_root, NULL);

  return retval;
}
