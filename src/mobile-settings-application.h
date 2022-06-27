/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#pragma once

#include "ms-toplevel-tracker.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define MOBILE_SETTINGS_TYPE_APPLICATION (mobile_settings_application_get_type ())

G_DECLARE_FINAL_TYPE (MobileSettingsApplication, mobile_settings_application, MOBILE_SETTINGS, APPLICATION, AdwApplication)

MobileSettingsApplication *mobile_settings_application_new (gchar *application_id,
                                                            GApplicationFlags flags);
GtkWidget *mobile_settings_application_get_device_panel  (MobileSettingsApplication *self);
MsToplevelTracker *mobile_settings_application_get_toplevel_tracker (MobileSettingsApplication *self);

G_END_DECLS