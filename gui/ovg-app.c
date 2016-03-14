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

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

struct _OvgApp
{
  GtkApplication parent;
};

struct _OvgAppClass
{
  GtkApplicationClass parent_class;
};

struct _OvgAppPrivate
{
  GtkWidget *window;
  OvLocalPeer *ov_local;
  gchar *scheduled_error;
};

G_DEFINE_TYPE_WITH_PRIVATE (OvgApp, ovg_app, GTK_TYPE_APPLICATION);

#ifdef __linux__
static gchar *device_path = NULL;
#endif
static gchar *iface_name = NULL;
static guint16 iface_port = 0;
static gboolean low_res = FALSE;
static gboolean net_stats = FALSE;

static GOptionEntry app_options[] =
{
#ifdef __linux__
  {"device", 'd', 0, G_OPTION_ARG_STRING, &device_path, "Path to the V4L2"
          " (camera) device; example: /dev/video0", "PATH"},
#endif
  {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
        " to listen on (default: all)", "NAME"},
  {"port", 'p', 0, G_OPTION_ARG_INT, &iface_port, "Override the TCP port to"
        " listen on for incoming connections", "PORT"},
  {"low-res", 0, 0, G_OPTION_ARG_NONE, &low_res, "Send low-resolution video"
        " for testing purposes (default: no)", NULL},
  {"net-stats", 0, 0, G_OPTION_ARG_NONE, &net_stats, "Show network statistics "
        " as calculated via RTCP (default: no)", NULL},
  {NULL}
};

static void
quit_activated (GSimpleAction * action, GVariant * param, gpointer app)
{
  g_application_quit (G_APPLICATION (app));
}

static GActionEntry app_entries[] =
{
  { "quit", quit_activated, NULL, NULL, NULL },
};

#ifdef G_OS_UNIX
static gboolean
on_ovg_app_sigint (GApplication * app)
{
  g_printerr ("SIGINT caught, quitting application...\n");
  quit_activated (NULL, NULL, app);

  return G_SOURCE_REMOVE;
}
#endif

static void
ovg_app_schedule_error (GApplication * app, const gchar * msg)
{
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (app));

  g_return_if_fail (priv->scheduled_error == NULL);

  priv->scheduled_error = g_strdup (msg);
}

static void
ovg_app_init (OvgApp * app)
{
  g_set_prgname ("OneVideo");
  g_set_application_name ("OneVideo");
  gtk_window_set_default_icon_name ("OneVideo");
  g_application_add_main_option_entries (G_APPLICATION (app), app_options);
}

static void
ovg_app_activate (GApplication * app)
{
  OvgAppPrivate *priv;

  g_return_if_fail (OVG_IS_APP (app));
  
  priv = ovg_app_get_instance_private (OVG_APP (app));
  if (!OVG_IS_APP_WINDOW (priv->window))
    priv->window = ovg_app_window_new (OVG_APP (app));

  gtk_window_present (GTK_WINDOW (priv->window));
}

static void
ovg_app_startup (GApplication * app)
{
  GList *devices;
  GstDevice *device;
  GtkBuilder *builder;
  GHashTable *missing;
  GMenuModel *app_menu;
  const gchar *quit_accels[2] = { "<Ctrl>Q", NULL };
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (app));

  G_APPLICATION_CLASS (ovg_app_parent_class)->startup (app);

  /* Setup app menu and accels */
  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   app_entries, G_N_ELEMENTS (app_entries),
                                   app);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "app.quit",
                                         quit_accels);

  builder = gtk_builder_new_from_resource ("/org/gtk/OneVideoGui/ovg-appmenu.ui");
  app_menu = G_MENU_MODEL (gtk_builder_get_object (builder, "appmenu"));
  gtk_application_set_app_menu (GTK_APPLICATION (app), app_menu);
  g_object_unref (builder);

  /* Initiate OneVideo library; listen on all interfaces and default port */
  gst_init (NULL, NULL);
  missing = ov_get_missing_gstreamer_plugins ("gtk");
  if (missing != NULL) {
    GString *msg;
    GHashTableIter iter;
    gpointer key, value;

    msg = g_string_new ("The following GStreamer plugins were not found:\n");
    g_hash_table_iter_init (&iter, missing);
    while (g_hash_table_iter_next (&iter, &key, &value))
      g_string_append_printf (msg, "%s (from package %s)\n", (gchar*) key,
          (gchar*) value);
    g_hash_table_unref (missing);
    ovg_app_schedule_error (app, msg->str);
    g_string_free (msg, TRUE);
    goto out;
  }

  /* This probes available devices at start, so start-up can be slow */
  priv->ov_local = ov_local_peer_new (iface_name, iface_port);
  if (priv->ov_local == NULL) {
    ovg_app_schedule_error (app, "Unable to create local peer!");
    goto out;
  }

  if (!ov_local_peer_start (priv->ov_local)) {
    ovg_app_schedule_error (app, "Unable to start local peer!");
    goto out;
  }

  devices = ov_local_peer_get_video_devices (priv->ov_local);
  device = devices ? GST_DEVICE (devices->data) : NULL;
#ifdef __linux__
  if (device_path != NULL)
    device = ov_get_device_from_device_path (devices, device_path);
#endif

  /* This currently always returns TRUE (aborts on error) */
  ov_local_peer_set_video_device (priv->ov_local, device);
  g_list_free_full (devices, g_object_unref);

out:
#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, (GSourceFunc) on_ovg_app_sigint, app);
#endif
}

static gint
ovg_app_command_line (GApplication * app, GApplicationCommandLine * cmdline)
{
  /* No command-line options; just raise */
  ovg_app_activate (app);
  return 0;
}

static void
ovg_app_shutdown (GApplication * app)
{
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (app));

  if (priv->ov_local)
    ov_local_peer_stop (priv->ov_local);

  G_APPLICATION_CLASS (ovg_app_parent_class)->shutdown (app);
}

static void
ovg_app_dispose (GObject * object)
{
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (object));

  g_clear_object (&priv->window);
  g_clear_object (&priv->ov_local);

  G_OBJECT_CLASS (ovg_app_parent_class)->dispose (object);
}

static void
ovg_app_finalize (GObject * object)
{
  OvgAppPrivate *priv = ovg_app_get_instance_private (OVG_APP (object));

  g_free (priv->scheduled_error);

  G_OBJECT_CLASS (ovg_app_parent_class)->finalize (object);
}

static void
ovg_app_class_init (OvgAppClass * class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GApplicationClass *application_class = G_APPLICATION_CLASS (class);

  application_class->activate = ovg_app_activate;
  application_class->startup = ovg_app_startup;
  application_class->command_line = ovg_app_command_line;
  application_class->shutdown = ovg_app_shutdown;

  object_class->dispose = ovg_app_dispose;
  object_class->finalize = ovg_app_finalize;
}

OvgApp *
ovg_app_new (void)
{
  return g_object_new (OVG_TYPE_APP, "application-id", "org.gtk.OneVideoGui",
      "flags", G_APPLICATION_HANDLES_OPEN, NULL);
}


OvLocalPeer *
ovg_app_get_ov_local_peer (OvgApp * app)
{
  OvgAppPrivate *priv;

  g_return_val_if_fail (OVG_IS_APP (app), NULL);
  priv = ovg_app_get_instance_private (app);
  if (!priv->ov_local)
    return NULL;

  return g_object_ref (priv->ov_local);
}

gchar *
ovg_app_get_scheduled_error (OvgApp * app)
{
  OvgAppPrivate *priv;

  g_return_val_if_fail (OVG_IS_APP (app), NULL);
  priv = ovg_app_get_instance_private (app);
  return priv->scheduled_error;
}

gboolean
ovg_app_get_low_res (OvgApp * app)
{
  return low_res;
}

gboolean
ovg_app_get_show_net_stats (OvgApp * app)
{
  return net_stats;
}
