/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2020 Rasmus Thomsen <oss@cogitri.dev>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <apk-polkit-1/apk-polkit-client.h>
#include <appstream.h>
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
  Downgradable,
  Reinstallable,
} ApkPackageState;

typedef struct
{
  const gchar *m_name;
  const gchar *m_version;
  const gchar *m_description;
  const gchar *m_license;
  const gchar *m_oldVersion;
  const gchar *m_url;
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
 * apk_to_app_state:
 * @state: A ApkPackageState
 *
 * Convenience function which converts ApkdPackageState to a GsAppState.
 **/
static GsAppState
apk_to_app_state (ApkPackageState state)
{
  switch (state)
    {
    case Installed:
    case PendingRemoval:
      return GS_APP_STATE_INSTALLED;
    case PendingInstall:
    case Available:
      return GS_APP_STATE_AVAILABLE;
    case Downgradable:
    case Reinstallable:
    case Upgradable:
      return GS_APP_STATE_UPDATABLE_LIVE;
    default:
      g_assert_not_reached ();
      return GS_APP_STATE_UNKNOWN;
    }
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

  gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
  gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
  gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
  gs_app_set_allow_cancel (app, FALSE);
  gs_app_add_source (app, pkg->m_name);
  gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, pkg->m_name);
  gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, pkg->m_description);
  gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, pkg->m_url);
  gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, pkg->m_license);
  gs_app_set_origin (app, "alpine");
  gs_app_set_origin_hostname (app, "alpinelinux.org");
  gs_app_set_management_plugin (app, "apk");
  gs_app_set_size_installed (app, pkg->m_installedSize);
  gs_app_set_size_download (app, pkg->m_size);
  gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE);
  gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "apk");
  gs_app_set_state (app, apk_to_app_state (pkg->m_packageState));
  if (gs_app_get_state (app) == GS_APP_STATE_UPDATABLE_LIVE)
    {
      gs_app_set_version (app, pkg->m_oldVersion);
      gs_app_set_update_version (app, pkg->m_version);
    }
  else
    {
      gs_app_set_version (app, pkg->m_version);
    }
  gs_plugin_cache_add (plugin, g_strdup_printf ("%s-%s", pkg->m_name, pkg->m_version), app);

  return app;
}

/**
 * gs_plugin_apk_get_source:
 * @app: The GsApp
 *
 * Convenience function that verifies that the app only has a single source.
 * Returns the corresponding source if successful or NULL if failed.
 */
static gchar *
gs_plugin_apk_get_source (GsApp *app, GError **error)
{
  GPtrArray *sources = gs_app_get_sources (app);
  if (sources->len != 1)
    {
      g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
                   "app %s has number of sources: %d != 1",
                   gs_app_get_unique_id (app), sources->len);
      return NULL;
    }
  return g_strdup (g_ptr_array_index (sources, 0));
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
        case GS_APP_STATE_INSTALLING:
          plugin_status = GS_PLUGIN_STATUS_INSTALLING;
          break;
        case GS_APP_STATE_REMOVING:
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
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "generic-updates");
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
  g_autoptr (GsApp) app_dl = gs_app_new (gs_plugin_get_name (plugin));
  GsPluginData *priv = gs_plugin_get_data (plugin);

  g_debug ("Adding updates");

  gs_app_set_progress (app_dl, GS_APP_PROGRESS_UNKNOWN);
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
      if (pkg.m_packageState == Upgradable || pkg.m_packageState == Downgradable)
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
  g_autofree gchar *source = NULL;

  g_return_val_if_fail (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY, TRUE);

  /* We can only install apps we know of */
  if (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0)
    return TRUE;

  source = gs_plugin_apk_get_source (app, error);
  if (source == NULL)
    return FALSE;

  g_debug ("Trying to install app %s", gs_app_get_unique_id (app));
  gs_app_set_progress (app, GS_APP_PROGRESS_UNKNOWN);
  gs_app_set_state (app, GS_APP_STATE_INSTALLING);

  priv->current_app = app;

  if (!apk_polkit1_call_add_package_sync (priv->proxy, source, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      gs_app_set_state_recover (app);
      priv->current_app = NULL;
      return FALSE;
    }

  gs_app_set_state (app, GS_APP_STATE_INSTALLED);
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
  g_autofree gchar *source = NULL;

  g_return_val_if_fail (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY, TRUE);

  /* We can only remove apps we know of */
  if (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0)
    return TRUE;

  source = gs_plugin_apk_get_source (app, error);
  if (source == NULL)
    return FALSE;

  g_debug ("Trying to remove app %s", gs_app_get_unique_id (app));
  gs_app_set_progress (app, GS_APP_PROGRESS_UNKNOWN);
  gs_app_set_state (app, GS_APP_STATE_REMOVING);

  priv->current_app = app;

  if (!apk_polkit1_call_delete_package_sync (priv->proxy, source, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      gs_app_set_state_recover (app);
      priv->current_app = NULL;
      return FALSE;
    }

  gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
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
  g_autoptr (GsApp) app_dl = gs_app_new (gs_plugin_get_name (plugin));
  GsPluginData *priv = gs_plugin_get_data (plugin);
  GsApp *app;

  gs_app_set_progress (app_dl, GS_APP_PROGRESS_UNKNOWN);
  for (guint i = 0; i < gs_app_list_length (apps); i++)
    {
      gboolean is_proxy;
      app = gs_app_list_index (apps, i);

      is_proxy = gs_app_has_quirk (app, GS_APP_QUIRK_IS_PROXY);

      /* We shall only touch the apps if they are are owned by us or
       * a proxy (and thus might contain some apps owned by us) */
      if (!is_proxy && (g_strcmp0 (gs_app_get_management_plugin (app), "apk") != 0))
        continue;

      priv->current_app = app;
      g_debug ("Updating app %s", gs_app_get_unique_id (app));

      gs_app_set_state (app, GS_APP_STATE_INSTALLING);

      if (is_proxy)
        {
          if (!gs_plugin_update (plugin, gs_app_get_related (app), cancellable, &local_error))
            goto error;
        }
      else
        {
          g_autofree gchar *source = gs_plugin_apk_get_source (app, &local_error);
          if (source == NULL)
            goto error;

          if (!apk_polkit1_call_upgrade_package_sync (priv->proxy, source, cancellable, &local_error))
            {
              g_dbus_error_strip_remote_error (local_error);
              goto error;
            }
        }

      gs_app_set_state (app, GS_APP_STATE_INSTALLED);
      priv->current_app = NULL;
    }

  gs_plugin_updates_changed (plugin);
  return TRUE;

error:
  g_propagate_error (error, g_steal_pointer (&local_error));
  gs_app_set_state_recover (app);
  priv->current_app = NULL;
  return FALSE;
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
  if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE &&
      gs_app_get_scope (app) == AS_COMPONENT_SCOPE_SYSTEM)
    {
      g_debug ("Adopted app %s", gs_app_get_unique_id (app));
      gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
    }

  if (gs_app_get_kind (app) == AS_COMPONENT_KIND_OPERATING_SYSTEM)
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
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN &&
      gs_app_get_origin (app) == NULL)
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
      gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, package->m_url);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE)
    {
      gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, package->m_license);
    }
  if (flags & GS_PLUGIN_REFINE_FLAGS_DEFAULT)
    {
      gs_app_set_version (app, package->m_version);
      if (gs_app_get_origin (app) == NULL)
        gs_app_set_origin (app, "alpine");
      gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, package->m_description);
      gs_app_set_size_download (app, package->m_size);
      gs_app_set_size_installed (app, package->m_installedSize);
      gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, package->m_url);
      gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, package->m_license);
    }
  g_debug ("State for pkg %s: %u", gs_app_get_unique_id (app), package->m_packageState);
  /* FIXME: Currently apk-rs-polkit only returns states Available and Installed
   * regardless of whether the packages are in a different state like upgraded.
   * If we blindly set the state of the app to the one from package, we will
   * in some circumstances overwrite the real state (that might have been).
   * Specially important for functions like gs_plugin_add_updates that only set
   * a temporary state. Therefore, here we only allow transitions which final
   * state is legally GS_APP_STATE_AVAILABLE or GS_APP_STATE_INSTALLED.
   */
  switch (gs_app_get_state (app))
    {
    case GS_APP_STATE_UNKNOWN:
    case GS_APP_STATE_QUEUED_FOR_INSTALL:
    case GS_APP_STATE_REMOVING:
    case GS_APP_STATE_INSTALLING:
    case GS_APP_STATE_UNAVAILABLE:
      gs_app_set_state (app, apk_to_app_state (package->m_packageState));
    case GS_APP_STATE_AVAILABLE:
    case GS_APP_STATE_INSTALLED:
      break; /* Ignore changes between the states */
    default:
      g_warning ("Wrong state transition detected and avoided!");
      break;
    }

  if (g_strcmp0 (gs_app_get_source_default (app), package->m_name) != 0)
    gs_app_add_source (app, package->m_name);
  gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
  gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
}

/**
 * fix_app_missing_appstream:
 * @plugin: The apk GsPlugin.
 * @app: The GsApp to resolve the metainfo/desktop file for.
 * @cancellable: GCancellable to cancel synchronous dbus call.
 *
 * If the appstream plugin could not find the app in the distribution metadata,
 * it might have created the application from the metainfo or desktop files
 * installed. It will contain some basic information, but the apk package to
 * which it belongs (the source) needs to completed by us.
 **/
static gboolean
fix_app_missing_appstream (GsPlugin *plugin,
                           GsApp *app,
                           GCancellable *cancellable)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) search_result = NULL;
  const gchar *fn = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  ApkdPackage package;

  /* The appstream plugin sets some metadata on apps that come from desktop
   * and metainfo files. If metadata is missing, just give-up */
  fn = gs_app_get_metadata_item (app, "appstream::source-file");
  if (fn == NULL)
    {
      g_warning ("Couldn't find 'appstream::source-file' metadata for %s",
                 gs_app_get_unique_id (app));
      return FALSE;
    }

  if (!apk_polkit1_call_search_file_owner_sync (priv->proxy, fn, &search_result, cancellable, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_dbus_error_strip_remote_error (error);
          g_warning ("Couldn't find any package owning file '%s': %s",
                     fn, error->message);
        }
      return FALSE;
    }

  package = g_variant_to_apkd_package (search_result);
  g_debug ("Found pkgname '%s' for app %s", package.m_name,
           gs_app_get_unique_id (app));
  gs_app_add_source (app, package.m_name);

  return TRUE;
}

/**
 * refine_apk_package:
 * @plugin: The apk GsPlugin.
 * @app: The app which we try to refine.
 * @flags: TheGsPluginRefineFlags which determine what data we add.
 * @cancellable: GCancellable to cancel resolving what app owns the appstream/desktop file.
 * @error: GError which is set if something goes wrong.
 *
 * Get details from apk package for a specific app and fill-in requested refine data.
 **/
static gboolean
refine_apk_package (GsPlugin *plugin,
                    GsApp *app,
                    GsPluginRefineFlags flags,
                    GCancellable *cancellable,
                    GError **error)
{
  g_autoptr (GVariant) apk_package = NULL;
  g_autoptr (GError) local_error = NULL;
  GsPluginData *priv = gs_plugin_get_data (plugin);
  const gchar *source = gs_app_get_source_default (app);

  g_debug ("Refining %s", gs_app_get_unique_id (app));
  if (!apk_polkit1_call_get_package_details_sync (priv->proxy, source, &apk_package, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  ApkdPackage pkg = g_variant_to_apkd_package (apk_package);

  set_app_metadata (plugin, app, &pkg, flags);
  /* We should only set generic apps for OS updates */
  if (gs_app_get_kind (app) == AS_COMPONENT_KIND_GENERIC)
    gs_app_set_special_kind (app, GS_APP_SPECIAL_KIND_OS_UPDATE);

  return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
                  GsAppList *apps,
                  GsPluginRefineFlags flags,
                  GCancellable *cancellable,
                  GError **error)
{
  g_debug ("Starting refinining process");

  for (guint i = 0; i < gs_app_list_length (apps); i++)
    {
      GsApp *app = gs_app_list_index (apps, i);
      GPtrArray *sources;
      AsBundleKind bundle_kind = gs_app_get_bundle_kind (app);

      if (gs_app_has_quirk (app, GS_APP_QUIRK_IS_WILDCARD) ||
          gs_app_get_kind (app) == AS_COMPONENT_KIND_REPOSITORY)
        {
          g_debug ("App %s has quirk WILDCARD or is of REPOSITORY kind; skipping!", gs_app_get_unique_id (app));
          continue;
        }

      /* Only package and unknown (desktop or metainfo file with upstream AS)
       * belog to us */
      if (bundle_kind != AS_BUNDLE_KIND_UNKNOWN &&
          bundle_kind != AS_BUNDLE_KIND_PACKAGE)
        {
          g_debug ("App %s has bundle kind %s; skipping!", gs_app_get_unique_id (app),
                   as_bundle_kind_to_string (bundle_kind));
          continue;
        }

      /* set management plugin for system apps just created by appstream */
      if (gs_app_get_management_plugin (app) == NULL &&
          gs_app_get_scope (app) == AS_COMPONENT_SCOPE_SYSTEM &&
          g_strcmp0 (gs_app_get_metadata_item (app, "GnomeSoftware::Creator"), "appstream") == 0)
        {
          /* If appstream couldn't assign a source, it means the app does not
           * have an entry in the distribution-generated metadata. That should
           * be fixed in the app, but we try to workaround it by finding the
           * owner of the metainfo or desktop file */
          if (gs_app_get_source_default (app) == NULL)
            {
              g_warning ("App %s missing pkgname. Trying to resolve via metainfo/desktop file",
                         gs_app_get_unique_id (app));
              if (!fix_app_missing_appstream (plugin, app, cancellable))
                {
                  if (g_cancellable_is_cancelled (cancellable))
                    return FALSE;
                  else
                    continue;
                }
            }

          g_debug ("Setting ourselves as management plugin for app %s", gs_app_get_unique_id (app));
          gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
        }

      if (g_strcmp0 (gs_app_get_management_plugin (app), gs_plugin_get_name (plugin)) != 0)
        {
          g_debug ("Ignoring app %s, not owned by apk", gs_app_get_unique_id (app));
          continue;
        }

      sources = gs_app_get_sources (app);
      if (sources->len == 0)
        {
          g_warning ("app %s has missing sources; skipping", gs_app_get_unique_id (app));
          continue;
        }
      if (sources->len >= 2)
        {
          g_warning ("app %s has %d > 1 sources; skipping", gs_app_get_unique_id (app), sources->len);
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
          if (!refine_apk_package (plugin, app, flags, cancellable, error))
            return FALSE;
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
      g_autofree gchar *url = NULL;
      g_autofree gchar *url_path = NULL;
      g_autofree gchar *url_scheme = NULL;
      g_autoptr (GsApp) app = NULL;
      g_autoptr (GVariant) value_tuple = NULL;
      gboolean enabled = FALSE;

      value_tuple = g_variant_get_child_value (repositories, i);
      enabled = g_variant_get_boolean (g_variant_get_child_value (value_tuple, 0));
      description = g_strdup (g_variant_get_string (g_variant_get_child_value (value_tuple, 1), NULL));
      url = g_strdup (g_variant_get_string (g_variant_get_child_value (value_tuple, 2), NULL));

      app = gs_plugin_cache_lookup (plugin, url);
      if (app)
        {
          gs_app_set_state (app, enabled ? GS_APP_STATE_INSTALLED : GS_APP_STATE_AVAILABLE);
          gs_app_list_add (list, g_steal_pointer (&app));
          continue;
        }

      g_debug ("Adding repository  %s", url);

      g_uri_split (url, G_URI_FLAGS_NONE, &url_scheme, NULL,
                   NULL, NULL, &url_path, NULL, NULL, error);
      if (*error)
        return FALSE;

      /* Transform /some/repo/url into some.repo.url
         We are not allowed to use '/' in the app id. */
      id = g_strdelimit (g_strdup (url_path + 1), "/", '.');

      if (url_scheme)
        {
          /* If there is a scheme, it is a remote repository. Try to build
           * a description depending on the information available,
           * e.g: ["alpine", "edge", "community"] or ["postmarketos", "master"] */
          gchar **repo_parts = g_strsplit (id, ".", 3);

          g_autofree gchar *repo = g_strdup (repo_parts[0]);
          if (g_strv_length (repo_parts) == 3)
            {
              g_free (repo);
              repo = g_strdup_printf ("%s %s", repo_parts[0], repo_parts[2]);
            }

          g_autofree gchar *release = g_strdup ("");
          if (g_strv_length (repo_parts) >= 2)
            {
              g_free (release);
              release = g_strdup_printf (" (release %s)", repo_parts[1]);
            }
          repo_displayname = g_strdup_printf (_ ("Remote repository %s%s"), repo, release);
          g_strfreev (repo_parts);
        }
      else
        {
          repo_displayname = g_strdup_printf (_ ("Local repository %s"), url_path);
        }

      app = gs_app_new (id);
      gs_app_set_kind (app, AS_COMPONENT_KIND_REPOSITORY);
      gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
      gs_app_set_state (app, enabled ? GS_APP_STATE_INSTALLED : GS_APP_STATE_AVAILABLE);
      gs_app_add_quirk (app, GS_APP_QUIRK_NOT_LAUNCHABLE);
      gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, repo_displayname);
      gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, description);
      gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, url);
      gs_app_set_metadata (app, "apk::repo-url", url);
      gs_app_set_management_plugin (app, "apk");
      gs_plugin_cache_add (plugin, url, app);
      gs_app_list_add (list, g_steal_pointer (&app));
    }

  g_debug ("Added repositories");

  return TRUE;
}

static gboolean
gs_plugin_repo_update (GsPlugin *plugin,
                       GsApp *repo,
                       GCancellable *cancellable,
                       GError **error,
                       gboolean is_install)
{
  GsPluginData *priv = gs_plugin_get_data (plugin);
  g_autoptr (GError) local_error = NULL;
  const gchar *url = NULL;
  const gchar *action = is_install ? "Install" : "Remov";
  int rc;

  gs_app_set_progress (repo, GS_APP_PROGRESS_UNKNOWN);

  url = gs_app_get_metadata_item (repo, "apk::repo-url");
  g_debug ("%sing repository %s", action, url);
  if (is_install)
    {
      rc = apk_polkit1_call_add_repository_sync (priv->proxy,
                                                 url,
                                                 cancellable,
                                                 &local_error);
    }
  else
    {
      rc = apk_polkit1_call_remove_repository_sync (priv->proxy,
                                                    url,
                                                    cancellable,
                                                    &local_error);
    }
  if (!rc)
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      gs_app_set_state_recover (repo);
      return FALSE;
    }

  g_debug ("%sed repository %s", action, url);
  return TRUE;
}

gboolean
gs_plugin_install_repo (GsPlugin *plugin,
                        GsApp *repo,
                        GCancellable *cancellable,
                        GError **error)
{
  g_return_val_if_fail (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY, FALSE);
  gs_app_set_state (repo, GS_APP_STATE_INSTALLING);

  if (!gs_plugin_repo_update (plugin, repo, cancellable, error, TRUE))
    return FALSE;

  gs_app_set_state (repo, GS_APP_STATE_INSTALLED);
  return TRUE;
}

gboolean
gs_plugin_remove_repo (GsPlugin *plugin,
                       GsApp *repo,
                       GCancellable *cancellable,
                       GError **error)
{
  g_return_val_if_fail (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY, FALSE);
  gs_app_set_state (repo, GS_APP_STATE_REMOVING);

  if (!gs_plugin_repo_update (plugin, repo, cancellable, error, FALSE))
    return FALSE;

  gs_app_set_state (repo, GS_APP_STATE_AVAILABLE);
  return TRUE;
}
