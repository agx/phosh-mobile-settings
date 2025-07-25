/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "ms-panel-switcher"

#include "mobile-settings-config.h"

#include "ms-panel-switcher.h"
#include "ms-panel.h"
#include "ms-util.h"

enum {
  ROW_ACTIVATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

enum {
  PROP_0,
  PROP_STACK,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

struct _MsPanelSwitcher {
  AdwBin              parent;

  GtkListBox         *panels_listbox;
  GtkSelectionModel  *pages;
  GtkFilter          *filter;
  GtkFilterListModel *filtered_panels;
  GtkStack           *stack;

  char               *query;
  char               *current_panelname;
};
G_DEFINE_TYPE (MsPanelSwitcher, ms_panel_switcher, ADW_TYPE_BIN)


static void
on_row_activated (MsPanelSwitcher *self, GtkListBoxRow *row, GtkListBox *listbox)
{
  GtkWidget *child = gtk_list_box_row_get_child (row);
  const char *name;

  name = g_object_get_data (G_OBJECT (child), "ms-panel-name");
  g_set_str (&self->current_panelname, name);

  g_debug ("Activating %s", name);

  gtk_stack_set_visible_child_name (self->stack, name);
  g_signal_emit (self, signals[ROW_ACTIVATED], 0);
}


static gboolean
panels_filter_func (GObject *item, gpointer user_data)
{
  MsPanelSwitcher *self = MS_PANEL_SWITCHER (user_data);
  GtkStackPage *page = GTK_STACK_PAGE (item);
  const char *panelname = gtk_stack_page_get_name (page);
  g_autofree char *query_normalized = NULL;
  g_auto (GStrv) query_words = NULL;
  MsPanel *panel;
  GtkStringList *keywords;

  if (STR_IS_NULL_OR_EMPTY (self->query))
    return TRUE;

  if (g_strcmp0 (panelname, "welcome") == 0)
    return FALSE;

  panel = MS_PANEL (gtk_stack_page_get_child (page));
  keywords = ms_panel_get_keywords (panel);
  query_normalized = ms_normalize_casefold_and_unaccent (self->query);
  query_words = g_strsplit (g_strstrip (query_normalized), " ", 0);

  /* Search is empty, show each panel */
  if (query_words[0] == NULL)
    return TRUE;

  if (!keywords) {
    g_warning ("Could not get `keywords` for panel: %s", panelname);

    return FALSE;
  }

  for (uint i = 0; query_words[i] != NULL; i++) {
    for (uint j = 0; j < g_list_model_get_n_items (G_LIST_MODEL (keywords)); j++) {
      const char *keyword = gtk_string_list_get_string (keywords, j);

      if (g_strstr_len (keyword, -1, query_words[i]) == keyword)
        return TRUE;
    }

    /* Try finally the name */
    if (g_strstr_len (panelname, -1, query_words[i]) != NULL)
      return TRUE;
  }

  return FALSE;
}


static int
get_panel_index_from_name (GListModel *model, const char *panelname)
{
  const char *name;

  for (uint i = 0; i < g_list_model_get_n_items (model); i++) {
    g_autoptr (GtkStackPage) page = g_list_model_get_item (model, i);
    name = gtk_stack_page_get_name (page);

    if (!g_strcmp0 (name, panelname))
      return i;
  }

  return -1;
}


static GtkWidget *
create_panel_row (gpointer object, gpointer user_data)
{
  GtkStackPage *page = GTK_STACK_PAGE (object);
  const char *title, *name, *icon_name;
  GtkWidget *row, *label, *icon;

  name = gtk_stack_page_get_name (page);
  g_return_val_if_fail (name, NULL);

  title = gtk_stack_page_get_title (page);
  label = g_object_new (GTK_TYPE_LABEL, "label", title ?: name,
                        "halign", GTK_ALIGN_START,
                        "hexpand", TRUE,
                        "vexpand", TRUE,
                        NULL);

  icon_name = gtk_stack_page_get_icon_name (page);
  icon = gtk_image_new_from_icon_name (icon_name ?: "image-missing-symbolic");
  g_object_bind_property (page, "icon-name", icon, "icon-name", G_BINDING_DEFAULT);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append (GTK_BOX (row), icon);
  gtk_box_append (GTK_BOX (row), label);

  g_object_set_data_full (G_OBJECT (row), "ms-panel-name", g_strdup (name), g_free);

  return row;
}


static void
ms_panel_switcher_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  MsPanelSwitcher *self = MS_PANEL_SWITCHER (object);

  switch (property_id) {
  case PROP_STACK:
    ms_panel_switcher_set_stack (self, g_value_get_object (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
ms_panel_switcher_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  MsPanelSwitcher *self = MS_PANEL_SWITCHER (object);

  switch (property_id) {
  case PROP_STACK:
    g_value_set_object (value, self->stack);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
ms_panel_switcher_dispose (GObject *object)
{
  MsPanelSwitcher *self = MS_PANEL_SWITCHER (object);

  g_clear_object (&self->stack);
  g_clear_object (&self->filtered_panels);
  g_clear_pointer (&self->query, g_free);
  g_clear_pointer (&self->current_panelname, g_free);

  G_OBJECT_CLASS (ms_panel_switcher_parent_class)->dispose (object);
}


static void
ms_panel_switcher_class_init (MsPanelSwitcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = ms_panel_switcher_get_property;
  object_class->set_property = ms_panel_switcher_set_property;
  object_class->dispose = ms_panel_switcher_dispose;

  props[PROP_STACK] =
    g_param_spec_object ("stack", "", "",
                         GTK_TYPE_STACK,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  signals[ROW_ACTIVATED] = g_signal_new ("row-activated",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST, 0,
                                         NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/mobi/phosh/MobileSettings/ui/ms-panel-switcher.ui");
  gtk_widget_class_bind_template_child (widget_class, MsPanelSwitcher, panels_listbox);
  gtk_widget_class_bind_template_callback (widget_class, on_row_activated);
}


static void
ms_panel_switcher_init (MsPanelSwitcher *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->filter = GTK_FILTER (gtk_custom_filter_new ((GtkCustomFilterFunc)panels_filter_func,
                                                    self,
                                                    NULL));
}


MsPanelSwitcher *
ms_panel_switcher_new (void)
{
  return MS_PANEL_SWITCHER (g_object_new (MS_TYPE_PANEL_SWITCHER, NULL));
}


void
ms_panel_switcher_set_stack (MsPanelSwitcher *self, GtkStack *stack)
{
  g_return_if_fail (MS_IS_PANEL_SWITCHER (self));
  g_return_if_fail (GTK_IS_STACK (stack));

  if (self->stack == stack)
    return;

  g_set_object (&self->stack, stack);

  if (stack) {
    g_clear_object (&self->pages);
    self->pages = gtk_stack_get_pages (stack);

    g_clear_object (&self->filtered_panels);
    self->filtered_panels = gtk_filter_list_model_new (G_LIST_MODEL (self->pages), self->filter);

    gtk_list_box_bind_model (self->panels_listbox,
                             G_LIST_MODEL (self->filtered_panels),
                             create_panel_row,
                             NULL, NULL);
  }

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STACK]);
}


gboolean
ms_panel_switcher_set_active_panel_name (MsPanelSwitcher *self, const char *panel)
{
  int idx;

  g_assert (MS_IS_PANEL_SWITCHER (self));

  idx = get_panel_index_from_name (G_LIST_MODEL (self->filtered_panels), panel);

  if (idx < 0)
    return FALSE;

  gtk_widget_activate (GTK_WIDGET (gtk_list_box_get_row_at_index (self->panels_listbox, idx)));

  return TRUE;
}


gboolean
ms_panel_switcher_set_active_panel_index (MsPanelSwitcher *self, uint idx)
{
  g_autoptr (GtkStackPage) page = NULL;

  g_assert (MS_IS_PANEL_SWITCHER (self));

  page = g_list_model_get_item (G_LIST_MODEL (self->filtered_panels), idx);

  return page ? ms_panel_switcher_set_active_panel_name (self,
                                                         gtk_stack_page_get_name (page)) : FALSE;
}


void
ms_panel_switcher_set_search_query (MsPanelSwitcher *self, const char *cur_query)
{
  g_assert (MS_IS_PANEL_SWITCHER (self));

  if (g_strcmp0 (self->query, cur_query) != 0) {
    g_set_str (&self->query, cur_query);

    gtk_filter_changed (self->filter, GTK_FILTER_CHANGE_DIFFERENT);

    /* Search was just closed so sync filter change with correct row selection */
    if (g_strcmp0 (cur_query, "") == 0) {
      int idx = get_panel_index_from_name (G_LIST_MODEL (self->filtered_panels),
                                           self->current_panelname);
      gtk_list_box_select_row (self->panels_listbox,
                               gtk_list_box_get_row_at_index (self->panels_listbox, idx));
    }
  }
}
