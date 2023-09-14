/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2020 Rasmus Thomsen <oss@cogitri.dev>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gs-plugin-apk.h"
#include <apk-polkit-2/apk-polkit-client-bitflags.h>
#include <apk-polkit-2/apk-polkit-client.h>
#include <appstream.h>
#include <gnome-software.h>
#include <libintl.h>
#include <locale.h>
#define _(string) gettext (string)

#define APK_POLKIT_CLIENT_DETAILS_FLAGS_ALL 0xFF

struct _GsPluginApk
{
  GsPlugin parent;

  ApkPolkit2 *proxy;
};

G_DEFINE_TYPE (GsPluginApk, gs_plugin_apk, GS_TYPE_PLUGIN);

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
  const gchar *name;
  const gchar *version;
  const gchar *description;
  const gchar *license;
  const gchar *stagingVersion;
  const gchar *url;
  gulong installedSize;
  gulong size;
  ApkPackageState packageState;
} ApkdPackage;

/**
 * gs_plugin_apk_variant_to_apkd:
 * @dict: a `a{sv}` GVariant representing a package
 * @pkg: an ApkdPackage pointer where to place the data
 *
 * Receives a GVariant dictionary representing a package and fills an
 * ApkdPackage with the fields available in the dictionary. Returns
 * a boolean depending on whether the dictionary contains or not an error
 * field.
 **/
static inline gboolean
gs_plugin_apk_variant_to_apkd (GVariant *dict, ApkdPackage *pkg)
{
  const gchar *error_str;
  if (!g_variant_lookup (dict, "name", "&s", &pkg->name))
    return FALSE;
  if (g_variant_lookup (dict, "error", "&s", &error_str))
    {
      g_warning ("Package %s could not be unpacked: %s", pkg->name, error_str);
      return FALSE;
    }
  g_variant_lookup (dict, "version", "&s", &pkg->version);
  g_variant_lookup (dict, "description", "&s", &pkg->description);
  g_variant_lookup (dict, "license", "&s", &pkg->license);
  g_variant_lookup (dict, "url", "&s", &pkg->url);
  g_variant_lookup (dict, "staging_version", "&s", &pkg->stagingVersion);
  g_variant_lookup (dict, "installed_size", "t", &pkg->installedSize);
  g_variant_lookup (dict, "size", "t", &pkg->size);
  g_variant_lookup (dict, "package_state", "u", &pkg->packageState);

  return TRUE;
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
  g_autofree gchar *cache_name = NULL;
  cache_name = g_strdup_printf ("%s-%s", pkg->name, pkg->version);
  GsApp *app = gs_plugin_cache_lookup (plugin, cache_name);
  if (app != NULL)
    return app;

  app = gs_app_new (pkg->name);

  gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
  gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
  gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
  gs_app_set_allow_cancel (app, FALSE);
  gs_app_add_source (app, pkg->name);
  gs_app_set_name (app, GS_APP_QUALITY_UNKNOWN, pkg->name);
  gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, pkg->description);
  gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, pkg->url);
  gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, pkg->license);
  gs_app_set_origin (app, "alpine");
  gs_app_set_origin_hostname (app, "alpinelinux.org");
  gs_app_set_management_plugin (app, plugin);
  gs_app_set_size_installed (app, GS_SIZE_TYPE_VALID, pkg->installedSize);
  gs_app_set_size_download (app, GS_SIZE_TYPE_VALID, pkg->size);
  gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE);
  gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "apk");
  gs_app_set_state (app, apk_to_app_state (pkg->packageState));
  gs_app_set_version (app, pkg->version);
  if (gs_app_get_state (app) == GS_APP_STATE_UPDATABLE_LIVE)
    gs_app_set_update_version (app, pkg->stagingVersion);
  gs_plugin_cache_add (plugin, cache_name, app);

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

static void
gs_plugin_apk_init (GsPluginApk *self)
{
  GsPlugin *plugin = GS_PLUGIN (self);

  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "icons");
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "generic-updates");
  /* We want to get packages from appstream and refine them */
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
  self->proxy = NULL;
}

static void
gs_plugin_apk_dispose (GObject *object)
{
  GsPluginApk *self = GS_PLUGIN_APK (object);

  g_clear_object (&self->proxy);

  G_OBJECT_CLASS (gs_plugin_apk_parent_class)->dispose (object);
}

static gboolean
gs_plugin_apk_setup_finish (GsPlugin *plugin,
                            GAsyncResult *result,
                            GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
apk_polkit_proxy_setup_cb (GObject *source_object,
                           GAsyncResult *result,
                           gpointer user_data);

static void
gs_plugin_apk_setup_async (GsPlugin *plugin,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_apk_setup_async);

  g_debug ("Initializing plugin");

  apk_polkit2_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                 G_DBUS_PROXY_FLAGS_NONE,
                                 "dev.Cogitri.apkPolkit2",
                                 "/dev/Cogitri/apkPolkit2",
                                 cancellable,
                                 apk_polkit_proxy_setup_cb,
                                 g_steal_pointer (&task));
}

static void
apk_polkit_proxy_setup_cb (GObject *source_object,
                           GAsyncResult *result,
                           gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginApk *self = g_task_get_source_object (task);
  g_autoptr (GError) local_error = NULL;

  self->proxy = apk_polkit2_proxy_new_for_bus_finish (result, &local_error);
  if (local_error != NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  /* Live update operations can take very, very long */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (self->proxy), G_MAXINT);

  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_apk_refresh_metadata_finish (GsPlugin *plugin,
                                       GAsyncResult *result,
                                       GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void apk_polkit_update_repositories_cb (GObject *source_object,
                                               GAsyncResult *res,
                                               gpointer user_data);

static void
gs_plugin_apk_refresh_metadata_async (GsPlugin *plugin,
                                      guint64 cache_age_secs,
                                      GsPluginRefreshMetadataFlags flags,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GTask) task = NULL;
  g_autoptr (GError) local_error = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_apk_refresh_metadata_async);

  g_debug ("Refreshing repositories");

  gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_DOWNLOADING);
  apk_polkit2_call_update_repositories (self->proxy, cancellable,
                                        apk_polkit_update_repositories_cb,
                                        g_steal_pointer (&task));
}

static void
apk_polkit_update_repositories_cb (GObject *source_object,
                                   GAsyncResult *res,
                                   gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginApk *self = g_task_get_source_object (task);
  g_autoptr (GError) local_error = NULL;

  if (!apk_polkit2_call_update_repositories_finish (self->proxy, res, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  gs_plugin_updates_changed (GS_PLUGIN (self));
  g_task_return_boolean (task, TRUE);
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
                       GsAppList *list,
                       GCancellable *cancellable,
                       GError **error)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GVariant) upgradable_packages = NULL;
  g_autoptr (GError) local_error = NULL;

  /* I believe we have to invalidate the cache here! */
  g_debug ("Adding updates");

  if (!apk_polkit2_call_list_upgradable_packages_sync (self->proxy,
                                                       APK_POLKIT_CLIENT_DETAILS_FLAGS_ALL,
                                                       &upgradable_packages,
                                                       cancellable,
                                                       &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_debug ("Found %" G_GSIZE_FORMAT " upgradable packages",
           g_variant_n_children (upgradable_packages));

  for (gsize i = 0; i < g_variant_n_children (upgradable_packages); i++)
    {
      g_autoptr (GVariant) dict = NULL;
      GsApp *app;
      ApkdPackage pkg = { NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, Available };

      dict = g_variant_get_child_value (upgradable_packages, i);
      /* list_upgradable_packages doesn't have array input, thus no error output */
      if (!gs_plugin_apk_variant_to_apkd (dict, &pkg))
        g_assert_not_reached ();
      if (pkg.packageState == Upgradable || pkg.packageState == Downgradable)
        {
          app = apk_package_to_app (plugin, &pkg);
          gs_app_list_add (list, app);
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
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GError) local_error = NULL;
  g_autofree gchar *source = NULL;

  g_return_val_if_fail (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY, TRUE);

  /* We can only install apps we know of */
  if (!gs_app_has_management_plugin (app, plugin))
    return TRUE;

  source = gs_plugin_apk_get_source (app, error);
  if (source == NULL)
    return FALSE;

  g_debug ("Trying to install app %s", gs_app_get_unique_id (app));
  gs_app_set_progress (app, GS_APP_PROGRESS_UNKNOWN);
  gs_app_set_state (app, GS_APP_STATE_INSTALLING);

  if (!apk_polkit2_call_add_package_sync (self->proxy, source, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      gs_app_set_state_recover (app);
      return FALSE;
    }

  gs_app_set_state (app, GS_APP_STATE_INSTALLED);
  return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
                      GsApp *app,
                      GCancellable *cancellable,
                      GError **error)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GError) local_error = NULL;
  g_autofree gchar *source = NULL;

  g_return_val_if_fail (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY, TRUE);

  /* We can only remove apps we know of */
  if (!gs_app_has_management_plugin (app, plugin))
    return TRUE;

  source = gs_plugin_apk_get_source (app, error);
  if (source == NULL)
    return FALSE;

  g_debug ("Trying to remove app %s", gs_app_get_unique_id (app));
  gs_app_set_progress (app, GS_APP_PROGRESS_UNKNOWN);
  gs_app_set_state (app, GS_APP_STATE_REMOVING);

  if (!apk_polkit2_call_delete_package_sync (self->proxy, source, cancellable, &local_error))
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, g_steal_pointer (&local_error));
      gs_app_set_state_recover (app);
      return FALSE;
    }

  gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
  return TRUE;
}

/**
 * gs_plugin_apk_prepare_update:
 * @plugin: The apk plugin
 * @list: List of desired apps to update
 * @ready: List to store apps once ready to be updated
 *
 * Convenience function which takes a list of apps to update and
 * a list to store apps once they are ready to be updated. It iterate
 * over the apps from @list, takes care that it is possible to update them,
 * and when they are ready to be updated, adds them to @ready.
 *
 * Returns: Number of non-proxy apps added to the list
 **/
static unsigned int
gs_plugin_apk_prepare_update (GsPlugin *plugin,
                              GsAppList *list,
                              GsAppList *ready)
{
  unsigned int added = 0;

  for (guint i = 0; i < gs_app_list_length (list); i++)
    {
      GsApp *app;
      app = gs_app_list_index (list, i);

      /* We shall only touch the apps if they are are owned by us or
       * a proxy (and thus might contain some apps owned by us) */
      if (gs_app_has_quirk (app, GS_APP_QUIRK_IS_PROXY))
        {
          unsigned int proxy_added;
          proxy_added = gs_plugin_apk_prepare_update (plugin, gs_app_get_related (app), ready);
          if (proxy_added)
            {
              gs_app_set_state (app, GS_APP_STATE_INSTALLING);
              gs_app_list_add (ready, app);
              added += proxy_added;
            }
          continue;
        }

      if (!gs_app_has_management_plugin (app, plugin))
        {
          g_debug ("Ignoring update on '%s', not owned by APK",
                   gs_app_get_unique_id (app));
          continue;
        }

      gs_app_set_state (app, GS_APP_STATE_INSTALLING);
      gs_app_list_add (ready, app);
      added++;
    }
  return added;
}

static void
upgrade_apk_packages_cb (GObject *object_source,
                         GAsyncResult *res,
                         gpointer user_data);

static void
gs_plugin_apk_update_apps_async (GsPlugin *plugin,
                                 GsAppList *list,
                                 GsPluginUpdateAppsFlags flags,
                                 GsPluginProgressCallback progress_callback,
                                 gpointer progress_user_data,
                                 GsPluginAppNeedsUserActionCallback app_needs_user_action_callback,
                                 gpointer app_needs_user_action_data,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GTask) task = NULL;
  GsAppList *list_installing = gs_app_list_new ();
  unsigned int num_sources;
  g_autofree const gchar **source_array = NULL;

  g_debug ("Updating apps");

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_apk_update_apps_async);

  if (!(flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_DOWNLOAD))
    {
      // This needs polkit changes. Ideally we'd download first, and
      // apply from cache then. We should probably test this out in
      // pmOS release upgrader script first
      g_warning ("We don't implement 'NO_DOWNLOAD'");
    }

  if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  /* update UI as this might take some time */
  gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_WAITING);

  num_sources = gs_plugin_apk_prepare_update (plugin, list, list_installing);

  g_debug ("Found %u apps to update", num_sources);

  source_array = g_new0 (const gchar *, num_sources + 1);
  for (int i = 0; i < num_sources;)
    {
      GsApp *app = gs_app_list_index (list_installing, i);
      const gchar *source = gs_app_get_source_default (app);
      if (source)
        {
          source_array[i] = source;
          i++;
        }
    }

  g_task_set_task_data (task, g_steal_pointer (&list_installing), g_object_unref);
  apk_polkit2_call_upgrade_packages (self->proxy, source_array, cancellable,
                                     upgrade_apk_packages_cb, g_steal_pointer (&task));
}

static void
upgrade_apk_packages_cb (GObject *object_source,
                         GAsyncResult *res,
                         gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (g_steal_pointer (&user_data));
  GsPluginApk *self = g_task_get_source_object (task);
  GsAppList *list_installing = g_task_get_task_data (task);
  g_autoptr (GError) local_error = NULL;

  if (!apk_polkit2_call_upgrade_packages_finish (self->proxy, res, &local_error))
    {
      /* When and upgrade transaction failed, it could be out of two reasons:
       * - The world constraints couldn't match. In that case, nothing was
       * updated and we are safe to set all the apps to the recover state.
       * - Actual errors happened! Could be a variety of things, including
       * network timeouts, errors in packages' ownership and what not. This
       * is dangerous, since the transaction was run half-way. Show an error
       * that the user should run `apk fix` and that the system might be in
       * an inconsistent state. We also have no idea of which apps succeded
       * and which didn't, so also recover everything and hope the refine
       * takes care of fixing things in the aftermath. */
      g_dbus_error_strip_remote_error (local_error);
      for (int i = 0; i < gs_app_list_length (list_installing); i++)
        {
          GsApp *app = gs_app_list_index (list_installing, i);
          gs_app_set_state_recover (app);
        }

      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  for (int i = 0; i < gs_app_list_length (list_installing); i++)
    {
      GsApp *app = gs_app_list_index (list_installing, i);
      gs_app_set_state (app, GS_APP_STATE_INSTALLED);
    }

  g_debug ("All apps updated correctly");

  gs_plugin_updates_changed (GS_PLUGIN (self));
  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_apk_update_apps_finish (GsPlugin *plugin,
                                  GAsyncResult *result,
                                  GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
  g_debug ("App to adopt: %s", gs_app_get_id (app));

  if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE && gs_app_get_scope (app) == AS_COMPONENT_SCOPE_SYSTEM)
    gs_app_set_management_plugin (app, plugin);

  if (gs_app_get_kind (app) == AS_COMPONENT_KIND_OPERATING_SYSTEM)
    gs_app_set_management_plugin (app, plugin);
}

/**
 * set_app_metadata:
 * @plugin: The apk GsPlugin.
 * @app: The GsApp for which we want to set the metadata on.
 * @package: The ApkdPackage to get the metadata from.
 *
 * Helper function to set the right metadata items on an app.
 **/
static void
set_app_metadata (GsPlugin *plugin, GsApp *app, ApkdPackage *package)
{
  if (package->version)
    gs_app_set_version (app, package->version);

  if (package->description)
    gs_app_set_summary (app, GS_APP_QUALITY_UNKNOWN, package->description);

  if (package->size)
    gs_app_set_size_download (app, GS_SIZE_TYPE_VALID, package->size);

  if (package->installedSize)
    gs_app_set_size_installed (app, GS_SIZE_TYPE_VALID, package->installedSize);

  if (package->url)
    gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, package->url);

  if (package->license)
    gs_app_set_license (app, GS_APP_QUALITY_UNKNOWN, package->license);

  g_debug ("State for pkg %s: %u", gs_app_get_unique_id (app), package->packageState);
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
      gs_app_set_state (app, apk_to_app_state (package->packageState));
    case GS_APP_STATE_AVAILABLE:
    case GS_APP_STATE_INSTALLED:
      break; /* Ignore changes between the states */
    default:
      g_warning ("Wrong state transition detected and avoided!");
      break;
    }

  if (gs_app_get_origin (app) == NULL)
    gs_app_set_origin (app, "alpine");
  if (g_strcmp0 (gs_app_get_source_default (app), package->name) != 0)
    gs_app_add_source (app, package->name);
  gs_app_set_management_plugin (app, plugin);
  gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
}

typedef struct
{
  GsAppList *missing_pkgname_list; /* (owned) (nullable) */
  GsAppList *refine_apps_list;     /* (owned) (not nullable) */
  GsPluginRefineFlags flags;
} RefineData;

static void
refine_data_free (RefineData *data)
{
  g_clear_object (&data->missing_pkgname_list);
  g_clear_object (&data->refine_apps_list);

  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RefineData, refine_data_free);

static void
refine_apk_packages_cb (GObject *object_source,
                        GAsyncResult *res,
                        gpointer user_data);

static void
fix_app_missing_appstream_async (GsPlugin *plugin,
                                 GsAppList *list,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

static gboolean
gs_plugin_apk_refine_finish (GsPlugin *plugin,
                             GAsyncResult *result,
                             GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_apk_refine_async (GsPlugin *plugin,
                            GsAppList *list,
                            GsPluginRefineFlags flags,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GsAppList) missing_pkgname_list = gs_app_list_new ();
  g_autoptr (GsAppList) refine_apps_list = gs_app_list_new ();
  RefineData *data = g_new0 (RefineData, 1);

  data->missing_pkgname_list = g_object_ref (missing_pkgname_list);
  data->refine_apps_list = g_object_ref (refine_apps_list);
  data->flags = flags;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_apk_refine_async);
  g_task_set_task_data (task, data, (GDestroyNotify) refine_data_free);

  g_debug ("Starting refinining process");

  for (guint i = 0; i < gs_app_list_length (list); i++)
    {
      GsApp *app = gs_app_list_index (list, i);
      GPtrArray *sources;
      AsBundleKind bundle_kind = gs_app_get_bundle_kind (app);

      if (gs_app_has_quirk (app, GS_APP_QUIRK_IS_WILDCARD) ||
          gs_app_get_kind (app) == AS_COMPONENT_KIND_REPOSITORY)
        {
          g_debug ("App %s has quirk WILDCARD or is a repository; not refining!", gs_app_get_unique_id (app));
          continue;
        }

      /* Only package and unknown (desktop or metainfo file with upstream AS)
       * belog to us */
      if (bundle_kind != AS_BUNDLE_KIND_UNKNOWN &&
          bundle_kind != AS_BUNDLE_KIND_PACKAGE)
        {
          g_debug ("App %s has bundle kind %s; not refining!", gs_app_get_unique_id (app),
                   as_bundle_kind_to_string (bundle_kind));
          continue;
        }

      /* set management plugin for system apps just created by appstream */
      if (gs_app_has_management_plugin (app, NULL) &&
          gs_app_get_scope (app) == AS_COMPONENT_SCOPE_SYSTEM &&
          g_strcmp0 (gs_app_get_metadata_item (app, "GnomeSoftware::Creator"), "appstream") == 0)
        {
          /* If appstream couldn't assign a source, it means the app does not
           * have an entry in the distribution-generated metadata. That should
           * be fixed in the app. We try to workaround it by finding the
           * owner of the metainfo or desktop file */
          if (gs_app_get_source_default (app) == NULL)
            {
              g_debug ("App %s missing pkgname. Will try to resolve via metainfo/desktop file",
                       gs_app_get_unique_id (app));
              gs_app_list_add (missing_pkgname_list, app);
              continue;
            }

          g_debug ("Setting ourselves as management plugin for app %s", gs_app_get_unique_id (app));
          gs_app_set_management_plugin (app, plugin);
        }

      if (!gs_app_has_management_plugin (app, plugin))
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

      /* If we reached here, the app is valid and under our responsibility.
         Therefore, we have to make sure that it stays valid. For that purpose,
         if the state is unknown, force refining by setting the SETUP_ACTION
         flag. This has the drawback that it forces a refine for all other apps.
         The alternative would be to have yet another app list. But since this
         is expected to happen very seldomly, it should be fine */
      if (gs_app_get_state (app) == GS_APP_STATE_UNKNOWN)
        data->flags |= GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION;

      gs_app_list_add (refine_apps_list, app);
    }

  fix_app_missing_appstream_async (plugin, missing_pkgname_list,
                                   cancellable, refine_apk_packages_cb,
                                   g_steal_pointer (&task));
}

static void
apk_polkit_search_files_owners_cb (GObject *object_source,
                                   GAsyncResult *res,
                                   gpointer user_data);

/**
 * fix_app_missing_appstream:
 * @plugin: The apk GsPlugin.
 * @list: The GsAppList to resolve the metainfo/desktop files for.
 * @cancellable: GCancellable to cancel synchronous dbus call.
 * @task: FIXME!
 *
 * If the appstream plugin could not find the apps in the distribution metadata,
 * it might have created the application from the metainfo or desktop files
 * installed. It will contain some basic information, but the apk package to
 * which it belongs (the source) needs to completed by us.
 **/
static void
fix_app_missing_appstream_async (GsPlugin *plugin,
                                 GsAppList *list,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autofree const gchar **fn_array = NULL;
  g_autoptr (GsAppList) search_list = gs_app_list_new ();
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, fix_app_missing_appstream_async);
  g_task_set_task_data (task, g_object_ref (list), g_object_unref);

  if (gs_app_list_length (list) == 0)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  g_debug ("Trying to find source packages for %d apps", gs_app_list_length (list));
  /* The appstream plugin sets some metadata on apps that come from desktop
   * and metainfo files. If metadata is missing, just give-up */
  for (int i = 0; i < gs_app_list_length (list); i++)
    {
      GsApp *app = gs_app_list_index (list, i);
      if (gs_app_get_metadata_item (app, "appstream::source-file"))
        gs_app_list_add (search_list, app);
      else
        g_warning ("Couldn't find 'appstream::source-file' metadata for %s",
                   gs_app_get_unique_id (app));
    }

  fn_array = g_new0 (const gchar *, gs_app_list_length (search_list) + 1);
  for (int i = 0; i < gs_app_list_length (search_list); i++)
    {
      GsApp *app = gs_app_list_index (search_list, i);
      fn_array[i] = gs_app_get_metadata_item (app, "appstream::source-file");
      g_assert (fn_array[i]);
    }

  if (gs_app_list_length (search_list) == 0)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  apk_polkit2_call_search_files_owners (self->proxy, fn_array,
                                        APK_POLKIT_CLIENT_DETAILS_FLAGS_NONE,
                                        cancellable, apk_polkit_search_files_owners_cb,
                                        g_steal_pointer (&task));
}

static void
apk_polkit_search_files_owners_cb (GObject *object_source,
                                   GAsyncResult *res,
                                   gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginApk *self = g_task_get_source_object (task);
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) search_results = NULL;
  GsAppList *search_list = g_task_get_task_data (task);

  if (!apk_polkit2_call_search_files_owners_finish (self->proxy, &search_results, res, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  g_assert (g_variant_n_children (search_results) == gs_app_list_length (search_list));
  for (int i = 0; i < gs_app_list_length (search_list); i++)
    {
      g_autoptr (GVariant) apk_pkg_variant = NULL;
      GsApp *app = gs_app_list_index (search_list, i);
      ApkdPackage apk_pkg = { NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, Available };

      apk_pkg_variant = g_variant_get_child_value (search_results, i);
      if (!gs_plugin_apk_variant_to_apkd (apk_pkg_variant, &apk_pkg))
        {
          // TODO: We need fn_array if we want to print this info!
          /* g_warning ("Couldn't find any package owning file '%s'", */
          /*            fn_array[i]); */
          continue;
        }
      g_debug ("Found pkgname '%s' for app %s: adding source and setting management plugin",
               apk_pkg.name, gs_app_get_unique_id (app));
      gs_app_add_source (app, apk_pkg.name);
      gs_app_set_management_plugin (app, GS_PLUGIN (self));
    }
  g_task_return_boolean (task, TRUE);
}

static gboolean
fix_app_missing_appstream_finish (GsPlugin *plugin,
                                  GAsyncResult *res,
                                  GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
apk_polkit_get_packages_details_cb (GObject *object_source,
                                    GAsyncResult *res,
                                    gpointer user_data);

/**
 * refine_apk_packages_cb:
 * @plugin: The apk GsPlugin.
 * @list: The list of apps to refine.
 * @flags: TheGsPluginRefineFlags which determine what data we add.
 * @cancellable: GCancellable to cancel resolving what app owns the appstream/desktop file.
 * @task: The task to follow the asynchronous execution.
 *
 * Get details from apk package for a list of apps and fill-in requested refine data.
 **/
static void
refine_apk_packages_cb (GObject *object_source,
                        GAsyncResult *res,
                        gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (g_steal_pointer (&user_data));
  GCancellable *cancellable = g_task_get_cancellable (task);
  GsPluginApk *self = g_task_get_source_object (task);
  g_autofree const gchar **source_array = NULL;
  guint details_flags = APK_POLKIT_CLIENT_DETAILS_FLAGS_PACKAGE_STATE;
  RefineData *data = g_task_get_task_data (task);
  GsAppList *list = data->refine_apps_list;
  GsAppList *missing_pkgname_list = data->missing_pkgname_list;
  GsPluginRefineFlags flags = data->flags;
  g_autoptr (GError) local_error = NULL;

  if (!fix_app_missing_appstream_finish (GS_PLUGIN (self), res, &local_error))
    {
      // TODO: We should print a warning, but continue execution!
      // There's no reason failing to find some package should stop
      // the rest of the processing.
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  if (!(flags &
        (GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
         GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN |
         GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION |
         GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
         GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE |
         GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL |
         GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE)))
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  for (int i = 0; i < gs_app_list_length (missing_pkgname_list); i++)
    {
      GsApp *app = gs_app_list_index (missing_pkgname_list, i);
      if (gs_app_get_source_default (app) != NULL)
        gs_app_list_add (list, app);
    }

  if (gs_app_list_length (list) == 0)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION)
    details_flags |= APK_POLKIT_CLIENT_DETAILS_FLAGS_ALL;
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION)
    details_flags |= APK_POLKIT_CLIENT_DETAILS_FLAGS_VERSION;
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION)
    details_flags |= APK_POLKIT_CLIENT_DETAILS_FLAGS_DESCRIPTION;
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE)
    details_flags |= (APK_POLKIT_CLIENT_DETAILS_FLAGS_SIZE |
                      APK_POLKIT_CLIENT_DETAILS_FLAGS_INSTALLED_SIZE);
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL)
    details_flags |= APK_POLKIT_CLIENT_DETAILS_FLAGS_URL;
  if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE)
    details_flags |= APK_POLKIT_CLIENT_DETAILS_FLAGS_LICENSE;

  source_array = g_new0 (const gchar *, gs_app_list_length (list) + 1);
  for (int i = 0; i < gs_app_list_length (list); i++)
    {
      GsApp *app = gs_app_list_index (list, i);
      source_array[i] = gs_app_get_source_default (app);
    }
  source_array[gs_app_list_length (list)] = NULL;

  apk_polkit2_call_get_packages_details (self->proxy, source_array,
                                         details_flags,
                                         cancellable,
                                         apk_polkit_get_packages_details_cb,
                                         g_steal_pointer (&task));
}

static void
apk_polkit_get_packages_details_cb (GObject *object_source,
                                    GAsyncResult *res,
                                    gpointer user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginApk *self = g_task_get_source_object (task);
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GVariant) apk_pkgs = NULL;
  RefineData *data = g_task_get_task_data (task);
  GsAppList *list = data->refine_apps_list;

  if (!apk_polkit2_call_get_packages_details_finish (self->proxy, &apk_pkgs, res, &local_error))
    {
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  g_assert (gs_app_list_length (list) == g_variant_n_children (apk_pkgs));
  for (int i = 0; i < gs_app_list_length (list); i++)
    {
      g_autoptr (GVariant) apk_pkg_variant = NULL;
      GsApp *app = gs_app_list_index (list, i);
      const gchar *source = gs_app_get_source_default (app);
      ApkdPackage apk_pkg = { NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, Available };

      g_debug ("Refining %s", gs_app_get_unique_id (app));
      apk_pkg_variant = g_variant_get_child_value (apk_pkgs, i);
      if (!gs_plugin_apk_variant_to_apkd (apk_pkg_variant, &apk_pkg))
        {
          if (g_strcmp0 (source, apk_pkg.name) != 0)
            g_warning ("source: '%s' and the pkg name: '%s' differ", source, apk_pkg.name);
          continue;
        }

      if (g_strcmp0 (source, apk_pkg.name) != 0)
        {
          g_warning ("source: '%s' and the pkg name: '%s' differ", source, apk_pkg.name);
          continue;
        }
      set_app_metadata (GS_PLUGIN (self), app, &apk_pkg);
      /* We should only set generic apps for OS updates */
      if (gs_app_get_kind (app) == AS_COMPONENT_KIND_GENERIC)
        gs_app_set_special_kind (app, GS_APP_SPECIAL_KIND_OS_UPDATE);
    }

  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_apk_filter_desktop_file_cb (GsPlugin *plugin,
                                      GsApp *app,
                                      const gchar *filename,
                                      GKeyFile *key_file)
{
  return strstr (filename, "/snapd/") == NULL &&
         strstr (filename, "/snap/") == NULL &&
         strstr (filename, "/flatpak/") == NULL &&
         g_key_file_has_group (key_file, "Desktop Entry") &&
         !g_key_file_has_key (key_file, "Desktop Entry", "X-Flatpak", NULL) &&
         !g_key_file_has_key (key_file, "Desktop Entry", "X-SnapInstanceName", NULL);
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
                  GsApp *app,
                  GCancellable *cancellable,
                  GError **error)
{
  /* only process this app if was created by this plugin */
  if (!gs_app_has_management_plugin (app, plugin))
    return TRUE;

  return gs_plugin_app_launch_filtered (plugin, app, gs_plugin_apk_filter_desktop_file_cb, NULL, error);
}

gboolean
gs_plugin_add_sources (GsPlugin *plugin,
                       GsAppList *list,
                       GCancellable *cancellable,
                       GError **error)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GVariant) repositories = NULL;
  g_autoptr (GError) local_error = NULL;

  g_debug ("Adding repositories");

  if (!apk_polkit2_call_list_repositories_sync (self->proxy, &repositories, cancellable, &local_error))
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
      g_variant_get (value_tuple, "(bss)", &enabled, &description, &url);

      app = gs_plugin_cache_lookup (GS_PLUGIN (self), url);
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
      gs_app_set_management_plugin (app, plugin);
      gs_plugin_cache_add (plugin, url, app);
      gs_app_list_add (list, g_steal_pointer (&app));
    }

  g_debug ("Added repositories");

  return TRUE;
}

static void
apk_polkit_remove_repository_cb (GObject *object_source,
                                 GAsyncResult *res,
                                 gpointer user_data);

static void
apk_polkit_add_repository_cb (GObject *object_source,
                              GAsyncResult *res,
                              gpointer user_data);

static void
gs_plugin_apk_repo_update_async (GsPlugin *plugin,
                                 GsApp *repo,
                                 gboolean is_install,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

static gboolean
gs_plugin_apk_install_repository_finish (GsPlugin *plugin,
                                         GAsyncResult *res,
                                         GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
gs_plugin_apk_install_repository_async (GsPlugin *plugin,
                                        GsApp *repo,
                                        GsPluginManageRepositoryFlags flags,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
  g_assert (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY);

  gs_app_set_state (repo, GS_APP_STATE_INSTALLING);

  gs_plugin_apk_repo_update_async (plugin, repo, TRUE, cancellable, callback, user_data);
}

static gboolean
gs_plugin_apk_remove_repository_finish (GsPlugin *plugin,
                                        GAsyncResult *res,
                                        GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
gs_plugin_apk_remove_repository_async (GsPlugin *plugin,
                                       GsApp *repo,
                                       GsPluginManageRepositoryFlags flags,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)

{
  g_assert (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY);

  gs_app_set_state (repo, GS_APP_STATE_REMOVING);

  gs_plugin_apk_repo_update_async (plugin, repo, FALSE, cancellable, callback, user_data);
}

static void
gs_plugin_apk_repo_update_async (GsPlugin *plugin,
                                 GsApp *repo,
                                 gboolean is_install,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  GsPluginApk *self = GS_PLUGIN_APK (plugin);
  g_autoptr (GTask) task = NULL;
  g_autoptr (GError) local_error = NULL;
  const gchar *url = NULL;
  const gchar *action = is_install ? "Install" : "Remov";

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_task_data (task, g_object_ref (repo), g_object_unref);

  if (!gs_app_has_management_plugin (repo, plugin))
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  gs_app_set_progress (repo, GS_APP_PROGRESS_UNKNOWN);

  url = gs_app_get_metadata_item (repo, "apk::repo-url");
  g_debug ("%sing repository %s", action, url);
  if (is_install)
    {
      g_task_set_source_tag (task, gs_plugin_apk_install_repository_async);
      apk_polkit2_call_add_repository (self->proxy,
                                       url,
                                       cancellable,
                                       apk_polkit_add_repository_cb,
                                       g_steal_pointer (&task));
    }
  else
    {
      g_task_set_source_tag (task, gs_plugin_apk_remove_repository_async);
      apk_polkit2_call_remove_repository (self->proxy,
                                          url,
                                          cancellable,
                                          apk_polkit_remove_repository_cb,
                                          g_steal_pointer (&task));
    }
}

static void
apk_polkit_add_repository_cb (GObject *object_source,
                              GAsyncResult *res,
                              gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (g_steal_pointer (&user_data));
  GsPluginApk *self = g_task_get_source_object (task);
  g_autoptr (GError) local_error = NULL;
  GsApp *repo = g_task_get_task_data (task);
  const gchar *url = gs_app_get_metadata_item (repo, "apk::repo-url");

  if (!apk_polkit2_call_add_repository_finish (self->proxy, res, &local_error))
    {
      gs_app_set_state_recover (repo);
      g_dbus_error_strip_remote_error (local_error);
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }
  g_debug ("Installed repository %s", url);
  gs_app_set_state (repo, GS_APP_STATE_INSTALLED);

  g_task_return_boolean (task, TRUE);
}

static void
apk_polkit_remove_repository_cb (GObject *object_source,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (g_steal_pointer (&user_data));
  GsPluginApk *self = g_task_get_source_object (task);
  g_autoptr (GError) local_error = NULL;
  GsApp *repo = g_task_get_task_data (task);
  const gchar *url = gs_app_get_metadata_item (repo, "apk::repo-url");

  if (!apk_polkit2_call_remove_repository_finish (self->proxy, res, &local_error))
    {
      gs_app_set_state_recover (repo);
      g_dbus_error_strip_remote_error (local_error);
      g_task_return_error (task, g_steal_pointer (&local_error));
      return;
    }

  g_debug ("Removed repository %s", url);
  gs_app_set_state (repo, GS_APP_STATE_AVAILABLE);

  g_task_return_boolean (task, TRUE);
}

static void
gs_plugin_apk_class_init (GsPluginApkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

  object_class->dispose = gs_plugin_apk_dispose;

  plugin_class->setup_async = gs_plugin_apk_setup_async;
  plugin_class->setup_finish = gs_plugin_apk_setup_finish;
  plugin_class->refine_async = gs_plugin_apk_refine_async;
  plugin_class->refine_finish = gs_plugin_apk_refine_finish;
  plugin_class->refresh_metadata_async = gs_plugin_apk_refresh_metadata_async;
  plugin_class->refresh_metadata_finish = gs_plugin_apk_refresh_metadata_finish;
  plugin_class->install_repository_async = gs_plugin_apk_install_repository_async;
  plugin_class->install_repository_finish = gs_plugin_apk_install_repository_finish;
  plugin_class->remove_repository_async = gs_plugin_apk_remove_repository_async;
  plugin_class->remove_repository_finish = gs_plugin_apk_remove_repository_finish;
  plugin_class->update_apps_async = gs_plugin_apk_update_apps_async;
  plugin_class->update_apps_finish = gs_plugin_apk_update_apps_finish;
}

GType
gs_plugin_query_type (void)
{
  return GS_TYPE_PLUGIN_APK;
}
