/*
 * Copyright (C) 2022 Dylan Van Assche <me@dylanvanassche.be>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#pragma once

#include <glib-object.h>
#include <glib.h>
#include <gnome-software.h>

G_BEGIN_DECLS

#define GS_TYPE_PLUGIN_APK (gs_plugin_apk_get_type ())

G_DECLARE_FINAL_TYPE (GsPluginApk, gs_plugin_apk, GS, PLUGIN_APK, GsPlugin)

G_END_DECLS
