/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2020 Rasmus Thomsen <oss@cogitri.dev>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <apk-polkit/apkd-dbus-client.h>
#include <gnome-software.h>

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
  uint percentage =
      g_variant_get_uint32 (g_variant_get_child_value (parameters, 0));

  /* nothing in progress */
  if (priv->current_app == NULL)
    {
      g_debug ("apk percentage: %u%%", percentage);
      return;
    }
  g_debug ("apk percentage for %s: %u%%",
           gs_app_get_unique_id (priv->current_app), percentage);
  gs_app_set_progress (priv->current_app, percentage);
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

  if (apkd_helper_call_update_repositories_sync (priv->proxy, cancellable, &local_error))
    {
      return TRUE;
    }
  else
    {
      g_propagate_error (error, local_error);
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
  app_name[1] = '\0';
  gs_app_set_state (app, AS_APP_STATE_INSTALLING);

  if (!apkd_helper_call_add_package_sync (priv->proxy, app_name, cancellable, &local_error))
    {
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
  app_name[1] = '\0';
  gs_app_set_state (app, AS_APP_STATE_REMOVING);

  if (!apkd_helper_call_delete_package_sync (priv->proxy, app_name, cancellable, &local_error))
    {
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

  if (!apkd_helper_call_search_for_packages_sync (priv->proxy, (const gchar *const *) values, &search_result, cancellable, &local_error))
    {
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
      app_name[1] = '\0';
      gs_app_set_state (app, AS_APP_STATE_INSTALLING);

      if (!apkd_helper_call_upgrade_package_sync (priv->proxy, app_name, cancellable, &local_error))
        {
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
