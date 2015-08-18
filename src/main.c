/*  vim: set sts=2 sw=2 et :
 *
 *  Copyright (C) 2015 Centricular Ltd
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

int
main (int   argc,
      char *argv[])
{
  guint index = 0;
  GPtrArray *remotes;
  OneVideoLocalPeer *local;

  gst_init (&argc, &argv);
  loop = g_main_loop_new (NULL, FALSE);

  local = one_video_local_peer_new (NULL);

  //g_timeout_add_seconds (5, (GSourceFunc) on_app_exit, local);
  g_unix_signal_add (SIGINT, (GSourceFunc) on_app_exit, local);

  remotes = one_video_local_peer_find_remotes (local);
  if (remotes->len < 1) {
    GST_ERROR ("No remote peers found, exiting");
    return EXIT_FAILURE;
  }

  for (index = 0; index < remotes->len; index++) {
    OneVideoRemotePeer *remote;
    remote = one_video_remote_peer_new (local,
        g_ptr_array_index (remotes, index));
    if (!one_video_local_peer_setup_remote (local, remote)) {
      gchar *address = g_inet_address_to_string (remote->addr);
      GST_ERROR ("Unable to receive from remote peer %s", address);
      g_free (address);
      continue;
    }
  }

  one_video_local_peer_start (local);
  GST_DEBUG ("Playing");

  g_main_loop_run (loop);

  g_clear_pointer (&local, one_video_local_peer_free);
  g_clear_pointer (&loop, g_main_loop_unref);
  g_ptr_array_free (remotes, TRUE);

  return EXIT_SUCCESS;
}
