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

#include <glib-unix.h>

static GMainLoop *loop = NULL;

static gboolean
kill_remote_peer (OneVideoRemotePeer * remote)
{
  one_video_remote_peer_remove (remote);
  g_main_loop_quit (loop);

  return FALSE;
}

static gboolean
on_app_exit (OneVideoLocalPeer * local)
{
  one_video_local_peer_stop (local);
  g_main_loop_quit (loop);

  /* Remove the source so it's not called again */
  return FALSE;
}

int
main (int   argc,
      char *argv[])
{
  OneVideoLocalPeer *local;
  OneVideoRemotePeer *remote;
  GOptionContext *optctx;
  GInetAddress *inet_addr = NULL;
  GSocketAddress *listen_addr;
  GError *error = NULL;
  guint index = 0;

  guint exit_after = 0;
  guint iface_port = ONEVIDEO_DEFAULT_COMM_PORT;
  gchar *iface_name = NULL;
  gchar *v4l2_device_path = NULL;
  gchar **remotes = NULL;
  GOptionEntry entries[] = {
    {"exit-after", 0, 0, G_OPTION_ARG_INT, &exit_after, "Exit cleanly after N"
          " seconds (default: never exit)", "SECONDS"},
    {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
          " to listen on (default: any)", "NAME"},
    {"port", 'p', 0, G_OPTION_ARG_INT, &iface_port, "TCP port to listen on"
          " for incoming connections (default: " STR(ONEVIDEO_DEFAULT_COMM_PORT)
          ")", "PORT"},
    {"peer", 'c', 0, G_OPTION_ARG_STRING_ARRAY, &remotes, "Peers to connect to"
          "; specify multiple times to connect to several peers", "PEER"},
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

  if (remotes == NULL) {
    g_printerr ("No remotes specified; running in loopback mode\n");
    remotes = g_malloc0_n (sizeof (gchar*), 2);
    remotes[0] = g_strdup ("127.0.0.1");
  }

  loop = g_main_loop_new (NULL, FALSE);

  if (iface_name != NULL)
    inet_addr = one_video_get_inet_addr_for_iface (iface_name);
  else
    inet_addr = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);

  listen_addr = g_inet_socket_address_new (inet_addr, iface_port);
  g_object_unref (inet_addr);

  local = one_video_local_peer_new (G_INET_SOCKET_ADDRESS (listen_addr),
      v4l2_device_path);
  g_object_unref (listen_addr);

  for (index = 0; index < g_strv_length (remotes); index++) {
    remote = one_video_remote_peer_new (local, remotes[index]);
    if (!one_video_local_peer_setup_remote (local, remote)) {
      GST_ERROR ("Unable to receive from remote peer %s", remote->addr_s);
      continue;
    }
    GST_DEBUG ("Created and setup remote peer %s", remote->addr_s);
  }

  one_video_local_peer_start (local);

  g_unix_signal_add (SIGINT, (GSourceFunc) on_app_exit, local);
  if (exit_after > 0)
    g_timeout_add_seconds (exit_after, (GSourceFunc) on_app_exit, local);

  GST_DEBUG ("Running");
  g_main_loop_run (loop);

  g_clear_pointer (&local, one_video_local_peer_free);
  g_clear_pointer (&loop, g_main_loop_unref);
  g_strfreev (remotes);

  return 0;
}
