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

#include <math.h>
#include <string.h>

#define MAX_ROWS_VISIBLE 5
#define PEER_DISCOVER_INTERVAL 5
#define CONNECT_WINDOW_TITLE "Connect"
#define CALL_WINDOW_TITLE "Ongoing Call"

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
  GtkWidget *end_call;

  GtkWidget *outer_box;

  GtkWidget *connect_sidebar;
  GtkWidget *peers_d;
  GtkWidget *peers_c;
  GtkWidget *peer_entry;
  GtkWidget *peer_entry_button;
  GtkWidget *start_call;

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
static gboolean ovg_app_window_reset_state (OvgAppWindow *win);

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
add_peer_to_discovered (OvLocalPeer * local, OvDiscoveredPeer * d,
    OvgAppWindow * win)
{
  gchar *addr_s;
  GtkWidget *row;
  OvgAppWindowPrivate *priv;

  g_return_val_if_fail (OVG_IS_APP_WINDOW (win), FALSE);

  priv = ovg_app_window_get_instance_private (win);

  g_object_get (d, "address-string", &addr_s, NULL);

  row = ovg_app_window_peers_d_row_get (win, addr_s);
  if (row)
    goto out;

  g_print ("Adding new row: %s\n", addr_s);

  row = ovg_app_window_peers_d_row_new (win, addr_s);
  gtk_list_box_insert (GTK_LIST_BOX (priv->peers_d), row, -1);
  gtk_widget_show_all (priv->peers_d);

out:
  /* Attach the latest DiscoveredPeer data to the row */
  g_object_set_data_full (G_OBJECT (row), "peer-data", g_object_ref (d),
      g_object_unref);
  g_free (addr_s);
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

  /* Peer was added, enable calling */
  gtk_widget_set_sensitive (priv->start_call, TRUE);

  row = ovg_app_window_peers_c_row_new (win, label); 
  gtk_list_box_insert (GTK_LIST_BOX (priv->peers_c), row, -1);
  gtk_widget_show_all (priv->peers_c);
  return TRUE;
}

static void
on_peers_d_add_to_connect (GtkButton * b, OvgAppWindow * win)
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
  g_signal_connect (w, "clicked", G_CALLBACK (on_peers_d_add_to_connect), win);
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
  GtkWidget *listboxrow = NULL;

  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_d));

  for (l = children; l != NULL; l = l->next) {
    gchar *p = g_object_get_data (G_OBJECT (l->data), "peer-name");
    if (g_strcmp0 (p, label) == 0) {
      listboxrow = l->data;
      break;
    }
  }

  g_list_free (children);
  return listboxrow;
}

static void
on_peers_c_remove (GtkButton * b, OvgAppWindow * win)
{
  gchar *label;
  GtkWidget *row1, *row2;
  OvgAppWindowPrivate *priv;
  GList *children;

  row1 = g_object_get_data (G_OBJECT (b), "parent-row");
  label = g_object_get_data (G_OBJECT (row1), "peer-name");

  row2 = ovg_app_window_peers_d_row_get (win, label); 
  if (row2 != NULL)
    gtk_widget_set_sensitive (row2, TRUE);

  gtk_widget_destroy (row1);

  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_c));
  if (children == NULL)
    /* All peers were removed from to-connect list; disable calling */
    gtk_widget_set_sensitive (priv->start_call, FALSE);
  g_list_free (children);
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
    OvDiscoveredPeer *peer;
    GInetSocketAddress *addr;
    g_assert (GTK_IS_LIST_BOX_ROW (l->data));
    peer = g_object_get_data (G_OBJECT (l->data), "peer-data");
    if (peer != NULL) {
      g_object_get (peer, "address", &addr, NULL);
      /* Peer was auto-discovered */
      g_ptr_array_add (remotes, addr);
    } else {
      /* Peer was added manually */
      const gchar *name;
      name = g_object_get_data (G_OBJECT (l->data), "peer-name");
      addr = ov_inet_socket_address_from_string (name); 
      g_ptr_array_add (remotes, addr);
    }
  }

  g_list_free (children);
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

static gboolean
ovg_app_window_peers_d_rows_clean_timed_out (OvLocalPeer * local,
    OvgAppWindow * win)
{
  gchar *addr_s;
  gint64 current_time;
  GList *children, *l;
  OvgAppWindowPrivate *priv;

  current_time = g_get_monotonic_time ();
  priv = ovg_app_window_get_instance_private (win);
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_d));

  for (l = children; l != NULL; l = l->next) {
    OvDiscoveredPeer *d;
    gint64 discover_time;

    d = g_object_get_data (G_OBJECT (l->data), "peer-data");
    g_object_get (d, "discover-time", &discover_time, "address-string", &addr_s,
        NULL);
    /* If this peer was discovered more than 2 discovery-intervals ago, it has
     * timed out */
    if ((current_time - discover_time) >
        2 * G_USEC_PER_SEC * PEER_DISCOVER_INTERVAL) {
      g_print ("Removing row peer name: %s (timed out)\n", addr_s);
      gtk_container_remove (GTK_CONTAINER (priv->peers_d),
          GTK_WIDGET (l->data));
    }
  }

  g_list_free (children);
  return TRUE;
}

static void
ovg_app_window_populate_peers_video (OvgAppWindow * win, OvLocalPeer * local,
    GPtrArray * remotes)
{
  guint ii, n_cols;
  gint sidebar_height, child_width;
  OvgAppWindowPrivate *priv;

  priv = ovg_app_window_get_instance_private (win);

  /* We try to fit the videos into a rectangular grid */
  n_cols = (unsigned int) ceilf (sqrtf (remotes->len));
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (priv->peers_video),
      n_cols);
  gtk_widget_get_preferred_height (priv->connect_sidebar, &sidebar_height,
      NULL);
  /* Set child width from the min sidebar height assuming the video is 16:9
   * and taking into account the number of videos to show */
  child_width = (int) floorf ((sidebar_height * 16) / (remotes->len * 9));

  for (ii = 0; ii < remotes->len; ii++) {
    GtkWidget *child, *area;
    OvRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    child = gtk_flow_box_child_new ();
    gtk_widget_set_size_request (child, child_width, -1);
    area = ov_remote_peer_add_gtkglsink (remote);
    gtk_container_add (GTK_CONTAINER (child), area);
    gtk_container_add (GTK_CONTAINER (priv->peers_video), child);
  }
}

static gboolean
ovg_app_window_show_peers_video (OvgAppWindow * win)
{
  OvgAppWindowPrivate *priv;

  priv = ovg_app_window_get_instance_private (win);

  /* Hide */
  gtk_widget_hide (priv->connect_sidebar);

  /* Show */
  gtk_header_bar_set_title (GTK_HEADER_BAR (priv->header_bar),
      CALL_WINDOW_TITLE);
  gtk_widget_show_all (priv->peers_video);
  gtk_widget_show (priv->end_call);
  return G_SOURCE_REMOVE;
}

static gboolean
wrapper_setup_peers_video (OvgAppWindow * win)
{
  OvLocalPeer *local;
  GPtrArray *remotes;
  GtkApplication *app;

  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));
  remotes = ov_local_peer_get_remotes (local);
  g_assert (remotes->len != 0);

  /* Populate the peers_video widget with gtk widget sinks */
  ovg_app_window_populate_peers_video (win, local, remotes);

  ov_local_peer_call_start (local);
  ovg_app_window_show_peers_video (win);

  return G_SOURCE_REMOVE;
}

static void
on_incoming_negotiate_started (OvLocalPeer * local, OvgAppWindow * win)
{
  g_print ("Remotes have replied; continuing negotiation...\n");
}

static void
on_outgoing_negotiate_started (OvLocalPeer * local, OvgAppWindow * win)
{
  on_incoming_negotiate_started (local, win);
}

static void
on_incoming_negotiate_finished (OvLocalPeer * local, OvgAppWindow * win)
{
  g_print ("Negotiation finished, starting call\n");
  /* Ensure gtk+ widget manipulation is only done from the main thread */
  g_main_context_invoke (NULL, (GSourceFunc) wrapper_setup_peers_video, win);
}

static void
on_outgoing_negotiate_finished (OvLocalPeer * local, OvgAppWindow * win)
{
  g_print ("Negotiation finished, starting call\n");
  ov_local_peer_call_start (local);
  /* Ensure gtk+ widget manipulation is only done from the main thread */
  g_main_context_invoke (NULL, (GSourceFunc) ovg_app_window_show_peers_video,
      win);
}

static void
on_outgoing_negotiate_skipped (OvLocalPeer * local, OvPeer * skipped,
    GError * error, OvgAppWindow * win)
{
  g_printerr ("Remote skipped: %s\n", error ? error->message : "Unknown error");
}

static void
on_negotiate_aborted (OvLocalPeer * local, GError * error, OvgAppWindow * win)
{
  g_printerr ("Error while negotiating: %s\n",
      error ? error->message : "Unknown error");
  /* Ensure gtk+ widget manipulation is only done from the main thread */
  g_main_context_invoke (NULL, (GSourceFunc) ovg_app_window_reset_state, win);
}

static gboolean
on_negotiate_incoming (OvLocalPeer * local, OvPeer * incoming,
    OvgAppWindow * win)
{
  g_signal_connect (local, "negotiate-started",
      G_CALLBACK (on_incoming_negotiate_started), win);
  g_signal_connect (local, "negotiate-finished",
      G_CALLBACK (on_incoming_negotiate_finished), win);
  g_signal_connect (local, "negotiate-aborted",
      G_CALLBACK (on_negotiate_aborted), win);
  /* Accept all incoming calls */
  return TRUE;
}

static void
on_call_remotes_hangup (OvLocalPeer * local, OvgAppWindow * win)
{
  g_print ("A remote peer has hung up. Resetting state...\n");
  /* Ensure gtk+ widget manipulation is only done from the main thread */
  g_main_context_invoke (NULL, (GSourceFunc) ovg_app_window_reset_state, win);
}

static void
setup_default_handlers (OvLocalPeer * local, OvgAppWindow * win)
{
  g_signal_connect (local, "negotiate-incoming",
      G_CALLBACK (on_negotiate_incoming), win);
  g_signal_connect (local, "peer-discovered",
      G_CALLBACK (add_peer_to_discovered), win);
  g_signal_connect (local, "discovery-sent",
      G_CALLBACK (ovg_app_window_peers_d_rows_clean_timed_out), win);
  g_signal_connect (local, "call-remotes-hungup",
      G_CALLBACK (on_call_remotes_hangup), win);
}

static gboolean
ovg_app_window_reset_state (OvgAppWindow * win)
{
  OvLocalPeer *local;
  GList *children, *l;
  GtkApplication *app;
  OvgAppWindowPrivate *priv;

  priv = ovg_app_window_get_instance_private (win);
  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));

  /* Hide */
  gtk_widget_hide (priv->end_call);
  gtk_widget_hide (priv->peers_video);

  /* Remove old flowbox children */
  children = gtk_container_get_children (GTK_CONTAINER (priv->peers_video));
  for (l = children; l != NULL; l = l->next)
    gtk_container_remove (GTK_CONTAINER (priv->peers_video),
        GTK_WIDGET (l->data));
  g_list_free (children);

  /* Show */
  gtk_header_bar_set_title (GTK_HEADER_BAR (priv->header_bar),
      CONNECT_WINDOW_TITLE);
  gtk_widget_set_sensitive (priv->start_call, TRUE);
  gtk_widget_show (priv->connect_sidebar);

  gtk_widget_set_size_request (priv->outer_box, -1, -1);

  /* Disconnect all handlers */
  g_signal_handlers_disconnect_by_data (local, win);
  /* Connect default handlers again */
  setup_default_handlers (local, win);

  /* Only call once if dispatched from a main context */
  return G_SOURCE_REMOVE;
}

static gboolean
setup_window (OvgAppWindow * win)
{
  OvLocalPeer *local;
  GtkApplication *app;

  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));

  setup_default_handlers (local, win);

  if (!ov_local_peer_discovery_start (local, PEER_DISCOVER_INTERVAL, NULL))
    g_application_quit (G_APPLICATION (app));

  return G_SOURCE_REMOVE;
}

static void
on_call_peers_button_clicked (OvgAppWindow * win, GtkButton * b)
{
  GPtrArray *remotes;
  GtkApplication *app;
  OvLocalPeer *local;
  guint ii;

  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));

  /* Make it so it can't be clicked twice */
  gtk_widget_set_sensitive (GTK_WIDGET (b), FALSE);

  remotes = ovg_app_window_peers_c_get_addrs (win);
  g_return_if_fail (remotes->len > 0);

  for (ii = 0; ii < remotes->len; ii++) {
    OvRemotePeer *remote;
    GInetSocketAddress *addr;

    addr = g_ptr_array_index (remotes, ii);
    remote = ov_remote_peer_new (local, addr);
    ov_local_peer_add_remote (local, remote);
  }

  g_ptr_array_free (remotes, TRUE);

  remotes = ov_local_peer_get_remotes (local);
  ovg_app_window_populate_peers_video (win, local, remotes);

  g_signal_connect (local, "negotiate-started",
      G_CALLBACK (on_outgoing_negotiate_started), win);
  g_signal_connect (local, "negotiate-skipped-remote",
      G_CALLBACK (on_outgoing_negotiate_skipped), win);
  g_signal_connect (local, "negotiate-finished",
      G_CALLBACK (on_outgoing_negotiate_finished), win);
  g_signal_connect (local, "negotiate-aborted",
      G_CALLBACK (on_negotiate_aborted), win);

  ov_local_peer_negotiate_start (local);
  g_print ("Waiting for remote peers\n");
}

static void
on_end_call_button_clicked (OvgAppWindow * win, GtkButton * b)
{
  GtkApplication *app;
  OvLocalPeer *local;

  app = gtk_window_get_application (GTK_WINDOW (win));
  local = ovg_app_get_ov_local_peer (OVG_APP (app));

  ov_local_peer_call_hangup (local);
  ovg_app_window_reset_state (win);
}

static void
ovg_app_window_init (OvgAppWindow * win)
{
  OvgAppWindowPrivate *priv;

  gtk_widget_init_template (GTK_WIDGET (win));

  priv = ovg_app_window_get_instance_private (win);

  gtk_header_bar_set_title (GTK_HEADER_BAR (priv->header_bar),
      CONNECT_WINDOW_TITLE);

  gtk_list_box_set_header_func (GTK_LIST_BOX (priv->peers_d),
      ovg_list_box_update_header_func, NULL, NULL);
  gtk_list_box_set_header_func (GTK_LIST_BOX (priv->peers_c),
      ovg_list_box_update_header_func, NULL, NULL);

  /* We can only initialize all this once the init is fully chained */
  g_idle_add ((GSourceFunc) setup_window, win);
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
      end_call);

  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      outer_box);

  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      connect_sidebar);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peers_d);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peers_c);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peer_entry);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      peer_entry_button);
  gtk_widget_class_bind_template_child_private (widget_class, OvgAppWindow,
      start_call);

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
  gtk_widget_class_bind_template_callback (widget_class,
      on_end_call_button_clicked);
}

GtkWidget *
ovg_app_window_new (OvgApp *app)
{
  return g_object_new (OVG_TYPE_APP_WINDOW, "application", app, NULL);
}
