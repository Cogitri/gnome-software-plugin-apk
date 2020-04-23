/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2020 Rasmus Thomsen <oss@cogitri.dev>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <apk-polkit/apkd-dbus-client.h>
#include <gnome-software.h>
#include <libintl.h>
#include <locale.h>
#define _(string) gettext (string)

/**
  Helper struct holding the current app (used to set progress) and
  the GDBusProxy used to communicate with apk-polkit
*/
struct GsPluginData
{
  GsApp *current_app;
  ApkdHelper *proxy;
};

typedef struct
{
  const gchar *m_name;
  const gchar *m_version;
  const gchar *m_oldVersion;
  const gchar *m_arch;
  const gchar *m_license;
  const gchar *m_origin;
  const gchar *m_maintainer;
  const gchar *m_url;
  const gchar *m_description;
  const gchar *m_commit;
  const gchar *m_filename;
  gulong m_installedSize;
  gulong m_size;
  glong m_buildTime;
  gboolean m_isInstalled;
} ApkdPackage;

static ApkdPackage
g_variant_to_apkd_package (GVariant *value_tuple)
{
  ApkdPackage pkg = {
    g_variant_get_string (g_variant_get_child_value (value_tuple, 0), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 1), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 2), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 3), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 4), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 5), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 6), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 7), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 8), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 9), NULL),
    g_variant_get_string (g_variant_get_child_value (value_tuple, 10), NULL),
    g_variant_get_uint64 (g_variant_get_child_value (value_tuple, 11)),
    g_variant_get_uint64 (g_variant_get_child_value (value_tuple, 12)),
    g_variant_get_int64 (g_variant_get_child_value (value_tuple, 13)),
    g_variant_get_boolean (g_variant_get_child_value (value_tuple, 14)),
  };
  return pkg;
}

static GsApp *
apk_package_to_app (ApkdPackage *pkg)
{
  GsApp *app;

  app = gs_app_new (pkg->m_name);

  gs_app_set_kind (app, AS_APP_KIND_GENERIC);
  gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
  gs_app_set_allow_cancel (app, FALSE);
  gs_app_add_source (app, pkg->m_name);
  gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, pkg->m_name);
  gs_app_set_version (app, pkg->m_oldVersion ? pkg->m_oldVersion : pkg->m_version);
  gs_app_set_update_version (app, pkg->m_version);
  gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, pkg->m_description);
  gs_app_set_description (app, GS_APP_QUALITY_UNKNOWN, pkg->m_description);
  gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, pkg->m_url);
  gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, pkg->m_license);
  gs_app_set_origin (app, "alpine");
  gs_app_set_origin_hostname (app, "alpinelinux.org");
  gs_app_set_management_plugin (app, "apk");
  gs_app_set_metadata (app, "apk::name", pkg->m_name);
  gs_app_set_size_installed (app, pkg->m_installedSize);
  gs_app_set_size_download (app, pkg->m_size);
  gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE);
  if (pkg->m_isInstalled)
    {
      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
    }
  else
    {
      gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
    }

  return app;
}

static void
apk_progress_signal_connect_callback (GDBusProxy *proxy,
                                      gchar *sender_name,
                                      gchar *signal_name,
                                      GVariant *parameters,
                                      gpointer user_data)
{
  GsPluginData *priv = gs_plugin_get_data ((GsPlugin *) user_data);
  GsPluginStatus plugin_status = GS_PLUGIN_STATUS_DOWNLOADING;
  uint percentage =
      g_variant_get_uint32 (g_variant_get_child_value (parameters, 0));

  /* nothing in progress */
  if (priv->current_app != NULL)
    {
      g_debug ("apk percentage for %s: %u%%",
               gs_app_get_unique_id (priv->current_app), percentage);
      gs_app_set_progress (priv->current_app, percentage);

      switch (gs_app_get_state (priv->current_app))
        {
        case AS_APP_STATE_INSTALLING:
          plugin_status = GS_PLUGIN_STATUS_INSTALLING;
          break;
        case AS_APP_STATE_REMOVING:
          plugin_status = GS_PLUGIN_STATUS_REMOVING;
          break;
        default:
          break;
        }
    }

  gs_plugin_status_update ((GsPlugin *) user_data, priv->current_app, plugin_status);
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
  GsPluginData *priv;

  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "icons");
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
  gs_plugin_alloc_data (plugin, sizeof (GsPluginData));
  priv = gs_plugin_get_data (plugin);
  priv->current_app = NULL;
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  priv->proxy = apkd_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    "dev.Cogitri.apkPolkit.Helper",
                                                    "/dev/Cogitri/apkPolkit/Helper",
                                                    cancellable,
                                                    &local_error);

  if (local_error != NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      return FALSE;
    }

  g_signal_connect (priv->proxy, "g-signal", G_CALLBACK (apk_progress_signal_connect_callback), plugin);

  return TRUE;
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
                   guint cache_age,
                   GCancellable *cancellable,
                   GError **error)
{
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  GsApp *app_dl = gs_app_new (gs_plugin_get_name (plugin));
  priv->current_app = app_dl;

  gs_app_set_summary_missing (app_dl, _ ("Getting apk repository indexes…"));
  gs_plugin_status_update (plugin, app_dl, GS_PLUGIN_STATUS_DOWNLOADING);
  if (apkd_helper_call_update_repositories_sync (priv->proxy, cancellable, &local_error))
    {
      gs_app_set_progress (app_dl, 100);
      priv->current_app = NULL;
      return TRUE;
    }
  else
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      priv->current_app = NULL;
      return FALSE;
    }
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
                       GsAppList *list,
                       GCancellable *cancellable,
                       GError **error)
{
  GVariant *upgradable_packages = NULL;
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  if (!apkd_helper_call_list_upgradable_packages_sync (priv->proxy, &upgradable_packages, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      return FALSE;
    }

  for (gsize i = 0; i < g_variant_n_children (upgradable_packages); i++)
    {
      GsApp *app;
      GVariant *value_tuple;
      ApkdPackage pkg;

      value_tuple = g_variant_get_child_value (upgradable_packages, i);
      pkg = g_variant_to_apkd_package (value_tuple);
      app = apk_package_to_app (&pkg);
      gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
      gs_app_set_kind (app, AS_APP_KIND_OS_UPDATE);
      gs_app_list_add (list, app);
    }

  return TRUE;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
                         GsAppList *list,
                         GCancellable *cancellable,
                         GError **error)
{
  GVariant *installed_packages = NULL;
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  if (!apkd_helper_call_list_installed_packages_sync (priv->proxy, &installed_packages, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      return FALSE;
    }

  for (gsize i = 0; i < g_variant_n_children (installed_packages); i++)
    {
      GsApp *app;
      GVariant *value_tuple;
      ApkdPackage pkg;

      value_tuple = g_variant_get_child_value (installed_packages, i);
      pkg = g_variant_to_apkd_package (value_tuple);
      app = apk_package_to_app (&pkg);
      gs_app_list_add (list, app);
    }

  return TRUE;
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
                       GsApp *app,
                       GCancellable *cancellable,
                       GError **error)
{
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  const gchar *app_name[2];

  /* We can only install apps we know of */
  if (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0)
    return TRUE;

  priv->current_app = app;
  app_name[0] = gs_app_get_metadata_item (app, "apk::name");
  /* FIXME: Properly zero terminate the array */
  app_name[1] = '\0';
  gs_app_set_state (app, AS_APP_STATE_INSTALLING);

  if (!apkd_helper_call_add_packages_sync (priv->proxy, app_name, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      gs_app_set_state_recover (app);
      priv->current_app = NULL;
      return FALSE;
    }

  gs_app_set_state (app, AS_APP_STATE_INSTALLED);
  priv->current_app = NULL;
  return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
                      GsApp *app,
                      GCancellable *cancellable,
                      GError **error)
{
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  const gchar *app_name[2];

  /* We can only remove apps we know of */
  if (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0)
    return TRUE;

  priv->current_app = app;
  app_name[0] = gs_app_get_metadata_item (app, "apk::name");
  /* FIXME: Properly zero terminate the array */
  app_name[1] = '\0';
  gs_app_set_state (app, AS_APP_STATE_REMOVING);

  if (!apkd_helper_call_delete_packages_sync (priv->proxy, app_name, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      gs_app_set_state_recover (app);
      priv->current_app = NULL;
      return FALSE;
    }

  gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
  priv->current_app = NULL;
  return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
                      gchar **values,
                      GsAppList *list,
                      GCancellable *cancellable,
                      GError **error)
{
  GError *local_error = NULL;
  GVariant *search_result = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  if (!apkd_helper_call_search_package_names_sync (priv->proxy, (const gchar *const *) values, &search_result, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      return FALSE;
    }

  for (guint i = 0; i < g_variant_n_children (search_result); i++)
    {
      GVariant *pkg_val = g_variant_get_child_value (search_result, i);
      ApkdPackage package = g_variant_to_apkd_package (pkg_val);
      GsApp *app = apk_package_to_app (&package);
      /*
        FIXME: We currently can't tell if an app is a desktop app or not due to limitations
        in apk and Software likes GENERIC apps, so let's just set all to AS_APP_KIND_DESKTOP
      */
      gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
      gs_app_list_add (list, app);
    }

  return TRUE;
}

gboolean
gs_plugin_update (GsPlugin *plugin,
                  GsAppList *apps,
                  GCancellable *cancellable,
                  GError **error)
{
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  for (guint i = 0; i < gs_app_list_length (apps); i++)
    {
      const gchar *app_name[2];
      GsApp *app = gs_app_list_index (apps, i);
      priv->current_app = app;

      app_name[0] = gs_app_get_metadata_item (app, "apk::name");
      /* FIXME: Properly zero terminate the array */
      app_name[1] = '\0';
      gs_app_set_state (app, AS_APP_STATE_INSTALLING);

      if (!apkd_helper_call_upgrade_packages_sync (priv->proxy, app_name, cancellable, &local_error))
        {
          g_dbus_error_strip_remote_error (local_error);
          g_propagate_error (error, local_error);
          gs_app_set_state_recover (app);
          priv->current_app = NULL;
          return FALSE;
        }

      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
      priv->current_app = NULL;
    }

  return TRUE;
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
  if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE &&
      gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM)
    {
      gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
    }

  if (gs_app_get_kind (app) == AS_APP_KIND_OS_UPGRADE)
    {
      gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
    }
}

static gboolean
resolve_appstream_source_file_to_package_name (GsPlugin *plugin,
                                               GsApp *app,
                                               GsPluginRefineFlags flags,
                                               GCancellable *cancellable,
                                               GError **error)
{
  gchar *fn;
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  GVariant *search_result;
  const gchar *tmp = gs_app_get_id (app);

  /* FIXME: Ideally we'd use gs_app_get_metadata("appstream::source-file") but apparently that's not realiable */
  /* Is there a desktop file ? */
  if (g_strrstr (tmp, ".desktop") == NULL)
    {
      fn = g_strdup_printf ("/usr/share/applications/%s.desktop", tmp);
    }
  else
    {
      fn = g_strdup_printf ("/usr/share/applications/%s", tmp);
    }
  /* If there is no desktop file (or it doesn't match $id.desktop), is there an appdata file? */
  if (!g_file_test (fn, G_FILE_TEST_EXISTS))
    {
      fn = g_strdup_printf ("/usr/share/metainfo/%s.metainfo.xml", tmp);
      if (!g_file_test (fn, G_FILE_TEST_EXISTS))
        {
          g_free (fn);
          fn = g_strdup_printf ("/usr/share/metainfo/%s.appdata.xml", tmp);
          if (!g_file_test (fn, G_FILE_TEST_EXISTS))
            {
              fn = g_strdup_printf ("/usr/share/appdata/%s.appdata.xml", tmp);
            }
        }
    }

  if (!g_file_test (fn, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED, "No desktop or appstream file found for app %s", gs_app_get_unique_id (app));
      return FALSE;
    }

  if (!apkd_helper_call_search_file_owner_sync (priv->proxy, fn, &search_result, cancellable, &local_error))
    {
      g_warning ("Couldn't find any matches for appdata file");
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      return FALSE;
    }

  ApkdPackage package = g_variant_to_apkd_package (search_result);

  if (gs_app_get_source_default (app) == NULL)
    {
      gs_app_add_source (app, package.m_name);
      gs_app_set_metadata (app, "apk::name", package.m_name);
      gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
      gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
      gs_app_set_size_installed (app, package.m_installedSize);
    }

  return TRUE;
}

static gboolean
resolve_available_packages_app (GsPlugin *plugin,
                                GPtrArray *arr,
                                GCancellable *cancellable,
                                GError **error)
{
  GVariant *installed_packages = NULL;
  GError *local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  if (!apkd_helper_call_list_installed_packages_sync (priv->proxy, &installed_packages, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
      return FALSE;
    }

  for (guint i = 0; i < g_variant_n_children (installed_packages); i++)
    {
      GVariant *value_tuple = g_variant_get_child_value (installed_packages, i);
      ApkdPackage pkg = g_variant_to_apkd_package (value_tuple);
      GsApp *app = NULL;

      for (guint j = 0; j < arr->len; j++)
        {
          GsApp *potential_match = g_ptr_array_index (arr, j);
          if (pkg.m_name == gs_app_get_source_default (potential_match))
            {
              app = potential_match;
              break;
            }
        }

      if (app == NULL)
        {
          continue;
        }

      if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
        {
          if (pkg.m_isInstalled)
            {
              gs_app_set_state (app, AS_APP_STATE_INSTALLED);
            }
          else
            {
              gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
            }
        }

      if (gs_app_get_origin (app) == NULL)
        {
          gs_app_set_origin (app, "alpine");
        }

      // set more metadata for packages that don't have appstream data
      gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, pkg.m_name);
      gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, pkg.m_description);
      gs_app_set_description (app, GS_APP_QUALITY_UNKNOWN, pkg.m_description);
      gs_app_set_url (app, GS_APP_QUALITY_UNKNOWN, pkg.m_url);
      gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, pkg.m_license);
      gs_app_set_size_download (app, pkg.m_size);
      gs_app_set_size_installed (app, pkg.m_installedSize);
      gs_app_set_version (app, pkg.m_version);
      return TRUE;
    }

  g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED, "No app found to refine");
  return FALSE;
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
                  GsAppList *apps,
                  GsPluginRefineFlags flags,
                  GCancellable *cancellable,
                  GError **error)
{
  GError *local_error = NULL;
  GPtrArray *not_found_app_arr = g_ptr_array_new ();

  g_debug ("Starting refinining process");

  for (guint i = 0; i < gs_app_list_length (apps); i++)
    {
      GsApp *app = gs_app_list_index (apps, i);

      if (gs_app_has_quirk (app, GS_APP_QUIRK_IS_WILDCARD))
        continue;

      /* set management plugin for apps where appstream just added the source package name in refine() */
      if (gs_app_get_management_plugin (app) == NULL &&
          gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE &&
          gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM &&
          gs_app_get_source_default (app) != NULL)
        {
          gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
        }

      /* resolve the source package name based on installed appdata/desktop file name */
      if (gs_app_get_management_plugin (app) == NULL &&
          gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_UNKNOWN &&
          gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM &&
          gs_app_get_source_default (app) == NULL)
        {
          if (resolve_appstream_source_file_to_package_name (plugin, app, flags, cancellable, &local_error))
            {
              continue;
            }
          else
            {
              g_dbus_error_strip_remote_error (local_error);
              g_propagate_error (error, local_error);
              return FALSE;
            }
        }

      if (g_strcmp0 (gs_app_get_management_plugin (app), gs_plugin_get_name (plugin)) != 0 || gs_app_get_source_default (app) == NULL)
        {
          continue;
        }

      g_ptr_array_add (not_found_app_arr, app);
    }

  if (flags &
      (GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
       GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN |
       GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION |
       GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
       GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE |
       GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL |
       GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE |
       GS_PLUGIN_REFINE_FLAGS_DEFAULT))
    {
      resolve_available_packages_app (plugin, not_found_app_arr, cancellable, NULL);
    }

  return TRUE;
}
