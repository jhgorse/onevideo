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

#include "ovg-app.h"
#include "ovg-appwin.h"

#include "onevideo/utils.h"
#include <string.h>

#define MAX_ROWS_VISIBLE 5
#define PEER_DISCOVER_INTERVAL 5

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
  GtkWidget *peer_entry_button;
  GtkWidget *peers_video;

  GSource *peers_source;
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
widget_set_error (GtkWidget * w, gboolean error)
{
  if (error)
    gtk_style_context_add_class (gtk_widget_get_style_context (w),
        GTK_STYLE_CLASS_ERROR);
  else
    gtk_style_context_remove_class (gtk_widget_get_style_context (w),
        GTK_STYLE_CLASS_ERROR);
}

static void
ovg_list_box_update_header_func (GtkListBoxRow * row, GtkListBoxRow * before,
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
add_peer_to_discovered (OneVideoDiscoveredPeer * d, gpointer user_data)
{
  GtkWidget *row;
  OvgAppWindowPrivate *priv;
  OvgAppWindow *win = user_data;

  g_return_val_if_fail (OVG_IS_APP_WINDOW (win), FALSE);

  priv = ovg_app_window_get_instance_private (win);

  row = ovg_app_window_peers_d_row_get (win, d->addr_s);
  if (row)
    goto out;

  row = ovg_app_window_peers_d_row_new (win, d->addr_s);
  gtk_list_box_insert (GTK_LIST_BOX (priv->peers_d), row, -1);
  gtk_widget_show_all (priv->peers_d);

out:
  /* Attach the latest DiscoveredPeer data to the row */
  g_object_set_data_full (G_OBJECT (row), "peer-data", d,
      (GDestroyNotify) one_video_discovered_peer_free);
  return G_SOURCE_CONTINUE;
}

static gboolean
add_peer_to_connect (OvgAppWindow * win, const gchar * label)
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
on_peers_d_restore (GtkButton * b, OvgAppWindow * win)
{
  gchar *label;
  GtkWidget *row1;

  row1 = g_object_get_data (G_OBJECT (b), "parent-row");
  gtk_widget_set_sensitive (row1, FALSE);

  label = g_object_get_data (G_OBJECT (row1), "peer-name");
  add_peer_to_connect (win, label);
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
  g_signal_connect (w, "clicked", G_CALLBACK (on_peers_d_restore), win);
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

static GPtrArray *
ovg_app_window_peers_c_get_addrs (OvgAppWindow * win)
{
  GList *children, *l;
  GPtrArray *remotes;
  OvgAppWindowPrivate *priv;

  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_c));
  remotes = g_ptr_array_new_full (5, (GDestroyNotify) g_object_unref);

  for (l = children; l != NULL; l = l->next) {
    OneVideoDiscoveredPeer *peer;
    g_assert (GTK_IS_LIST_BOX_ROW (l->data));
    peer = g_object_get_data (G_OBJECT (l->data), "peer-data");
    if (peer != NULL) {
      /* Peer was auto-discovered */
      g_ptr_array_add (remotes, g_object_ref (peer->addr));
    } else {
      /* Peer was added manually */
      const gchar *name;
      GInetSocketAddress *addr;
      name = g_object_get_data (G_OBJECT (l->data), "peer-name");
      addr = one_video_inet_socket_address_from_string (name); 
      g_ptr_array_add (remotes, addr);
    }
  }

  return remotes;
}

static void
on_peer_entry_button_clicked (OvgAppWindow * win, GtkButton * b G_GNUC_UNUSED)
{
  const gchar *label;
  OvgAppWindowPrivate *priv;

  priv = ovg_app_window_get_instance_private (win);

  label = gtk_entry_get_text (GTK_ENTRY (priv->peer_entry));

  if (!add_peer_to_connect (win, label))
    g_warning ("User tried to add duplicate peer address");
}

static void
on_peer_entry_text_changed (OvgAppWindow * win)
{
  gchar **split;
  GtkEntry *entry;
  const gchar *text;
  OvgAppWindowPrivate *priv;
  gboolean text_valid = FALSE;

  priv = ovg_app_window_get_instance_private (win);
  entry = GTK_ENTRY (priv->peer_entry);
  text = gtk_entry_get_text (entry);

  if (strlen (text) > 0)
    gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY,
        "edit-clear-symbolic");
  else
    gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY, NULL);

  split = g_strsplit (text, ":", 2);

  switch (g_strv_length (split)) {
    case 1:
      text_valid = g_hostname_is_ip_address (split[0]);
      break;
    case 2:
      text_valid = g_hostname_is_ip_address (split[0]) && 
        /* XXX: We don't check if it's a valid port; we just check if it's an
         * unsigned integer */
        g_ascii_strtoull (split[1], NULL, 10);
      break;
    default:
      break;
  }

  if (g_strcmp0 (text, "") == 0)
    /* Empty text entry is not an error */
    widget_set_error (priv->peer_entry, FALSE);
  else
    widget_set_error (priv->peer_entry, !text_valid);
  gtk_widget_set_sensitive (priv->peer_entry_button, text_valid);
  g_strfreev (split);
}

static void
on_peer_entry_clear_pressed (OvgAppWindow * win, GtkEntryIconPosition icon_pos,
    GdkEvent * event, GtkEntry * entry)
{
  gtk_entry_set_text (entry, "");
}

static void
ovg_app_window_peers_d_rows_clean_timed_out (OvgAppWindow * win)
{
  gint64 current_time;
  GList *children, *l;
  OvgAppWindowPrivate *priv;

  current_time = g_get_monotonic_time ();
  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_d));

  for (l = children; l != NULL; l = l->next) {
    OneVideoDiscoveredPeer *d;

    d = g_object_get_data (G_OBJECT (l->data), "peer-data");
    /* If this peer was discovered more than 2 discovery-intervals ago, it has
     * timed out */
    if ((current_time - d->discover_time) >
        2 * G_USEC_PER_SEC * PEER_DISCOVER_INTERVAL)
      gtk_container_remove (GTK_CONTAINER (priv->peers_d),
          GTK_WIDGET (l->data));
  }

  g_list_free (children);
}

static gboolean
do_peer_discovery (gpointer user_data)
{
  GtkApplication *app;
  OneVideoLocalPeer *local;
  OvgAppWindowPrivate *priv;
  OvgAppWindow *win = user_data;
  GError *error = NULL;

  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));

  priv = ovg_app_window_get_instance_private (win);

  if (priv->peers_source && g_source_is_destroyed (priv->peers_source))
    /* If already destroyed, just exit */
    return G_SOURCE_REMOVE;

  if (priv->peers_source) {
    /* Cancel existing stuff so we can send the discover message again */
    /* TODO: We don't use g_source_destroy() here because of a strange bug that
     * I wasn't able to track down. Destroying the source was causing all future
     * sources attached to the same socket address to not fire events for
     * incoming unicast UDP messages. It would still fire for incoming multicast
     * messages. */
    g_source_remove (g_source_get_id (priv->peers_source));
    g_source_unref (priv->peers_source);
  }

  priv->peers_source =
    one_video_local_peer_find_remotes_create_source (local, NULL,
      add_peer_to_discovered, win, &error);

  ovg_app_window_peers_d_rows_clean_timed_out (win);

  return G_SOURCE_CONTINUE;
}

static gboolean
do_peer_discovery_once (gpointer user_data)
{
  do_peer_discovery (user_data);

  g_timeout_add_seconds (PEER_DISCOVER_INTERVAL, do_peer_discovery, user_data);

  return G_SOURCE_REMOVE;
}

static void
on_negotiate_done (GObject * source_object, GAsyncResult * res,
    gpointer user_data)
{
  gboolean ret;
  OneVideoLocalPeer *local;
  GError *error = NULL;

  local = g_task_get_task_data (G_TASK (res));
  ret = one_video_local_peer_negotiate_finish (local, res, &error);
  if (ret) {
    g_print ("All remotes have replied.\n");
    one_video_local_peer_start (local);
    return;
  } else {
    if (error != NULL)
      g_printerr ("Error while negotiating: %s\n", error->message);
  }
}

static void
on_call_peers_button_clicked (OvgAppWindow * win, GtkButton * b)
{
  guint ii;
  GPtrArray *remotes;
  GtkApplication *app;
  OneVideoLocalPeer *local;
  OvgAppWindowPrivate *priv;

  /* Make it so it can't be clicked twice */
  gtk_widget_set_sensitive (GTK_WIDGET (b), FALSE);

  priv = ovg_app_window_get_instance_private (win);
  g_source_destroy (priv->peers_source);

  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));

  remotes = ovg_app_window_peers_c_get_addrs (win);
  for (ii = 0; ii < remotes->len; ii++) {
    GInetSocketAddress *addr;
    OneVideoRemotePeer *remote;

    addr = g_ptr_array_index (remotes, ii);
    remote = one_video_remote_peer_new (local, addr);
    one_video_local_peer_add_remote (local, remote);
  }

  one_video_local_peer_negotiate_async (local, NULL, on_negotiate_done, win);
  g_print ("Started async negotiation with peers...");
}

static void
ovg_app_window_init (OvgAppWindow * win)
{
  OvgAppWindowPrivate *priv;

  gtk_widget_init_template (GTK_WIDGET (win));

  priv = ovg_app_window_get_instance_private (win);

  gtk_header_bar_set_title (GTK_HEADER_BAR (priv->header_bar), "Connect");

  gtk_list_box_set_header_func (GTK_LIST_BOX (priv->peers_d),
      ovg_list_box_update_header_func, NULL, NULL);
  gtk_list_box_set_header_func (GTK_LIST_BOX (priv->peers_c),
      ovg_list_box_update_header_func, NULL, NULL);

  /* We can only initialize all this once the init is fully chained */
  g_idle_add (do_peer_discovery_once, win);
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
      peer_entry_button);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peers_video);

  gtk_widget_class_bind_template_callback (widget_class,
      on_peer_entry_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class,
      on_peer_entry_text_changed);
  gtk_widget_class_bind_template_callback (widget_class,
      on_peer_entry_clear_pressed);
  gtk_widget_class_bind_template_callback (widget_class,
      on_call_peers_button_clicked);
}

GtkWidget *
ovg_app_window_new (OvgApp *app)
{
  return g_object_new (OVG_TYPE_APP_WINDOW, "application", app, NULL);
}
