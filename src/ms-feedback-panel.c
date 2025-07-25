/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "ms-feedback-panel"

#include "mobile-settings-config.h"
#include "mobile-settings-enums.h"
#include "ms-audio-devices.h"
#include "ms-audio-device-row.h"
#include "ms-enum-types.h"
#include "ms-feedback-row.h"
#include "ms-sound-row.h"
#include "ms-feedback-panel.h"
#include "ms-util.h"

#include "gvc-mixer-control.h"

#include <phosh-settings-enums.h>

#include <libfeedback.h>
#include <gmobile.h>

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

/* Verbatim from feedbackd */
#define FEEDBACKD_SCHEMA_ID "org.sigxcpu.feedbackd"
#define FEEDBACKD_KEY_PROFILE "profile"
#define FEEDBACKD_KEY_PREFER_FLASH "prefer-flash"
#define FEEDBACKD_KEY_MAX_HAPTIC_STRENGTH "max-haptic-strength"
#define APP_SCHEMA FEEDBACKD_SCHEMA_ID ".application"
#define APP_PREFIX "/org/sigxcpu/feedbackd/application/"

#define NOTIFICATIONS_SCHEMA "sm.puri.phosh.notifications"
#define NOTIFICATIONS_URGENCY_ENUM "sm.puri.phosh.NotificationUrgency"
#define NOTIFICATIONS_WAKEUP_SCREEN_TRIGGERS_KEY "wakeup-screen-triggers"
#define NOTIFICATIONS_WAKEUP_SCREEN_URGENCY_KEY "wakeup-screen-urgency"
#define NOTIFICATIONS_WAKEUP_SCREEN_CATEGORIES_KEY "wakeup-screen-categories"

enum {
  PROP_0,
  PROP_FEEDBACK_PROFILE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

typedef enum {
  NOTIFICATION_CATEGORY_CALL,
  NOTIFICATION_CATEGORY_DEVICE,
  NOTIFICATION_CATEGORY_EMAIL,
  NOTIFICATION_CATEGORY_IM,
  NOTIFICATION_CATEGORY_NETWORK,
  NOTIFICATION_CATEGORY_PRESENCE,
  NOTIFICATION_CATEGORY_TRANSFER,
  NOTIFICATION_CATEGORY_LAST
} NotificationCategory;

static const char * const notification_category_names[] = {
  "call",
  "device",
  "email",
  "im",
  "network",
  "presence",
  "transfer",
  NULL
};

typedef struct {
  char      *munged_app_id;
  GAppInfo  *app_info;
  GSettings *settings;
} MsFbdApplication;

struct _MsFeedbackPanel {
  MsPanel                    parent;

  GtkListBox                *app_listbox;
  GtkListBox                *sounds_listbox;
  GHashTable                *known_applications;
  AdwSwitchRow              *quick_silent_switch;
  GtkAdjustment             *haptic_strenth_adj;
  AdwSpinRow                *haptic_strenth_row;

  GSettings                 *settings;
  MsFeedbackProfile          profile;

  GtkWidget                 *prefer_flash;

  GSoundContext             *sound_context;
  GCancellable              *sound_cancel;

  AdwToastOverlay           *toast_overlay;
  AdwToast                  *toast;

  AdwComboRow               *notificationssettings_row;
  GSettings                 *notifications_settings;
  MsPhoshNotificationUrgency notifications_urgency;

  /* Audio Settings */
  GvcMixerControl           *mixer_control;
  MsAudioDevices            *audio_devices;
  GtkListBox                *audio_devices_listbox;
  AdwPreferencesGroup       *sound_settings_group;

  GStrv                      notifications_wakeup_categories;

  AdwSwitchRow              *category_switches[NOTIFICATION_CATEGORY_LAST];
};

G_DEFINE_TYPE (MsFeedbackPanel, ms_feedback_panel, MS_TYPE_PANEL)


static GtkWidget *
create_audio_device_row (gpointer item, gpointer user_data)
{
  MsAudioDevice *audio_device = MS_AUDIO_DEVICE (item);

  return GTK_WIDGET (ms_audio_device_row_new (audio_device));
}


static void
wait_a_bit (gpointer unused)
{
  g_autoptr (LfbEvent) event = lfb_event_new ("message-new-sms");

  if (!lfb_is_initted ())
    return;

  lfb_event_set_feedback_profile (event, "quiet");
  lfb_event_trigger_feedback (event, NULL);
}


static void
on_haptic_strength_changed (void)
{
  /* We don't know when exactly feedbackd picked up the new value so wait a bit */
  g_timeout_add_once (200, wait_a_bit, NULL);
}

static void
stop_playback (MsFeedbackPanel *self)
{
  g_cancellable_cancel (self->sound_cancel);
  g_clear_object (&self->sound_cancel);
  if (self->toast)
    adw_toast_dismiss (self->toast);
}


static void
update_sound_row_playing_state (MsFeedbackPanel *self)
{
  GtkWidget *child;

  g_assert (MS_IS_FEEDBACK_PANEL (self));

  for (child = gtk_widget_get_first_child (GTK_WIDGET (self->sounds_listbox));
       child != NULL;
       child = gtk_widget_get_next_sibling (child)) {
         ms_sound_row_set_playing (MS_SOUND_ROW (child), FALSE);
  }
}


static void
on_toast_dismissed  (AdwToast *toast,  MsFeedbackPanel *self)
{
  g_assert (MS_IS_FEEDBACK_PANEL (self));
  g_debug ("Stopping sound playback");

  update_sound_row_playing_state (self);
  stop_playback (self);
}


static void
stop_sound_activated  (GtkWidget *widget,  const char* action_name, GVariant *parameter)

{
  MsFeedbackPanel *self = MS_FEEDBACK_PANEL (widget);

  g_assert (MS_IS_FEEDBACK_PANEL (self));
  stop_playback (self);
}


static void
on_sound_play_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  gboolean success;

  g_autoptr (GError) err = NULL;
  const char *msg = NULL;
  MsFeedbackPanel *self;

  self = MS_FEEDBACK_PANEL (user_data);
  g_assert (MS_IS_FEEDBACK_PANEL (self));

  success = gsound_context_play_full_finish (GSOUND_CONTEXT (source_object), res, &err);

  if (!success && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {

    if (g_error_matches (err, GSOUND_ERROR, GSOUND_ERROR_NOTFOUND)) {
      msg = _("Sound file does not exist");
    } else if (g_error_matches (err, GSOUND_ERROR, GSOUND_ERROR_CORRUPT)) {
      msg = _("Sound file is corrupt");
    } else {
      msg = _("Failed to play sound");
    }

    update_sound_row_playing_state (self);

    g_warning ("Failed to play sound: %s", err->message);
    adw_toast_set_title (self->toast, msg);
  }

  /* Clear cancellable if unused, if used it's cleared in stop_playback */
  if (success || !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_clear_object (&self->sound_cancel);
}


static void
play_sound_activated  (GtkWidget *widget,  const char* action_name, GVariant *parameter)

{
  MsFeedbackPanel *self = MS_FEEDBACK_PANEL (widget);
  const char *path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *title = NULL;

  path = g_variant_get_string (parameter, NULL);
  g_return_if_fail (!STR_IS_NULL_OR_EMPTY (path));
  g_return_if_fail (GSOUND_IS_CONTEXT (self->sound_context));

  stop_playback (self);

  g_debug ("Playing sound file '%s'", path);
  basename = g_path_get_basename (path);

  if (self->toast == NULL) {
    self->toast = adw_toast_new ("");
    g_signal_connect_object (self->toast, "dismissed", G_CALLBACK (on_toast_dismissed), self, 0);
    adw_toast_set_timeout (self->toast, 0);
  }

  title = g_strdup_printf (_("Playing %s"), basename);
  adw_toast_set_title (self->toast, title);
  adw_toast_overlay_add_toast (self->toast_overlay, g_object_ref (self->toast));

  self->sound_cancel = g_cancellable_new ();
  gsound_context_play_full (self->sound_context,
                            self->sound_cancel,
                            on_sound_play_finished,
                            self,
                            GSOUND_ATTR_MEDIA_FILENAME, path,
                            NULL);
}


static void
app_destroy (gpointer data)
{
  MsFbdApplication *app = data;

  g_clear_object (&app->settings);
  g_clear_object (&app->app_info);
  g_clear_pointer (&app->munged_app_id, g_free);
  g_slice_free (MsFbdApplication, app);
}


static char *
item_feedback_profile_name (AdwEnumListItem   *item,
                            gpointer user_data G_GNUC_UNUSED)
{
  return ms_feedback_profile_to_label (adw_enum_list_item_get_value (item));
}


static gboolean
settings_name_to_profile (GValue *value, GVariant *variant, gpointer user_data)
{
  const char *name;

  name = g_variant_get_string (variant, NULL);

  g_value_set_enum (value, ms_feedback_profile_from_setting (name));

  return TRUE;
}


static GVariant *
settings_profile_to_name (const GValue       *value,
                          const GVariantType *expected_type,
                          gpointer            user_data)
{
  MsFeedbackProfile profile;

  profile = g_value_get_enum (value);

  return g_variant_new_take_string (ms_feedback_profile_to_setting (profile));
}


static void
add_application_row (MsFeedbackPanel *self, MsFbdApplication *app)
{
  GtkWidget *w;
  MsFeedbackRow *row;
  g_autoptr (GIcon) icon = NULL;
  g_autofree char *markup = NULL;
  const char *app_name;

  app_name = g_app_info_get_name (app->app_info);
  if (STR_IS_NULL_OR_EMPTY (app_name))
    return;

  icon = g_app_info_get_icon (app->app_info);
  if (icon == NULL)
    icon = g_themed_icon_new ("application-x-executable");
  else
    g_object_ref (icon);

  row = ms_feedback_row_new ();

  /* TODO: we can move most of this into MsMobileSettingsRow */
  markup = g_markup_escape_text (app_name, -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), markup);
  g_object_set_data_full (G_OBJECT (row), "app", app, app_destroy);
  g_settings_bind_with_mapping (app->settings, FEEDBACKD_KEY_PROFILE,
                                row, "feedback-profile",
                                G_SETTINGS_BIND_DEFAULT,
                                settings_name_to_profile,
                                settings_profile_to_name,
                                NULL, NULL);

  gtk_list_box_append (self->app_listbox, GTK_WIDGET (row));

  w = gtk_image_new_from_gicon (icon);
  gtk_widget_add_css_class (w, "lowres-icon");
  gtk_image_set_icon_size (GTK_IMAGE (w), GTK_ICON_SIZE_LARGE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), w);

  g_hash_table_add (self->known_applications, g_strdup (app->munged_app_id));
}


static void
process_app_info (MsFeedbackPanel *self, GAppInfo *app_info)
{
  MsFbdApplication *app;
  const char *app_id;
  g_autofree char *munged_id = NULL;
  g_autofree char *path = NULL;

  app_id = g_app_info_get_id (app_info);
  if (STR_IS_NULL_OR_EMPTY (app_id))
    return;

  munged_id = ms_munge_app_id (app_id);

  app = g_slice_new (MsFbdApplication);

  path = g_strconcat (APP_PREFIX, munged_id, "/", NULL);
  g_debug ("Monitoring settings path: %s", path);
  app->settings = g_settings_new_with_path (APP_SCHEMA, path);
  app->app_info = g_object_ref (app_info);
  app->munged_app_id = g_steal_pointer (&munged_id);

  if (g_hash_table_contains (self->known_applications, app->munged_app_id))
    return;

  g_debug ("Processing queued application %s", app->munged_app_id);

  add_application_row (self, app);
}


static void
load_apps (MsFeedbackPanel *self)
{
  GList *iter, *apps;

  apps = g_app_info_get_all ();

  for (iter = apps; iter; iter = iter->next) {
    GDesktopAppInfo *app;

    app = G_DESKTOP_APP_INFO (iter->data);
    if (g_desktop_app_info_get_boolean (app, "X-Phosh-UsesFeedback")) {
      g_debug ("App '%s' uses libfeedback", g_app_info_get_id (G_APP_INFO (app)));
      process_app_info (self, G_APP_INFO (app));
    }
  }
  g_list_free_full (apps, g_object_unref);
}


static char *
on_notifications_urgency (AdwEnumListItem *item,
                          gpointer         user_data)
{
  switch (adw_enum_list_item_get_value (item)) {
  case MS_PHOSH_NOTIFICATION_URGENCY_LOW:
    /* Translators: "low" categorizes notifications with minor importance */
    return g_strdup (_("low"));
  case MS_PHOSH_NOTIFICATION_URGENCY_NORMAL:
    /* Translators: "normal" categorizes notifications with standard importance */
    return g_strdup (_("normal"));
  case MS_PHOSH_NOTIFICATION_URGENCY_CRITICAL:
    /* Translators: "critical" categorizes notifications with urgent importance */
    return g_strdup (_("critical"));
  default:
  case MS_PHOSH_NOTIFICATION_NONE:
    /* Translators: "none" is the default for notifications with no specified importance */
    return g_strdup (_("none"));
  }
}

/* Update the wakeup-screen-triggers type based on the notification urgency */
static void
update_wakeup_screen_triggers (MsFeedbackPanel *self)
{
  gboolean wants_urgency;
  gboolean wants_category;

  PhoshNotifyScreenWakeupFlags flags, new_flags;

  switch (self->notifications_urgency) {
  case MS_PHOSH_NOTIFICATION_URGENCY_LOW:
  case MS_PHOSH_NOTIFICATION_URGENCY_NORMAL:
  case MS_PHOSH_NOTIFICATION_URGENCY_CRITICAL:
    wants_urgency = TRUE;
    break;
  case MS_PHOSH_NOTIFICATION_NONE:
  default:
    wants_urgency = FALSE;
  }

  wants_category = !gm_strv_is_null_or_empty (self->notifications_wakeup_categories);

  flags = g_settings_get_flags (self->notifications_settings, NOTIFICATIONS_WAKEUP_SCREEN_TRIGGERS_KEY);
  new_flags = flags ^ (PHOSH_NOTIFY_SCREEN_WAKEUP_FLAG_URGENCY |
                       PHOSH_NOTIFY_SCREEN_WAKEUP_FLAG_CATEGORY);

  if (wants_urgency)
    new_flags |= PHOSH_NOTIFY_SCREEN_WAKEUP_FLAG_URGENCY;

  if (wants_category)
    new_flags |= PHOSH_NOTIFY_SCREEN_WAKEUP_FLAG_CATEGORY;

  if (flags == new_flags)
    return;

  g_settings_set_flags (self->notifications_settings, NOTIFICATIONS_WAKEUP_SCREEN_TRIGGERS_KEY, new_flags);
}

/* Update the sensitivity of the category switches based on the notification urgency */
static void
update_category_switches_sensitivity (MsFeedbackPanel *self)
{
  gboolean sensitive = (self->notifications_urgency != MS_PHOSH_NOTIFICATION_NONE);

  for (int i = 0; i < NOTIFICATION_CATEGORY_LAST; i++)
    gtk_widget_set_sensitive (GTK_WIDGET (self->category_switches[i]), sensitive);
}


static guint
notifications_urgency_to_combo_pos (MsPhoshNotificationUrgency urgency)
{
  return urgency + 1;
}


static MsPhoshNotificationUrgency
combo_pos_to_notifications_urgency (guint pos)
{
  return pos - 1;
}


static void
on_notifications_settings_changed (MsFeedbackPanel *self)
{
  MsPhoshNotificationUrgency urgency;
  PhoshNotifyScreenWakeupFlags flags;

  urgency = g_settings_get_enum (self->notifications_settings, NOTIFICATIONS_WAKEUP_SCREEN_URGENCY_KEY);
  flags = g_settings_get_flags (self->notifications_settings, NOTIFICATIONS_WAKEUP_SCREEN_TRIGGERS_KEY);

  if (!(flags & PHOSH_NOTIFY_SCREEN_WAKEUP_FLAG_URGENCY))
    urgency = MS_PHOSH_NOTIFICATION_NONE;

  adw_combo_row_set_selected (self->notificationssettings_row, notifications_urgency_to_combo_pos (urgency));
}


static void
change_notifications_settings (MsFeedbackPanel *self)
{
  guint pos;
  MsPhoshNotificationUrgency urgency;

  pos = adw_combo_row_get_selected (self->notificationssettings_row);
  urgency = combo_pos_to_notifications_urgency (pos);

  if (urgency == self->notifications_urgency)
    return;
  self->notifications_urgency = urgency;

  if (urgency != MS_PHOSH_NOTIFICATION_NONE)
    g_settings_set_enum (self->notifications_settings, NOTIFICATIONS_WAKEUP_SCREEN_URGENCY_KEY, urgency);

  update_category_switches_sensitivity (self);
  update_wakeup_screen_triggers (self);
}


static void
sync_category_switch (MsFeedbackPanel *self)
{
  for (int i = 0; i < NOTIFICATION_CATEGORY_LAST; i++) {
    gboolean on = g_strv_contains ((const char *const *)self->notifications_wakeup_categories,
                                   notification_category_names[i]);
    adw_switch_row_set_active (self->category_switches[i], on);
  }
}


static void
on_wakeup_screen_categories_key_changed (MsFeedbackPanel *self)
{
  g_strfreev (self->notifications_wakeup_categories);
  self->notifications_wakeup_categories = g_settings_get_strv (self->notifications_settings,
                                                     NOTIFICATIONS_WAKEUP_SCREEN_CATEGORIES_KEY);

  sync_category_switch (self);
  update_wakeup_screen_triggers (self);
}


static GStrv
wakeup_categories_append (const char *const *categories, const char *category)
{
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

  if (g_strv_contains (categories, category))
    return g_strdupv ((GStrv)categories);

  for (int i = 0; categories[i]; i++)
    g_strv_builder_add (builder, categories[i]);

  g_strv_builder_add (builder, category);

  return g_strv_builder_end (builder);
}


static GStrv
wakeup_categories_remove (const char *const *categories, const char *category)
{
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

  if (!categories)
    return NULL;

  for (int i = 0; categories[i]; i++) {
    if (g_str_equal (category, categories[i]))
      continue;
    g_strv_builder_add (builder, categories[i]);
  }

  return g_strv_builder_end (builder);
}


static void
on_notification_category_activate_changed (MsFeedbackPanel *self,
                                           GParamSpec      *spec,
                                           AdwSwitchRow    *switch_)
{
  const char *category = NULL;
  g_auto (GStrv) categories = NULL;
  gboolean state;

  for (int i = 0; i < NOTIFICATION_CATEGORY_LAST; i++) {
    if (switch_ == self->category_switches[i]) {
      category = notification_category_names[i];
      break;
    }
  }

  if (!category) {
    g_critical ("Unknown notification wakeup switch");
    return;
  }

  state = adw_switch_row_get_active (switch_);

  if (state) {
    categories = wakeup_categories_append (
      (const char * const *) self->notifications_wakeup_categories, category);
  } else {
    categories = wakeup_categories_remove (
      (const char * const *) self->notifications_wakeup_categories, category);
  }

  g_settings_set_strv (self->notifications_settings, NOTIFICATIONS_WAKEUP_SCREEN_CATEGORIES_KEY,
                       (const char * const *)categories);
}


static void
ms_feedback_panel_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  MsFeedbackPanel *self = MS_FEEDBACK_PANEL (object);

  switch (property_id) {
  case PROP_FEEDBACK_PROFILE:
    self->profile = g_value_get_enum (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
ms_feedback_panel_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  MsFeedbackPanel *self = MS_FEEDBACK_PANEL (object);

  switch (property_id) {
  case PROP_FEEDBACK_PROFILE:
    g_value_set_enum (value, self->profile);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static gboolean
max_haptic_strength_get (GValue *value, GVariant *variant, gpointer user_data)
{
  double strength = g_variant_get_double (variant);

  g_value_set_double (value, (strength * 100));

  return TRUE;
}


static GVariant *
max_haptic_strength_set (const GValue       *value,
                         const GVariantType *expected_type,
                         gpointer            user_data)
{
  guint percent = g_value_get_double (value);

  return g_variant_new_double (0.01 * percent);
}


static void
ms_feedback_panel_constructed (GObject *object)
{
  MsFeedbackPanel *self = MS_FEEDBACK_PANEL (object);
  gboolean found;

  G_OBJECT_CLASS (ms_feedback_panel_parent_class)->constructed (object);

  load_apps (self);

  self->settings = g_settings_new (FEEDBACKD_SCHEMA_ID);
  g_settings_bind_with_mapping (self->settings, FEEDBACKD_KEY_PROFILE,
                                self, "feedback-profile",
                                G_SETTINGS_BIND_DEFAULT,
                                settings_name_to_profile,
                                settings_profile_to_name,
                                NULL, NULL);

  g_settings_bind (self->settings, FEEDBACKD_KEY_PREFER_FLASH,
                   self->prefer_flash, "active",
                   G_SETTINGS_BIND_DEFAULT);

  found = ms_schema_bind_property ("sm.puri.phosh",
                                   "quick-silent",
                                   G_OBJECT (self->quick_silent_switch),
                                   "active",
                                   G_SETTINGS_BIND_DEFAULT);
  gtk_widget_set_visible (GTK_WIDGET (self->quick_silent_switch), found);

  g_settings_bind_with_mapping (self->settings,
                                FEEDBACKD_KEY_MAX_HAPTIC_STRENGTH,
                                G_OBJECT (self->haptic_strenth_adj),
                                "value",
                                G_SETTINGS_BIND_DEFAULT,
                                max_haptic_strength_get,
                                max_haptic_strength_set,
                                NULL,
                                NULL);

  g_signal_connect (self->settings,
                    "changed::" FEEDBACKD_KEY_MAX_HAPTIC_STRENGTH,
                    G_CALLBACK (on_haptic_strength_changed),
                    NULL);
}


static void
ms_feedback_panel_dispose (GObject *object)
{
  MsFeedbackPanel *self = MS_FEEDBACK_PANEL (object);

  g_clear_object (&self->sound_cancel);
  g_clear_object (&self->sound_context);

  g_clear_object (&self->settings);
  g_clear_object (&self->notifications_settings);
  g_strfreev (self->notifications_wakeup_categories);
  g_clear_pointer (&self->known_applications, g_hash_table_unref);

  g_clear_object (&self->audio_devices);
  g_clear_object (&self->mixer_control);

  G_OBJECT_CLASS (ms_feedback_panel_parent_class)->dispose (object);
}


static void
ms_feedback_panel_class_init (MsFeedbackPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = ms_feedback_panel_get_property;
  object_class->set_property = ms_feedback_panel_set_property;
  object_class->constructed = ms_feedback_panel_constructed;
  object_class->dispose = ms_feedback_panel_dispose;

  props[PROP_FEEDBACK_PROFILE] =
    g_param_spec_enum ("feedback-profile", "", "",
                       MS_TYPE_FEEDBACK_PROFILE,
                       MS_FEEDBACK_PROFILE_FULL,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  g_type_ensure (MS_TYPE_FEEDBACK_PROFILE);
  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/mobi/phosh/MobileSettings/ui/ms-feedback-panel.ui");
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, app_listbox);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, audio_devices_listbox);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, haptic_strenth_adj);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, haptic_strenth_row);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, prefer_flash);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, sound_settings_group);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, sounds_listbox);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, quick_silent_switch);
  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, toast_overlay);
  gtk_widget_class_bind_template_callback (widget_class, item_feedback_profile_name);

  gtk_widget_class_bind_template_child (widget_class, MsFeedbackPanel, notificationssettings_row);
  gtk_widget_class_bind_template_callback (widget_class, on_notifications_urgency);
  gtk_widget_class_bind_template_callback (widget_class, change_notifications_settings);

  for (int i = 0; i < NOTIFICATION_CATEGORY_LAST; i++) {
    g_autofree gchar *widget_name = g_strdup_printf ("%s_notifications_wakeup_switch",
                                                     notification_category_names[i]);
    gtk_widget_class_bind_template_child_full (widget_class,
                                               widget_name,
                                               FALSE,
                                               G_STRUCT_OFFSET (MsFeedbackPanel, category_switches[i]));
  }

  gtk_widget_class_bind_template_callback (widget_class, on_notification_category_activate_changed);

  gtk_widget_class_install_action (widget_class, "sound-player.play", "s", play_sound_activated);
  gtk_widget_class_install_action (widget_class, "sound-player.stop", NULL, stop_sound_activated);
}


static void
ms_feedback_panel_init_audio (MsFeedbackPanel *self)
{
  self->mixer_control = gvc_mixer_control_new (_("Mobile Settings Volume Control"));
  g_return_if_fail (self->mixer_control);
  gvc_mixer_control_open (self->mixer_control);
  self->audio_devices = ms_audio_devices_new (self->mixer_control, FALSE);
  gtk_list_box_bind_model (self->audio_devices_listbox,
                           G_LIST_MODEL (self->audio_devices),
                           create_audio_device_row,
                           self,
                           NULL);

  g_object_bind_property (self->audio_devices,
                          "has-devices",
                          self->sound_settings_group,
                          "visible",
                          G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
}


static void
ms_feedback_panel_init (MsFeedbackPanel *self)
{
  g_autoptr (GError) error = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Notifications settings */
  self->notifications_settings = g_settings_new (NOTIFICATIONS_SCHEMA);
  self->notifications_wakeup_categories = g_settings_get_strv (self->notifications_settings,
                                                     NOTIFICATIONS_WAKEUP_SCREEN_CATEGORIES_KEY);

  g_signal_connect_object (self->notifications_settings,
                           "changed::" NOTIFICATIONS_WAKEUP_SCREEN_URGENCY_KEY,
                           G_CALLBACK (on_notifications_settings_changed),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->notifications_settings,
                           "changed::"
                           NOTIFICATIONS_WAKEUP_SCREEN_TRIGGERS_KEY,
                           G_CALLBACK (on_notifications_settings_changed),
                           self,
                           G_CONNECT_SWAPPED);
  on_notifications_settings_changed (self);

  g_signal_connect_swapped (self->notifications_settings,
                            "changed::" NOTIFICATIONS_WAKEUP_SCREEN_CATEGORIES_KEY,
                            G_CALLBACK (on_wakeup_screen_categories_key_changed), self);
  on_wakeup_screen_categories_key_changed (self);

  update_category_switches_sensitivity (self);

  self->known_applications = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    NULL, g_free);

  self->sound_context = gsound_context_new (NULL, &error);
  if (self->sound_context == NULL)
    g_warning ("Failed to make sound context: %s", error->message);

  ms_feedback_panel_init_audio (self);
}


MsFeedbackPanel *
ms_feedback_panel_new (void)
{
  return MS_FEEDBACK_PANEL (g_object_new (MS_TYPE_FEEDBACK_PANEL, NULL));
}
