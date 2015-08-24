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
check_do_app_quit (gpointer data)
{
  OneVideoLocalPeer *local = data;

  if (local->state != ONE_VIDEO_STATE_NULL)
    return TRUE;
  GST_DEBUG ("All done, exiting");
  g_main_loop_quit (loop);
  return FALSE;
}


static gboolean
on_app_exit (OneVideoLocalPeer * local)
{
  one_video_local_peer_stop (local);
  /* FIXME: Add a way for the app to get notified when the local source has
   * finished cleanup */
  g_idle_add (check_do_app_quit, local);

  /* Remove the source so it's not called again */
  return FALSE;
}

static gboolean
_parse_test_mode_ports (gchar * test_mode_ports, guint * audio_port,
    guint * video_port)
{
  gchar **split;
  gboolean ret = FALSE;

  if (!test_mode_ports) {
    *audio_port = *video_port = 0;
    return TRUE;
  }
  
  split = g_strsplit (test_mode_ports, ",", -1);
  if (g_strv_length (split) != 2) {
    g_printerr ("Invalid format for test_mode_ports: %s\n", test_mode_ports);
    goto out;
  }

  *audio_port = g_ascii_strtoull (split[0], NULL, 10);
  if (*audio_port == 0) {
    g_printerr ("Invalid format for audio port: %s\n", split[0]);
    goto out;
  }

  *video_port = g_ascii_strtoull (split[1], NULL, 10);
  if (*video_port == 0) {
    g_printerr ("Invalid format for video port: %s\n", split[1]);
    goto out;
  }

  ret = TRUE;
out:
  g_strfreev (split);
  return ret;
}

int
main (int   argc,
      char *argv[])
{
  OneVideoLocalPeer *local;
  GOptionContext *optctx;
  GInetAddress *listen_addr = NULL;
  GError *error = NULL;
  guint audio_port, video_port, index = 0;

  guint exit_after = 0;
  gchar *iface_name = NULL;
  gchar *test_mode_ports = NULL;
  gchar *remotes[] = {"127.0.0.1", NULL};
  GOptionEntry entries[] = {
    {"test-mode", 0, 0, G_OPTION_ARG_STRING, &test_mode_ports, "Run in test"
          " mode on localhost with the given ports", "AUDIO_PORT,VIDEO_PORT"},
    {"exit-after", 0, 0, G_OPTION_ARG_INT, &exit_after, "Exit cleanly after N"
          " seconds (default: never exit)", "SECONDS"},
    {"interface", 'i', 0, G_OPTION_ARG_STRING, &iface_name, "Network interface"
          " to listen on (default: any)", "NAME"},
    {"peer", 'p', 0, G_OPTION_ARG_STRING_ARRAY, &remotes, "Peer(s) to connect to"
          ". Specify multiple times to connect to multiple peers.", "PEER"},
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

  if (!_parse_test_mode_ports (test_mode_ports, &audio_port, &video_port))
    return -1;

  loop = g_main_loop_new (NULL, FALSE);

  if (iface_name != NULL)
    listen_addr = one_video_get_ip_for_interface (iface_name);
  local = one_video_local_peer_new (listen_addr);

  for (index = 0; index < g_strv_length (remotes); index++) {
    OneVideoRemotePeer *remote;
    remote = one_video_remote_peer_new (local, remotes[index], audio_port,
        video_port);
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

  return 0;
}
