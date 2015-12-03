/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2015 Centricular Ltd
 *  Author(s): Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ovg-appwin.h"

#define MAX_ROWS_VISIBLE 5

struct _OvgAppWindow
{
  GtkApplicationWindow parent;
};

struct _OvgAppWindowClass
{
  GtkApplicationWindowClass parent_class;
};

struct _OvgAppWindowPrivate
{
  GtkWidget *header_bar;
  GtkWidget *peers_d;
  GtkWidget *peers_c;
  GtkWidget *peer_entry;
  GtkWidget *peers_video;
};

G_DEFINE_TYPE_WITH_PRIVATE (OvgAppWindow, ovg_app_window,
    GTK_TYPE_APPLICATION_WINDOW);

static GtkWidget* ovg_app_window_peers_d_row_new (OvgAppWindow *win,
    const gchar *label);
static GtkWidget* ovg_app_window_peers_d_row_get (OvgAppWindow *win,
    const gchar * label);
static GtkWidget* ovg_app_window_peers_c_row_new (OvgAppWindow *win,
    const gchar *label);
static GtkWidget* ovg_app_window_peers_c_row_get (OvgAppWindow *win,
    const gchar * label);

static void
update_header_func (GtkListBoxRow * row, GtkListBoxRow * before,
    gpointer user_data)
{
  GtkWidget *current;

  if (before == NULL) {
    gtk_list_box_row_set_header (row, NULL);
    return;
  }

  current = gtk_list_box_row_get_header (row);
  if (current == NULL) {
    current = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_show (current);
    gtk_list_box_row_set_header (row, current);
  }
}

static gboolean
add_peer (OvgAppWindow * win, const gchar * label)
{
  GtkWidget *row;
  OvgAppWindowPrivate *priv;

  if (ovg_app_window_peers_c_row_get (win, label) != NULL)
    return FALSE;

  priv = ovg_app_window_get_instance_private (win);

  row = ovg_app_window_peers_c_row_new (win, label); 
  gtk_list_box_insert (GTK_LIST_BOX (priv->peers_c), row, -1);
  gtk_widget_show_all (priv->peers_c);
  return TRUE;
}

static void
on_peers_d_add (GtkButton * b, OvgAppWindow * win)
{
  gchar *label;
  GtkWidget *row1;

  row1 = g_object_get_data (G_OBJECT (b), "parent-row");
  gtk_widget_set_sensitive (row1, FALSE);

  label = g_object_get_data (G_OBJECT (row1), "peer-name");
  add_peer (win, label);
}

static GtkWidget *
ovg_app_window_peers_d_row_new (OvgAppWindow * win, const gchar * label)
{
  GtkWidget *row, *box, *w;

  row = gtk_list_box_row_new ();

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_set_border_width (GTK_CONTAINER (box), 8);
  gtk_container_add (GTK_CONTAINER (row), box);

  if (label == NULL)
    return row;

  /* Label */
  w = gtk_label_new (label);
  g_object_set_data (G_OBJECT (row), "peer-name",
      (gchar*) gtk_label_get_text (GTK_LABEL (w)));
  gtk_container_add (GTK_CONTAINER (box), w);

  /* Add button */
  w = gtk_button_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_margin_start (w, 8);
  gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
  g_signal_connect (w, "clicked", G_CALLBACK (on_peers_d_add), win);
  g_object_set_data (G_OBJECT (w), "parent-row", row);
  gtk_box_pack_end (GTK_BOX (box), w, FALSE, FALSE, 0);
  gtk_widget_show_all (row);

  return row;
}

static GtkWidget *
ovg_app_window_peers_d_row_get (OvgAppWindow * win, const gchar * label)
{
  GList *children, *l;
  OvgAppWindowPrivate *priv;
  GtkWidget *listbox = NULL;

  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_d));

  for (l = children; l != NULL; l = l->next) {
    gchar *p = g_object_get_data (G_OBJECT (l->data), "peer-name");
    if (g_strcmp0 (p, label) == 0) {
      listbox = l->data;
      break;
    }
  }

  g_list_free (children);
  return listbox;
}

static void
on_peers_c_remove (GtkButton * b, OvgAppWindow * win)
{
  gchar *label;
  GtkWidget *row1, *row2;

  row1 = g_object_get_data (G_OBJECT (b), "parent-row");
  label = g_object_get_data (G_OBJECT (row1), "peer-name");

  row2 = ovg_app_window_peers_d_row_get (win, label); 
  if (row2 != NULL)
    gtk_widget_set_sensitive (row2, TRUE);

  gtk_widget_destroy (row1);
}

static GtkWidget *
ovg_app_window_peers_c_row_new (OvgAppWindow * win, const gchar * label)
{
  GtkWidget *row, *box, *w;

  row = gtk_list_box_row_new ();

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_set_border_width (GTK_CONTAINER (box), 8);
  gtk_container_add (GTK_CONTAINER (row), box);

  /* Label */
  w = gtk_label_new (label);
  g_object_set_data (G_OBJECT (row), "peer-name",
      (gchar*) gtk_label_get_text (GTK_LABEL (w)));
  gtk_container_add (GTK_CONTAINER (box), w);

  /* Add button */
  w = gtk_button_new_from_icon_name ("list-remove-symbolic",
      GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_margin_start (w, 8);
  gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
  g_signal_connect (w, "clicked", G_CALLBACK (on_peers_c_remove), win);
  g_object_set_data (G_OBJECT (w), "parent-row", row);
  gtk_box_pack_end (GTK_BOX (box), w, FALSE, FALSE, 0);
  gtk_widget_show_all (row);

  return row;
}

static GtkWidget *
ovg_app_window_peers_c_row_get (OvgAppWindow * win, const gchar * label)
{
  GList *children, *l;
  OvgAppWindowPrivate *priv;
  GtkWidget *listbox = NULL;

  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_c));

  for (l = children; l != NULL; l = l->next) {
    gchar *p = g_object_get_data (G_OBJECT (l->data), "peer-name");
    if (g_strcmp0 (p, label) == 0) {
      listbox = l->data;
      break;
    }
  }

  g_list_free (children);
  return listbox;
}

static void
on_peer_entry_activated (OvgAppWindow * win)
{
  const gchar *label;
  OvgAppWindowPrivate *priv;

  priv = ovg_app_window_get_instance_private (win);
  label = gtk_entry_get_text (GTK_ENTRY (priv->peer_entry));
  if (g_strcmp0 (label, "") == 0)
    return;
  if (!add_peer (win, label))
    g_warning ("User tried to add duplicate peer address");
}

static void
ovg_app_window_init (OvgAppWindow *win)
{
  GtkWidget *row;
  GtkListBox *peers_d, *peers_c;
  OvgAppWindowPrivate *priv;

  gtk_widget_init_template (GTK_WIDGET (win));

  priv = ovg_app_window_get_instance_private (win);
  peers_d = GTK_LIST_BOX (priv->peers_d);
  peers_c = GTK_LIST_BOX (priv->peers_c);

  gtk_header_bar_set_title (GTK_HEADER_BAR (priv->header_bar), "Connect");

  gtk_list_box_set_header_func (peers_d, update_header_func, NULL, NULL);
  gtk_list_box_set_header_func (peers_c, update_header_func, NULL, NULL);

  row = ovg_app_window_peers_d_row_new (win, "192.168.1.44");
  gtk_list_box_insert (peers_d, row, -1);
  row = ovg_app_window_peers_d_row_new (win, "192.168.1.32");
  gtk_list_box_insert (peers_d, row, -1);
  row = ovg_app_window_peers_d_row_new (win, "192.168.1.154");
  gtk_list_box_insert (peers_d, row, -1);
  row = ovg_app_window_peers_d_row_new (win, "192.168.1.98");
  gtk_list_box_insert (peers_d, row, -1);
  row = ovg_app_window_peers_d_row_new (win, "Fedora-PC1");
  gtk_list_box_insert (peers_d, row, -1);
  gtk_widget_show_all (priv->peers_d);
}

static void
ovg_app_window_class_init (OvgAppWindowClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

  gtk_widget_class_set_template_from_resource (widget_class,
      "/org/gtk/OneVideoGui/ovg-window.ui");
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      header_bar);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peers_d);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peers_c);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peer_entry);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peers_video);

  gtk_widget_class_bind_template_callback (widget_class,
      on_peer_entry_activated);
}

GtkWidget *
ovg_app_window_new (OvgApp *app)
{
  return g_object_new (OVG_TYPE_APP_WINDOW, "application", app, NULL);
}
