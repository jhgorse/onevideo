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

#include "onevideo/lib.h"
#include "onevideo/utils.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#include <glib-unix.h>

static GMainLoop *loop = NULL;

typedef struct {
  gchar **remotes;
  OvLocalPeer *local;
} FindRemotesData;

static gboolean
found_remote_cb (OvLocalPeer * local, OvDiscoveredPeer * d,
    FindRemotesData * data)
{
  guint len;
  gchar *addr_s;

  if (data->remotes == NULL)
    data->remotes = g_malloc0_n (sizeof (gchar*), 1);

  /* This returns the length minus the trailing NUL */
  len = g_strv_length (data->remotes) + 1;
  /* Expand to include another gchar* pointer */
  data->remotes = g_realloc_n (data->remotes, sizeof (gchar*), len + 1);

  g_object_get (d, "address-string", &addr_s, NULL);
  data->remotes[len - 1] = addr_s;
  data->remotes[len] = NULL;

  return TRUE;
}

static gboolean
kill_remote_peer (OvRemotePeer * remote)
{
  ov_local_peer_remove_remote (remote->local, remote);
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static void
on_call_ended_exit (OvLocalPeer * local)
{
  g_print ("Local peer call ended, exiting...\n");
  ov_local_peer_stop (local);
  g_main_loop_quit (loop);
}

static void
on_call_ended_continue (OvLocalPeer * local)
{
  g_print ("Local peer call ended, continuing...\n");
  ov_local_peer_call_hangup (local);
}

static gboolean
on_app_exit (OvLocalPeer * local)
{
  ov_local_peer_stop (local);
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static gboolean
on_negotiate_incoming (OvLocalPeer * local, OvPeer * peer, gpointer user_data)
{
  gchar *addr_s;

  g_object_get (peer, "address-string", &addr_s, NULL);
  g_print ("Accepting incoming call from %s\n", addr_s);
  g_free (addr_s);

  return TRUE;
}

static void
on_negotiate_finished (OvLocalPeer * local, gpointer user_data)
{
  g_print ("Negotiation finished successfully; starting call\n");
  ov_local_peer_call_start (local);
}

static void
on_negotiate_skipped (OvLocalPeer * local, OvPeer * skipped,
    GError * error, gpointer user_data)
{
  gchar *addr_s;
  g_object_get (skipped, "address-string", &addr_s, NULL);
  g_print ("Remote %s skipped because it did not respond\n", addr_s);
  g_free (addr_s);
}

static void
on_incoming_negotiate_aborted (OvLocalPeer * local, GError * error, gpointer user_data)
{
  g_printerr ("Error while negotiating: %s\n",
      error ? error->message : "Unknown error");
}

static void
on_outgoing_negotiate_aborted (OvLocalPeer * local, GError * error, gpointer user_data)
{
  g_printerr ("Error while negotiating: %s\n",
      error ? error->message : "Unknown error");
  ov_local_peer_stop (local);
  g_main_loop_quit (loop);
}

static void
dial_remotes (OvLocalPeer * local, gchar ** remotes)
{
  guint index;

  g_print ("Dialling remotes...\n");
  for (index = 0; index < g_strv_length (remotes); index++) {
    OvRemotePeer *remote;

    remote = ov_remote_peer_new_from_string (local, remotes[index]);
    ov_local_peer_add_remote (local, remote);

    g_print ("Created and added remote peer %s\n", remote->addr_s);
  }

  g_signal_connect (local, "negotiate-skipped-remote",
      G_CALLBACK (on_negotiate_skipped), NULL);
  ov_local_peer_negotiate_start (local);
  g_print ("Waiting for remotes to reply...\n");
}

static gboolean
aggregate_and_dial_remotes (gpointer user_data)
{
  FindRemotesData *data = user_data;

  if (data->remotes == NULL) {
    g_print (" found no remotes. Exiting.\n");
    ov_local_peer_stop (data->local);
    g_main_loop_quit (loop);
    goto out;
  }

  ov_local_peer_discovery_stop (data->local);
  g_print (" found %u remotes.\n", g_strv_length (data->remotes));
  dial_remotes (data->local, data->remotes);

out:
  g_strfreev (data->remotes);
  g_free (data);
  return G_SOURCE_REMOVE;
}

static gboolean
device_is_in_use (GstDevice * device)
{
  gchar *name;
  GstElement *check, *src, *sink;
  GstStateChangeReturn state_ret;
  GstState state;
  gboolean ret = FALSE;

  return FALSE;

  check = gst_pipeline_new ("test-v4l2");
  src = gst_device_create_element (device, "test-src");
  sink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add_many (GST_BIN (check), src, sink, NULL);
  ret = gst_element_link (src, sink);
  g_assert (ret);
  gst_element_set_state (check, GST_STATE_PLAYING);

  /* Wait for upto 10 seconds in case the state change is ASYNC */
  state_ret = gst_element_get_state (check, &state, NULL, 5*GST_SECOND);
  if (state_ret == GST_STATE_CHANGE_FAILURE) {
    ret = TRUE;
    name = gst_device_get_display_name (device);
    g_printerr ("Unable to use device %s, failing\n", name);
    g_free (name);
  } else if (state_ret == GST_STATE_CHANGE_ASYNC) {
    ret = TRUE;
    name = gst_device_get_display_name (device);
    g_printerr ("Took too long checking device %s, failing\n", name);
    g_free (name);
  } else {
    g_assert (state_ret == GST_STATE_CHANGE_SUCCESS);
    g_assert (state == GST_STATE_PLAYING);
  }

  gst_element_set_state (check, GST_STATE_NULL);
  gst_object_unref (check);
  return ret;
}

static GstDevice *
get_device_choice (GList * devices)
{
  gchar *name;
  GList *device;
  GString *line;
  GIOChannel *channel;
  GError *error = NULL;
  guint num = 0;

  if (devices == NULL) {
    g_printerr ("No video sources detected, using the test video source\n");
    return NULL;
  }

  channel = g_io_channel_unix_new (STDIN_FILENO);
  g_print ("Choose a webcam to use for sending video by entering the "
      "number next to the name:\n");
  g_print ("Test video source [%u]\n", num);
  num += 1;
  for (device = devices; device; device = device->next, num++) {
    /* TODO: Add API to GstDevice for this */
    if (device_is_in_use (device->data))
      continue;
    name = gst_device_get_display_name (device->data);
    g_print ("%s [%u]\n", name, num);
    g_free (name);
  }
  g_print ("> ");
  device = NULL;

again:
  line = g_string_new ("");
  switch (g_io_channel_read_line_string (channel, line, NULL, &error)) {
    gint index;
    case G_IO_STATUS_NORMAL:
      index = g_ascii_digit_value (line->str[0]);
      if (index < 0 || index >= num) {
        g_printerr ("Invalid selection %c, try again\n> ", line->str[0]);
        g_string_free (line, TRUE);
        goto again;
      }
      if (index == 0) {
        /* device is NULL and the test device will be selected */
        g_print ("Selected test video source, continuing...\n");
        break;
      }
      device = g_list_nth (devices, index - 1);
      g_assert (device);
      name = gst_device_get_display_name (device->data);
      g_print ("Selected device %s, continuing...\n", name);
      g_free (name);
      break;
    case G_IO_STATUS_ERROR:
      g_printerr ("ERROR reading line: %s\n", error->message);
      break;
    case G_IO_STATUS_EOF:
      g_printerr ("Nothing entered? (EOF)\n");
      goto again;
    case G_IO_STATUS_AGAIN:
      g_printerr ("EAGAIN\n");
      goto again;
    default:
      g_assert_not_reached ();
  }

  g_string_free (line, TRUE);
  g_io_channel_unref (channel);

  return device ? device->data : NULL;
}

static GstDevice *
get_device (GList * devices, const gchar * device_path)
{
  GstDevice *device;

#ifdef __linux__
  device = ov_get_device_from_device_path (devices, device_path);
#else
  /* Just pick the first one */
  device = devices ? GST_DEVICE (devices->data) : NULL;
#endif

  if (device == NULL)
    g_printerr ("Selected video device was not found!\n");

  return device;
}

int
main (int   argc,
      char *argv[])
{
  OvLocalPeer *local;
  GOptionContext *optctx;
  GHashTable *missing;
  GError *error = NULL;
  GList *devices;

  guint exit_after = 0;
  gboolean auto_exit = FALSE;
  gboolean discover_peers = FALSE;
  guint16 iface_port = 0;
  gchar *iface_name = NULL;
  gchar *device_path = NULL;
  gchar **remotes = NULL;
  GOptionEntry entries[] = {
    {"exit-after", 0, 0, G_OPTION_ARG_INT, &exit_after, "Exit cleanly after N"
          " seconds (default: never exit)", "SECONDS"},
    {"auto-exit", 0, 0, G_OPTION_ARG_NONE, &auto_exit, "Automatically exit when"
          " the call is ended in passive mode (default: no)", NULL},
    {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
          " to listen on (default: any)", "NAME"},
    {"device", 'd', 0, G_OPTION_ARG_STRING, &device_path, "Path to the V4L2"
          " (camera) device; example: /dev/video0 (default: ask)", "PATH"},
    {"port", 'p', 0, G_OPTION_ARG_INT, &iface_port, "TCP port to listen on"
          " for incoming connections (default: " STR(OV_DEFAULT_COMM_PORT)
          ")", "PORT"},
    {"peer", 'c', 0, G_OPTION_ARG_STRING_ARRAY, &remotes, "Peers with an"
          " optional port to connect to. Specify multiple times to connect to"
          " several peers. Without this option, passive mode is used in which"
          " we wait for incoming connections.", "PEER:PORT"},
    {"discover", 0, 0, G_OPTION_ARG_NONE, &discover_peers, "Automatically"
          " discover and connect to peers (default: no)", NULL},
    {NULL}
  };

  gst_init (&argc, &argv);
  optctx = g_option_context_new (" - Peer-to-Peer low-latency high-bandwidth "
      "VoIP application");
  g_option_context_add_main_entries (optctx, entries, NULL);
  g_option_context_add_group (optctx, gst_init_get_option_group ());
  if (!g_option_context_parse (optctx, &argc, &argv, &error)) {
    g_printerr ("Error parsing options: %s\n", error->message);
    return -1;
  }
  g_option_context_free (optctx);

  /* Ensure that we have all the gstreamer plugins needed */
  missing = ov_get_missing_gstreamer_plugins (NULL);
  if (missing != NULL) {
    gpointer key, value;
    GHashTableIter iter;

    g_printerr ("The following GStreamer plugins could not be found:\n");
    g_hash_table_iter_init (&iter, missing);
    while (g_hash_table_iter_next (&iter, &key, &value))
      g_printerr ("\tplugin %s,\tpackage %s\n", (gchar*) key, (gchar*) value);
    g_hash_table_unref (missing);
    return -1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  if (iface_name == NULL)
    g_printerr ("Interface not specified, listening on all interfaces\n");
  else
    g_printerr ("Listening on interface %s\n", iface_name);

  local = ov_local_peer_new (iface_name, iface_port);
  if (local == NULL)
    goto out;

  g_print ("Probing devices...\n");
  ov_local_peer_start (local);
  devices = ov_local_peer_get_video_devices (local);
  g_print ("Probing finished\n");
  ov_local_peer_set_video_device (local, device_path ?
        get_device (devices, device_path) : get_device_choice (devices));
  g_list_free_full (devices, g_object_unref);

  /* Common for incoming and outgoing calls */
  g_signal_connect (local, "negotiate-finished",
      G_CALLBACK (on_negotiate_finished), NULL);

  if (remotes == NULL && !discover_peers) {
      g_print ("No remotes specified; listening for incoming connections\n");
      g_signal_connect (local, "negotiate-incoming",
          G_CALLBACK (on_negotiate_incoming), NULL);
      /* When an incoming negotation is aborted, we just carry on like nothing
       * happened */
      g_signal_connect (local, "negotiate-aborted",
          G_CALLBACK (on_incoming_negotiate_aborted), NULL);
      goto remotes_done;
  }

  /* When an outgoing negotation is aborted, we exit */
  g_signal_connect (local, "negotiate-aborted",
      G_CALLBACK (on_outgoing_negotiate_aborted), NULL);

  if (remotes == NULL) {
    FindRemotesData *data;
    GError *error = NULL;

    data = g_new0 (FindRemotesData, 1);
    data->local = local;

    g_print ("Discovering remote peers using multicast discovery...");

    g_signal_connect (local, "peer-discovered", G_CALLBACK (found_remote_cb),
        data);
    if (!ov_local_peer_discovery_start (local, 0, &error)) {
      g_print (" unable to search: %s. Exiting", error->message);
      g_error_free (error);
      g_free (data);
      goto out;
    }
    /* In a GUI, this would update a list of peers from which the user would
     * select a list of call them all. Since this program is not interactive,
     * we add a callback that waits for 2 seconds and dials whatever remotes
     * are found or exits if none are found. */
    g_timeout_add_seconds (5, aggregate_and_dial_remotes, data);
    goto remotes_done;
  }

  dial_remotes (local, remotes);

remotes_done:
  /* If in passive mode, auto exit only when requested */
  if (remotes != NULL || discover_peers || auto_exit)
    g_signal_connect (local, "call-all-remotes-gone",
        G_CALLBACK (on_call_ended_exit), NULL);
  else
    g_signal_connect (local, "call-all-remotes-gone",
        G_CALLBACK (on_call_ended_continue), NULL);
  g_unix_signal_add (SIGINT, (GSourceFunc) on_app_exit, local);
  if (exit_after > 0)
    g_timeout_add_seconds (exit_after, (GSourceFunc) on_app_exit, local);

  g_main_loop_run (loop);

out:
  g_clear_pointer (&loop, g_main_loop_unref);
  g_clear_object (&local);
  g_strfreev (remotes);
  g_free (device_path);
  g_free (iface_name);

  return 0;
}
