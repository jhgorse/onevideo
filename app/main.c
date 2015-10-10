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

#include "lib.h"
#include "utils.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#include <glib-unix.h>

static GMainLoop *loop = NULL;

static gboolean
kill_remote_peer (OneVideoRemotePeer * remote)
{
  one_video_remote_peer_remove (remote);
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static gboolean
on_local_peer_stop (OneVideoLocalPeer * local)
{
  if (local->state & ONE_VIDEO_LOCAL_STATE_STOPPED) {
      g_print ("Local peer stopped, exiting...\n");
      goto quit;
  }

  if (local->state & ONE_VIDEO_LOCAL_STATE_FAILED &&
      local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATOR) {
      g_print ("Local negotiator peer failed, exiting...\n");
      one_video_local_peer_stop (local);
      goto quit;
  }

  return G_SOURCE_CONTINUE;
quit:
  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static gboolean
on_app_exit (OneVideoLocalPeer * local)
{
  if (local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATING)
    one_video_local_peer_negotiate_stop (local);
  one_video_local_peer_stop (local);
  g_main_loop_quit (loop);

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
dial_remotes (OneVideoLocalPeer * local, gchar ** remotes)
{
  guint index;

  for (index = 0; index < g_strv_length (remotes); index++) {
    OneVideoRemotePeer *remote;

    remote = one_video_remote_peer_new (local, remotes[index]);
    one_video_local_peer_add_remote (local, remote);

    g_print ("Created and added remote peer %s\n", remote->addr_s);
  }

  one_video_local_peer_negotiate_async (local, NULL, on_negotiate_done, NULL);
}

int
main (int   argc,
      char *argv[])
{
  OneVideoLocalPeer *local;
  GOptionContext *optctx;
  GInetAddress *inet_addr = NULL;
  GSocketAddress *listen_addr;
  GError *error = NULL;

  guint exit_after = 0;
  gboolean auto_exit = FALSE;
  guint iface_port = ONE_VIDEO_DEFAULT_COMM_PORT;
  gchar *iface_name = NULL;
  gchar *v4l2_device_path = NULL;
  gchar **remotes = NULL;
  GOptionEntry entries[] = {
    {"exit-after", 0, 0, G_OPTION_ARG_INT, &exit_after, "Exit cleanly after N"
          " seconds (default: never exit)", "SECONDS"},
    {"auto-exit", 0, 0, G_OPTION_ARG_NONE, &auto_exit, "Automatically exit when"
          " the call is ended in passive mode (default: no)", NULL},
    {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
          " to listen on (default: any)", "NAME"},
    {"port", 'p', 0, G_OPTION_ARG_INT, &iface_port, "TCP port to listen on"
          " for incoming connections (default: " STR(ONE_VIDEO_DEFAULT_COMM_PORT)
          ")", "PORT"},
    {"peer", 'c', 0, G_OPTION_ARG_STRING_ARRAY, &remotes, "Peers with an"
          " optional port to connect to. Specify multiple times to connect to"
          " several peers. Without this option, passive mode is used in which"
          " we wait for incoming connections.", "PEER:PORT"},
    {"device", 'd', 0, G_OPTION_ARG_STRING, &v4l2_device_path, "Path to the"
          " V4L2 (camera) device (Ex: /dev/video0)", "PATH"},
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

  loop = g_main_loop_new (NULL, FALSE);

  if (iface_name != NULL) {
    inet_addr = one_video_get_inet_addr_for_iface (iface_name);
  } else {
    inet_addr = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    /* FIXME: Listen on the default route instead */
    g_printerr ("Interface not specified, listening on localhost\n");
  }

  listen_addr = g_inet_socket_address_new (inet_addr, iface_port);
  g_object_unref (inet_addr);

  local = one_video_local_peer_new (G_INET_SOCKET_ADDRESS (listen_addr),
      v4l2_device_path);
  g_object_unref (listen_addr);

  if (remotes == NULL) {
    g_print ("No remotes specified; listening for incoming connections\n");
    /* Stop the mainloop and exit when the call ends */
  } else {
    g_print ("Dialling remotes...\n");
    dial_remotes (local, remotes);
    g_print ("Waiting for remotes to reply...\n");
  }

  /* If in passive mode, auto exit only when requested */
  if (remotes != NULL || auto_exit)
    g_idle_add ((GSourceFunc) on_local_peer_stop, local);
  g_unix_signal_add (SIGINT, (GSourceFunc) on_app_exit, local);
  if (exit_after > 0)
    g_timeout_add_seconds (exit_after, (GSourceFunc) on_app_exit, local);

  g_main_loop_run (loop);

  g_clear_pointer (&local, one_video_local_peer_free);
  g_clear_pointer (&loop, g_main_loop_unref);
  g_strfreev (remotes);
  g_free (iface_name);
  g_free (v4l2_device_path);

  return 0;
}
