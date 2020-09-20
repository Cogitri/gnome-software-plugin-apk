/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2020 Rasmus Thomsen <oss@cogitri.dev>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <apk-polkit-1/apk-polkit-client.h>
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
  ApkPolkit1 *proxy;
};

typedef enum _ApkPackageState
{
  Available,
  Installed,
  PendingInstall,
  PendingRemoval,
  Upgradable,
} ApkPackageState;

typedef struct
{
  const gchar *m_name;
  const gchar *m_version;
  const gchar *m_oldVersion;
  const gchar *m_license;
  const gchar *m_url;
  const gchar *m_description;
  gulong m_installedSize;
  gulong m_size;
  ApkPackageState m_packageState;
} ApkdPackage;

/**
 * g_variant_to_apkd_package:
 * @value_tuple: a GVariant, as received from apk_polkit1_call*
 *
 * Convenience finction which conerts a GVariant we get pack from our DBus
 * proxy to a ApkdPackage
 **/
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
    g_variant_get_uint64 (g_variant_get_child_value (value_tuple, 6)),
    g_variant_get_uint64 (g_variant_get_child_value (value_tuple, 7)),
    g_variant_get_uint32 (g_variant_get_child_value (value_tuple, 8)),
  };
  return pkg;
}

/**
 * apk_package_to_app:
 * @pkg: A ApkPackage
 *
 * Convenience function which converts a ApkdPackage to a GsApp.
 **/
static GsApp *
apk_package_to_app (GsPlugin *plugin, ApkdPackage *pkg)
{
  GsApp *app = gs_plugin_cache_lookup (plugin, g_strdup_printf ("%s-%s", pkg->m_name, pkg->m_version));
  if (app != NULL)
    {
      return app;
    }

  app = gs_app_new (pkg->m_name);

  gs_app_set_kind (app, AS_APP_KIND_GENERIC);
  gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
  gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
  gs_app_set_allow_cancel (app, FALSE);
  gs_app_add_source (app, pkg->m_name);
  gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, pkg->m_name);
  gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, pkg->m_description);
  gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, pkg->m_url);
  gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, pkg->m_license);
  gs_app_set_origin (app, "alpine");
  gs_app_set_origin_hostname (app, "alpinelinux.org");
  gs_app_set_management_plugin (app, "apk");
  gs_app_set_metadata (app, "apk::name", pkg->m_name);
  gs_app_set_size_installed (app, pkg->m_installedSize);
  gs_app_set_size_download (app, pkg->m_size);
  gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE);
  gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
  gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "apk");
  switch (pkg->m_packageState)
    {
    case Installed:
    case PendingRemoval:
      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
      gs_app_set_version (app, pkg->m_version);
      break;
    case PendingInstall:
    case Available:
      gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
      gs_app_set_version (app, pkg->m_version);
      break;
    case Upgradable:
      gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
      gs_app_set_kind (app, AS_APP_KIND_OS_UPDATE);
      gs_app_set_version (app, pkg->m_oldVersion);
      gs_app_set_update_version (app, pkg->m_version);
      break;
    }
  gs_plugin_cache_add (plugin, g_strdup_printf ("%s-%s", pkg->m_name, pkg->m_version), app);

  return app;
}

/**
 * apk_progress_signal_connect_callback:
 * @proxy: A GDBusProxy. Currently unused.
 * @sender_name: The name of the sender.
 * @signal_name: The name of the signal which we received.
 * @parameters: The return value of the signal.
 * @user_data: User data which we previously passed to g_signal_connect. It's our GsPlugin.
 *
 * Callback passed to g_signal_connect to update the current progress.
 */
static void
apk_progress_signal_connect_callback (GDBusProxy *proxy,
                                      gchar *sender_name,
                                      gchar *signal_name,
                                      GVariant *parameters,
                                      gpointer user_data)
{
  GsPluginData *priv = gs_plugin_get_data ((GsPlugin *) user_data);
  GsPluginStatus plugin_status = GS_PLUGIN_STATUS_DOWNLOADING;

  /* We only act upon the progressNotification signal to set progress */
  if (g_strcmp0 (signal_name, "progressNotification") != 0)
    {
      return;
    }

  /* nothing in progress */
  if (priv->current_app != NULL)
    {
      uint percentage =
          g_variant_get_uint32 (g_variant_get_child_value (parameters, 0));

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
  /* We want to get packages from appstream and refine them */
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
  gs_plugin_alloc_data (plugin, sizeof (GsPluginData));
  priv = gs_plugin_get_data (plugin);
  priv->current_app = NULL;
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  g_debug ("Initializing plugin");

  priv->proxy = apk_polkit1_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    "dev.Cogitri.apkPolkit1",
                                                    "/dev/Cogitri/apkPolkit1",
                                                    cancellable,
                                                    &local_error);

  if (local_error != NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  // FIXME: Instead of disabling the timeout here, apkd should have an async API.
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (priv->proxy), G_MAXINT);

  g_signal_connect (priv->proxy, "g-signal", G_CALLBACK (apk_progress_signal_connect_callback), plugin);

  return TRUE;
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
                   guint cache_age,
                   GCancellable *cancellable,
                   GError **error)
{
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  g_autoptr (GsApp) app_dl = gs_app_new (gs_plugin_get_name (plugin));
  priv->current_app = app_dl;

  g_debug ("Refreshing repositories");

  gs_app_set_summary_missing (app_dl, _ ("Getting apk repository indexes…"));
  gs_plugin_status_update (plugin, app_dl, GS_PLUGIN_STATUS_DOWNLOADING);
  if (apk_polkit1_call_update_repositories_sync (priv->proxy, cancellable, &local_error))
    {
      gs_app_set_progress (app_dl, 100);
      priv->current_app = NULL;
      gs_plugin_updates_changed (plugin);
      return TRUE;
    }
  else
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
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
  g_autoptr (GVariant) upgradable_packages = NULL;
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  g_debug ("Adding updates");

  if (!apk_polkit1_call_list_upgradable_packages_sync (priv->proxy, &upgradable_packages, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_debug ("Found %" G_GSIZE_FORMAT " upgradable packages", g_variant_n_children (upgradable_packages));

  for (gsize i = 0; i < g_variant_n_children (upgradable_packages); i++)
    {
      g_autoptr (GsApp) app = NULL;
      g_autoptr (GVariant) value_tuple = NULL;
      ApkdPackage pkg;

      value_tuple = g_variant_get_child_value (upgradable_packages, i);
      pkg = g_variant_to_apkd_package (value_tuple);
      if (pkg.m_packageState == Upgradable)
        {
          app = apk_package_to_app (plugin, &pkg);
          gs_app_list_add (list, g_steal_pointer (&app));
        }
    }

  return TRUE;
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
                       GsApp *app,
                       GCancellable *cancellable,
                       GError **error)
{
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  g_debug ("Trying to install app %s", gs_app_get_unique_id (app));

  /* We can only install apps we know of */
  if (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0)
    return TRUE;

  gs_app_set_state (app, AS_APP_STATE_INSTALLING);

  if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
    {
      if (!apk_polkit1_call_add_repository_sync (priv->proxy, gs_app_get_metadata_item (app, "apk::repo-url"), cancellable, &local_error))
        {
          g_dbus_error_strip_remote_error (local_error);
          g_propagate_error (error, g_steal_pointer (&local_error));
          gs_app_set_state_recover (app);
          return FALSE;
        }

      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
      return TRUE;
    }

  priv->current_app = app;

  if (!apk_polkit1_call_add_package_sync (priv->proxy, gs_app_get_metadata_item (app, "apk::name"), cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
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
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  g_debug ("Trying to remove app %s", gs_app_get_unique_id (app));

  /* We can only remove apps we know of */
  if (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0)
    return TRUE;

  gs_app_set_state (app, AS_APP_STATE_REMOVING);

  if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
    {
      if (!apk_polkit1_call_remove_repository_sync (priv->proxy, gs_app_get_metadata_item (app, "apk::repo-url"), cancellable, &local_error))
        {
          g_dbus_error_strip_remote_error (local_error);
          g_propagate_error (error, g_steal_pointer (&local_error));
          gs_app_set_state_recover (app);
          return FALSE;
        }

      gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
      return TRUE;
    }

  priv->current_app = app;

  if (!apk_polkit1_call_delete_package_sync (priv->proxy, gs_app_get_metadata_item (app, "apk::name"), cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      gs_app_set_state_recover (app);
      priv->current_app = NULL;
      return FALSE;
    }

  gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
  priv->current_app = NULL;
  return TRUE;
}

gboolean
gs_plugin_update (GsPlugin *plugin,
                  GsAppList *apps,
                  GCancellable *cancellable,
                  GError **error)
{
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  for (guint i = 0; i < gs_app_list_length (apps); i++)
    {
      GsApp *app = gs_app_list_index (apps, i);
      priv->current_app = app;

      g_debug ("Updating app %s", gs_app_get_unique_id (app));

      gs_app_set_state (app, AS_APP_STATE_INSTALLING);

      if (!apk_polkit1_call_upgrade_package_sync (priv->proxy, gs_app_get_metadata_item (app, "apk::name"), cancellable, &local_error))
        {
          g_dbus_error_strip_remote_error (local_error);
          g_propagate_error (error, g_steal_pointer (&local_error));
          gs_app_set_state_recover (app);
          priv->current_app = NULL;
          return FALSE;
        }

      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
      priv->current_app = NULL;
    }

  gs_plugin_updates_changed (plugin);
  return TRUE;
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
  if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE &&
      gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM)
    {
      g_debug ("Adopted app %s", gs_app_get_unique_id (app));
      gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
    }

  if (gs_app_get_kind (app) == AS_APP_KIND_OS_UPGRADE)
    {
      g_debug ("Adopted app %s", gs_app_get_unique_id (app));
      gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
    }
}

/**
 * set_app_metadata:
 * @plugin: The apk GsPlugin.
 * @app: The GsApp for which we want to set the metadata on.
 * @package: The ApkdPackage to get the metadata from.
 * @flags: The GsPluginRefineFlags which determine what metadata to set
 *
 * Helper function to set the right metadata items on an app.
 **/
static void
set_app_metadata (GsPlugin *plugin, GsApp *app, ApkdPackage *package, GsPluginRefineFlags flags)
{
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION)
    {
      gs_app_set_version (app, package->m_version);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN)
    {
      gs_app_set_origin (app, "alpine");
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION)
    {
      gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, package->m_description);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE)
    {
      gs_app_set_size_download (app, package->m_size);
      gs_app_set_size_installed (app, package->m_installedSize);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL)
    {
      gs_app_set_url (app, GS_APP_QUALITY_UNKNOWN, package->m_url);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE)
    {
      gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, package->m_license);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_DEFAULT)
    {
      gs_app_set_version (app, package->m_version);
      gs_app_set_origin (app, "alpine");
      gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, package->m_description);
      gs_app_set_size_download (app, package->m_size);
      gs_app_set_size_installed (app, package->m_installedSize);
      gs_app_set_url (app, GS_APP_QUALITY_UNKNOWN, package->m_url);
      gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, package->m_license);
    }
  g_debug ("State for pkg %s: %d", gs_app_get_unique_id (app), package->m_packageState);
  switch (package->m_packageState)
    {
    case Installed:
    case PendingRemoval:
      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
      break;
    case PendingInstall:
    case Available:
      gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
      break;
    case Upgradable:
      gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
      gs_app_set_kind (app, AS_APP_KIND_OS_UPDATE);
      break;
    }

  gs_app_add_source (app, package->m_name);
  gs_app_set_metadata (app, "apk::name", package->m_name);
  gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
  gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
}

/**
 * resolve_appstream_source_file_to_package_name:
 * @plugin: The apk GsPlugin.
 * @app: The GsApp to resolve the appstream/desktop file for.
 * @flags: TheGsPluginRefineFlags which determine what data we add.
 * @cancellable: GCancellable to cancel resolving what app owns the appstream/desktop file.
 * @error: GError which is set if something goes wrong.
 *
 * Check what apk package owns the desktop/appstream file from which the app was generated from
 * by the desktop/appstream plugin. Add additional info to the package we have in our repos if we
 * find a matching package installed on the system and adopt the package.
 **/
static gboolean
resolve_appstream_source_file_to_package_name (GsPlugin *plugin,
                                               GsApp *app,
                                               GsPluginRefineFlags flags,
                                               GCancellable *cancellable,
                                               GError **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) search_result = NULL;
  gchar *fn;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  const gchar *tmp = gs_app_get_id (app);
  ApkdPackage package;

  g_debug ("Trying to find desktop/appstream file for app %s", gs_app_get_unique_id (app));

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
          fn = g_strdup_printf ("/usr/share/metainfo/%s.appdata.xml", tmp);
          if (!g_file_test (fn, G_FILE_TEST_EXISTS))
            {
              fn = g_strdup_printf ("/usr/share/appdata/%s.appdata.xml", tmp);
            }
        }
    }

  if (!g_file_test (fn, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED, _ ("No desktop or appstream file found for app %s"), gs_app_get_unique_id (app));
      return FALSE;
    }

  g_debug ("Found desktop/appstream file %s for app %s", fn, gs_app_get_unique_id (app));

  if (!apk_polkit1_call_search_file_owner_sync (priv->proxy, fn, &search_result, cancellable, &local_error))
    {
      g_warning ("Couldn't find any matches for appdata file");
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  package = g_variant_to_apkd_package (search_result);

  set_app_metadata (plugin, app, &package, flags);

  return TRUE;
}

/**
 * resolve_matching_package:
 * @plugin: The apk GsPlugin.
 * @app: The app which we try to refine.
 * @flags: TheGsPluginRefineFlags which determine what data we add.
 * @cancellable: GCancellable to cancel resolving what app owns the appstream/desktop file.
 * @error: GError which is set if something goes wrong.
 *
 * Try to find a package among all available packages which matches the specified app and
 * refine it with additional info apk provides.
 **/
static gboolean
resolve_matching_package (GsPlugin *plugin,
                          GsApp *app,
                          GsPluginRefineFlags flags,
                          GCancellable *cancellable,
                          GError **error)
{
  g_autoptr (GVariant) matching_package = NULL;
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  if (!apk_polkit1_call_get_package_details_sync (priv->proxy, gs_app_get_source_default (app), &matching_package, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  ApkdPackage pkg = g_variant_to_apkd_package (matching_package);

  g_debug ("Found matching apk package %s for app %s", pkg.m_name, gs_app_get_unique_id (app));

  set_app_metadata (plugin, app, &pkg, flags);
  return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
                  GsAppList *apps,
                  GsPluginRefineFlags flags,
                  GCancellable *cancellable,
                  GError **error)
{
  g_autoptr (GError) local_error = NULL;

  g_debug ("Starting refinining process");

  for (guint i = 0; i < gs_app_list_length (apps); i++)
    {
      GsApp *app = gs_app_list_index (apps, i);

      if (gs_app_has_quirk (app, GS_APP_QUIRK_IS_WILDCARD) || gs_app_get_kind (app) & AS_APP_KIND_SOURCE)
        {
          g_debug ("App %s has quirk WILDCARD or is of SOURCE kind; skipping!", gs_app_get_unique_id (app));
          continue;
        }

      /* set management plugin for apps where appstream just added the source package name in refine() */
      if (gs_app_get_management_plugin (app) == NULL &&
          gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE &&
          gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM &&
          gs_app_get_source_default (app) != NULL)
        {
          g_debug ("Setting ourselves as management plugin for app %s", gs_app_get_unique_id (app));
          gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
        }

      /* resolve the source package name based on installed appdata/desktop file name */
      if (gs_app_get_management_plugin (app) == NULL &&
          gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_UNKNOWN &&
          gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM &&
          gs_app_get_source_default (app) == NULL)
        {
          g_debug ("Trying to resolve package name via appstream/desktop file for app %s", gs_app_get_unique_id (app));
          if (resolve_appstream_source_file_to_package_name (plugin, app, flags, cancellable, &local_error))
            {
              continue;
            }
          else
            {
              g_dbus_error_strip_remote_error (local_error);
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      if (g_strcmp0 (gs_app_get_management_plugin (app), gs_plugin_get_name (plugin)) != 0 || gs_app_get_source_default (app) == NULL)
        {
          continue;
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
          if (!resolve_matching_package (plugin, app, flags, cancellable, &local_error))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }
    }

  return TRUE;
}

gboolean
gs_plugin_add_sources (GsPlugin *plugin,
                       GsAppList *list,
                       GCancellable *cancellable,
                       GError **error)
{
  g_autoptr (GVariant) repositories = NULL;
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);

  g_debug ("Adding repositories");

  if (!apk_polkit1_call_list_repositories_sync (priv->proxy, &repositories, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  for (gsize i = 0; i < g_variant_n_children (repositories); i++)
    {
      g_autofree gchar *description = NULL;
      g_autofree gchar *id = NULL;
      g_autofree gchar *repo_displayname = NULL;
      g_autofree gchar **repo_name = NULL;
      g_autofree gchar *url = NULL;
      g_autoptr (GsApp) app = NULL;
      g_autoptr (GVariant) value_tuple = NULL;
      gboolean enabled = FALSE;
      gsize len;

      value_tuple = g_variant_get_child_value (repositories, i);
      enabled = g_variant_get_boolean (g_variant_get_child_value (value_tuple, 0));
      description = g_strdup (g_variant_get_string (g_variant_get_child_value (value_tuple, 1), &len));
      url = g_strdup (g_variant_get_string (g_variant_get_child_value (value_tuple, 2), &len));
      repo_name = g_strsplit (url, "/", -1);
      len = g_strv_length (repo_name);

      /* create something that we can use to enable/disable */
      switch (len)
        {
        case 0:
          id = g_strdup_printf ("org.alpinelinux.%s.repo.%s", url, enabled ? "enabled" : "disabled");
        case 1:
          g_strdup_printf ("org.alpinelinux.%s.repo.%s", repo_name[0], enabled ? "enabled" : "disabled");
        default:
          id = g_strdup_printf ("org.alpinelinux.%s-%s.repo.%s", repo_name[len - 2], repo_name[len - 1], enabled ? "enabled" : "disabled");
        }

      if (strstr (url, "http") == NULL)
        {
          if (len > 1)
            {
              repo_displayname = g_strdup_printf (_ ("Local repository %s/%s"), repo_name[len - 2], repo_name[len - 1]);
            }
          else
            {
              repo_displayname = _ ("Local repository");
            }
        }
      else
        {
          if (len > 1)
            {
              repo_displayname = g_strdup_printf (_ ("Remote repository %s (branch: %s)"), repo_name[len - 1], repo_name[len - 2]);
            }
          else
            {
              repo_displayname = _ ("Remote repository");
            }
        }

      app = gs_app_new (id);
      gs_app_set_kind (app, AS_APP_KIND_SOURCE);
      gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
      gs_app_set_state (app, enabled ? AS_APP_STATE_INSTALLED : AS_APP_STATE_AVAILABLE);
      gs_app_add_quirk (app, GS_APP_QUIRK_NOT_LAUNCHABLE);
      gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, repo_displayname);
      gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, description);
      gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, url);
      gs_app_set_metadata (app, "apk::repo-url", url);
      gs_app_set_management_plugin (app, "apk");
      gs_app_list_add (list, g_steal_pointer (&app));
    }

  return TRUE;
}
