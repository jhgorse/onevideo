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
#include "lib-priv.h"
#include "lib-setup.h"
#include "comms.h"
#include "utils.h"
#include "outgoing.h"

#include <string.h>

GST_DEBUG_CATEGORY (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

static void one_video_remote_peer_remove_unlocked   (OneVideoRemotePeer *remote);
static void one_video_remote_peer_free_unlocked     (OneVideoRemotePeer *remote);
static gboolean one_video_local_peer_begin_transmit (OneVideoLocalPeer *local);

#define on_remote_receive_error one_video_on_gst_bus_error

static void
one_video_local_peer_stop_transmit (OneVideoLocalPeer * local)
{
  g_assert (gst_element_set_state (local->transmit, GST_STATE_NULL)
      == GST_STATE_CHANGE_SUCCESS);
  GST_DEBUG ("Stopped transmitting");
}

static void
one_video_local_peer_stop_playback (OneVideoLocalPeer * local)
{
  g_mutex_lock (&local->priv->lock);
  g_ptr_array_foreach (local->priv->remote_peers,
      (GFunc) one_video_remote_peer_remove_unlocked, NULL);
  g_ptr_array_free (local->priv->remote_peers, TRUE);
  local->priv->remote_peers = g_ptr_array_new ();
  g_mutex_unlock (&local->priv->lock);

  g_assert (gst_element_set_state (local->playback, GST_STATE_NULL)
      == GST_STATE_CHANGE_SUCCESS);
  GST_DEBUG ("Stopped playback");
}

static void
one_video_local_peer_stop_comms (OneVideoLocalPeer * local)
{
  g_signal_handlers_disconnect_by_data (local->priv->tcp_server, local);
  g_socket_service_stop (local->priv->tcp_server);
}

OneVideoLocalPeer *
one_video_local_peer_new (GInetSocketAddress * listen_addr,
    gchar * v4l2_path)
{
  OneVideoLocalPeer *local;

  g_return_val_if_fail (listen_addr != NULL, NULL);

  if (onevideo_debug == NULL)
    GST_DEBUG_CATEGORY_INIT (onevideo_debug, "onevideo", 0,
        "OneVideo VoIP library");

  local = g_new0 (OneVideoLocalPeer, 1);
  local->addr = listen_addr;
  g_object_ref (local->addr);
  local->addr_s = one_video_inet_socket_address_to_string (local->addr);
  local->state = ONE_VIDEO_LOCAL_STATE_NULL;
  local->priv = g_new0 (OneVideoLocalPeerPriv, 1);
  /* Hard-coded for now, but this should be selected in _setup depending
   * on the negotiated video format to be sent */
  local->priv->v4l2_path = v4l2_path;
  /* Threaded socket service since we use blocking TCP network reads
   * TODO: Use threads equal to number of remote peers? To ensure that peers
   * never wait while communicating. */
  local->priv->tcp_server = g_threaded_socket_service_new (10);
  /* NOTE: GArray and GPtrArray are not thread-safe; we must lock accesses */
  local->priv->used_ports = g_array_sized_new (FALSE, TRUE, sizeof (guint), 4);
  local->priv->remote_peers = g_ptr_array_new ();
  g_mutex_init (&local->priv->lock);

  /*-- Initialize caps supported by us --*/
  /* We will only ever use 48KHz Opus */
  local->priv->supported_send_acaps = gst_caps_from_string (RTP_AUDIO_CAPS_STR);
  /* For now, we force everyone to send 720p JPEG.
   * TODO: Query devices and fetch available JPEG dimensions + H.264 formats */
  local->priv->supported_send_vcaps = gst_caps_from_string (RTP_VIDEO_CAPS_STR
      ", " VIDEO_CAPS_STR);
  /* We will only ever use 48KHz Opus */
  local->priv->supported_recv_acaps = gst_caps_from_string (RTP_AUDIO_CAPS_STR);
  /* For now, only support JPEG.
   * TODO: Add other supported formats here */
  local->priv->supported_recv_vcaps = gst_caps_from_string (RTP_VIDEO_CAPS_STR);

  /* Setup transmit pipeline */
  g_assert (one_video_local_peer_setup_transmit_pipeline (local));

  /* Setup components of the playback pipeline */
  g_assert (one_video_local_peer_setup_playback_pipeline (local));

  /* Setup negotiation/comms */
  g_assert (one_video_local_peer_setup_tcp_comms (local));

  local->state = ONE_VIDEO_LOCAL_STATE_INITIALISED;

  return local;
}

/* NOT a public symbol. Called internally after the call details are set. */
gboolean
one_video_local_peer_setup (OneVideoLocalPeer * local)
{
  if (local->state != ONE_VIDEO_LOCAL_STATE_NEGOTIATED) {
    GST_ERROR ("Cannot setup local peer before negotiation is done");
    return FALSE;
  }

  /* TODO TODO TODO: Set the send/recv caps here */

  local->state = ONE_VIDEO_LOCAL_STATE_SETUP;
  return TRUE;
}

void
one_video_local_peer_stop (OneVideoLocalPeer * local)
{
  if (local->state >= ONE_VIDEO_LOCAL_STATE_SETUP) {
    one_video_local_peer_stop_transmit (local);
    one_video_local_peer_stop_playback (local);
  }

  /*if (local->state >= ONE_VIDEO_LOCAL_STATE_PLAYING) {
    one_video_local_peer_end_call (local);
  }*/

  one_video_local_peer_stop_comms (local);
  local->state = ONE_VIDEO_LOCAL_STATE_NULL;
  g_clear_pointer (&local->priv->send_acaps, gst_caps_unref);
  g_clear_pointer (&local->priv->send_vcaps, gst_caps_unref);
}

void
one_video_local_peer_free (OneVideoLocalPeer * local)
{
  g_ptr_array_free (local->priv->remote_peers, TRUE);
  g_array_free (local->priv->used_ports, TRUE);
  g_mutex_clear (&local->priv->lock);

  g_object_unref (local->priv->tcp_server);

  gst_caps_unref (local->priv->supported_send_acaps);
  gst_caps_unref (local->priv->supported_send_vcaps);
  gst_caps_unref (local->priv->supported_recv_acaps);
  gst_caps_unref (local->priv->supported_recv_vcaps);

  g_object_unref (local->transmit);
  g_object_unref (local->playback);
  g_object_unref (local->addr);
  g_free (local->addr_s);

  g_free (local->priv);

  g_free (local);
}

static gint
compare_ints (const void * a, const void * b)
{
  return (*(const unsigned int*)a - *(const unsigned int*)b);
}

/* Called with the lock TAKEN */
static gboolean
set_free_recv_ports (OneVideoLocalPeer * local, guint (*recv_ports)[4])
{
  guint ii, start;

  /* Start from the port right after the configured TCP communication port */
  start = 1 + g_inet_socket_address_get_port (local->addr);

  g_array_sort (local->priv->used_ports, compare_ints);

  /* Ports are always in contiguous sets of 4, so if we find a hole in the
   * sorted list of used ports, it definitely has 4 ports in it */
  for (ii = 0; ii < local->priv->used_ports->len; ii++)
    if (g_array_index (local->priv->used_ports, guint, ii) == start)
      start++;
    else
      break;

  /* TODO: Check whether these ports are actually available on the system */

  (*recv_ports)[0] = start;
  (*recv_ports)[1] = start + 1;
  (*recv_ports)[2] = start + 2;
  (*recv_ports)[3] = start + 3;
  g_array_append_vals (local->priv->used_ports, recv_ports, 4);
  return TRUE;
}

/* This is NOT a public symbol */
OneVideoRemotePeer *
one_video_remote_peer_new_unlocked (OneVideoLocalPeer * local,
    const gchar * addr_s)
{
  gchar *name;
  GstBus *bus;
  OneVideoRemotePeer *remote;

  remote = g_new0 (OneVideoRemotePeer, 1);
  remote->state = ONE_VIDEO_REMOTE_STATE_NULL;
  remote->receive = gst_pipeline_new ("receive-%u");
  remote->local = local;
  remote->addr = one_video_inet_socket_address_from_string (addr_s);
  remote->addr_s = g_strdup (addr_s);

  remote->priv = g_new0 (OneVideoRemotePeerPriv, 1);
  name = g_strdup_printf ("audio-playback-bin-%s", remote->addr_s);
  remote->priv->aplayback = gst_bin_new (name);
  g_free (name);
  name = g_strdup_printf ("video-playback-bin-%s", remote->addr_s);
  remote->priv->vplayback = gst_bin_new (name);
  g_free (name);

  g_assert (set_free_recv_ports (local, &remote->priv->recv_ports));

  /* Use the system clock and explicitly reset the base/start times to ensure
   * that all the pipelines started by us have the same base/start times */
  gst_pipeline_use_clock (GST_PIPELINE (remote->receive),
      gst_system_clock_obtain());
  gst_element_set_base_time (remote->receive, 0);

  bus = gst_pipeline_get_bus (GST_PIPELINE (remote->receive));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_remote_receive_error), remote);
  g_object_unref (bus);

  remote->state = ONE_VIDEO_REMOTE_STATE_ALLOCATED;

  return remote;
}

OneVideoRemotePeer *
one_video_remote_peer_new (OneVideoLocalPeer * local, const gchar * addr_s)
{
  OneVideoRemotePeer *remote;

  /* We need a lock for set_free_recv_ports() which manipulates
   * local->priv->used_ports */
  g_mutex_lock (&local->priv->lock);
  remote = one_video_remote_peer_new_unlocked (local, addr_s);
  g_mutex_unlock (&local->priv->lock);

  return remote;
}

void
one_video_remote_peer_pause (OneVideoRemotePeer * remote)
{
  OneVideoLocalPeer *local = remote->local;

  g_assert (remote->state == ONE_VIDEO_REMOTE_STATE_PLAYING);

  /* Stop transmitting */
  g_signal_emit_by_name (local->priv->audpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[0]);
  g_signal_emit_by_name (local->priv->artcpudpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[1]);
  g_signal_emit_by_name (local->priv->vudpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[2]);
  g_signal_emit_by_name (local->priv->vrtcpudpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[3]);

  /* Pause receiving */
  g_assert (gst_element_set_state (remote->receive, GST_STATE_PAUSED)
      == GST_STATE_CHANGE_SUCCESS);

  if (remote->priv->audio_proxysrc != NULL) {
    GstPad *srcpad, *sinkpad;

    srcpad = gst_element_get_static_pad (remote->priv->aplayback, "audiopad");
    sinkpad = gst_pad_get_peer (srcpad);

    gst_pad_unlink (srcpad, sinkpad);
    gst_object_unref (srcpad);
    GST_DEBUG ("Unlinked audio pads of %s", remote->addr_s);

    gst_element_release_request_pad (local->priv->audiomixer, sinkpad);
    gst_object_unref (sinkpad);
    GST_DEBUG ("Released audiomixer sinkpad of %s", remote->addr_s);

    g_assert (gst_element_set_state (remote->priv->aplayback, GST_STATE_PAUSED)
        == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Paused audio of %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    g_assert (gst_element_set_state (remote->priv->vplayback, GST_STATE_PAUSED)
        == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Paused video of %s", remote->addr_s);
  }

  remote->state = ONE_VIDEO_REMOTE_STATE_PAUSED;
  GST_DEBUG ("Fully paused remote peer %s", remote->addr_s);
}

void
one_video_remote_peer_resume (OneVideoRemotePeer * remote)
{
  OneVideoLocalPeer *local = remote->local;

  g_assert (remote->state == ONE_VIDEO_REMOTE_STATE_PAUSED);

  /* Start transmitting */
  g_signal_emit_by_name (local->priv->audpsink, "add", remote->addr_s,
      remote->priv->recv_ports[0]);
  g_signal_emit_by_name (local->priv->artcpudpsink, "add", remote->addr_s,
      remote->priv->recv_ports[1]);
  g_signal_emit_by_name (local->priv->vudpsink, "add", remote->addr_s,
      remote->priv->recv_ports[2]);
  g_signal_emit_by_name (local->priv->vrtcpudpsink, "add", remote->addr_s,
      remote->priv->recv_ports[3]);

  if (remote->priv->audio_proxysrc != NULL) {
    GstPadLinkReturn ret;
    GstPad *srcpad, *sinkpad;

    sinkpad = gst_element_get_request_pad (local->priv->audiomixer, "sink_%u");
    srcpad = gst_element_get_static_pad (remote->priv->aplayback, "audiopad");

    ret = gst_pad_link (srcpad, sinkpad);
    g_assert (ret == GST_PAD_LINK_OK);
    gst_object_unref (sinkpad);
    gst_object_unref (srcpad);

    g_assert (gst_element_set_state (remote->priv->aplayback, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Resumed audio of %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    g_assert (gst_element_set_state (remote->priv->vplayback, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Resumed video of %s", remote->addr_s);
  }

  /* Resume receiving */
  g_assert (gst_element_set_state (remote->receive, GST_STATE_PLAYING)
      == GST_STATE_CHANGE_SUCCESS);
  remote->state = ONE_VIDEO_REMOTE_STATE_PLAYING;
  GST_DEBUG ("Fully resumed remote peer %s", remote->addr_s);
}

/* Does not do any operations that involve taking the OneVideoLocalPeer lock.
 * See: one_video_remote_peer_remove() */
static void
one_video_remote_peer_remove_unlocked (OneVideoRemotePeer * remote)
{
  gchar *tmp;
  OneVideoLocalPeer *local = remote->local;

  /* Stop transmitting */
  g_signal_emit_by_name (local->priv->audpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[0]);
  g_signal_emit_by_name (local->priv->artcpudpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[1]);
  g_signal_emit_by_name (local->priv->vudpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[2]);
  g_signal_emit_by_name (local->priv->vrtcpudpsink, "remove", remote->addr_s,
      remote->priv->recv_ports[3]);

  /* Release all requested pads and relevant playback bins */
  if (remote->priv->audio_proxysrc != NULL) {
    GstPad *srcpad, *sinkpad;

    srcpad = gst_element_get_static_pad (remote->priv->aplayback, "audiopad");
    sinkpad = gst_pad_get_peer (srcpad);

    if (sinkpad) {
      gst_pad_unlink (srcpad, sinkpad);
      GST_DEBUG ("Unlinked audio pad of %s", remote->addr_s);

      gst_element_release_request_pad (local->priv->audiomixer,
          sinkpad);
      gst_object_unref (sinkpad);
      GST_DEBUG ("Released audiomixer sinkpad of %s", remote->addr_s);
    } else {
      GST_DEBUG ("Remote %s wasn't playing", remote->addr_s);
    }
    gst_object_unref (srcpad);

    g_assert (gst_element_set_state (remote->priv->aplayback, GST_STATE_NULL)
        == GST_STATE_CHANGE_SUCCESS);
    g_assert (gst_bin_remove (GST_BIN (local->playback),
          remote->priv->aplayback));
    GST_DEBUG ("Released audio playback bin of remote %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    g_assert (gst_element_set_state (remote->priv->vplayback, GST_STATE_NULL)
        == GST_STATE_CHANGE_SUCCESS);
    g_assert (gst_bin_remove (GST_BIN (local->playback),
          remote->priv->vplayback));
    GST_DEBUG ("Released video playback bin of remote %s", remote->addr_s);
  }

  /* Stop receiving */
  g_assert (gst_element_set_state (remote->receive, GST_STATE_NULL)
      == GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (remote->receive);
  remote->receive = NULL;
  remote->state = ONE_VIDEO_REMOTE_STATE_NULL;

  tmp = g_strdup (remote->addr_s);
  one_video_remote_peer_free_unlocked (remote);
  GST_DEBUG ("Freed everything for remote peer %s", tmp);
  g_free (tmp);
}

void
one_video_remote_peer_remove (OneVideoRemotePeer * remote)
{
  /* Remove from the peers list first so nothing else tries to use it */
  g_mutex_lock (&remote->local->priv->lock);
  g_ptr_array_remove (remote->local->priv->remote_peers, remote);
  g_mutex_unlock (&remote->local->priv->lock);

  one_video_remote_peer_remove_unlocked (remote);
}

static void
one_video_remote_peer_free_unlocked (OneVideoRemotePeer * remote)
{
  guint ii;
  OneVideoLocalPeer *local = remote->local;

  for (ii = 0; ii < local->priv->used_ports->len; ii++)
    /* Port numbers are unique, sorted, and contiguous. So if we find the first
     * port, we've found all of them. */
    if (g_array_index (local->priv->used_ports, guint, ii) ==
        remote->priv->recv_ports[0])
      g_array_remove_range (local->priv->used_ports, ii, 4);

  gst_caps_unref (remote->priv->recv_acaps);
  gst_caps_unref (remote->priv->recv_vcaps);
  g_object_unref (remote->addr);
  g_free (remote->addr_s);
  g_free (remote->priv);
  g_free (remote);
}

void
one_video_remote_peer_free (OneVideoRemotePeer * remote)
{
  OneVideoLocalPeer *local = remote->local;
  g_mutex_lock (&local->priv->lock);
  one_video_remote_peer_free_unlocked (remote);
  g_mutex_unlock (&local->priv->lock);
}

/*
 * Searches for and returns remote peers
 */
GPtrArray *
one_video_local_peer_find_remotes (OneVideoLocalPeer * local)
{
  GPtrArray *remotes;

  remotes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  /* FIXME: implement this */

  return remotes;
}

OneVideoRemotePeer *
one_video_local_peer_get_remote_by_addr_s (OneVideoLocalPeer * local,
    const gchar * addr_s)
{
  guint ii;
  OneVideoRemotePeer *remote;

  g_mutex_lock (&local->priv->lock);
  for (ii = 0; ii < local->priv->remote_peers->len; ii++) {
    remote = g_ptr_array_index (local->priv->remote_peers, ii);
    if (g_strcmp0 (addr_s, remote->addr_s))
      break;
  }
  g_mutex_unlock (&local->priv->lock);

  return remote;
}

static void
append_clients (gpointer data, gpointer user_data)
{
  OneVideoRemotePeer *remote = data;
  GString **clients = user_data;
  gchar *addr_s;

  addr_s = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));

  g_string_append_printf (clients[0], "%s:%u,", addr_s,
      remote->priv->send_ports[0]);
  g_string_append_printf (clients[1], "%s:%u,", addr_s,
      remote->priv->send_ports[1]);
  g_string_append_printf (clients[2], "%s:%u,", addr_s,
      remote->priv->send_ports[2]);
  g_string_append_printf (clients[3], "%s:%u,", addr_s,
      remote->priv->send_ports[3]);

  g_free (addr_s);
}

/* Called with the lock TAKEN */
static gboolean
one_video_local_peer_begin_transmit (OneVideoLocalPeer * local)
{
  GString **clients;
  GstStateChangeReturn ret;

  /* {audio RTP, audio RTCP, video RTP, video RTCP} */
  clients = g_malloc0_n (sizeof (GString*), 4);
  clients[0] = g_string_new ("");
  clients[1] = g_string_new ("");
  clients[2] = g_string_new ("");
  clients[3] = g_string_new ("");
  
  g_ptr_array_foreach (local->priv->remote_peers, append_clients, clients);
  g_object_set (local->priv->audpsink, "clients", clients[0]->str, NULL);
  g_object_set (local->priv->artcpudpsink, "clients", clients[1]->str, NULL);
  g_object_set (local->priv->vudpsink, "clients", clients[2]->str, NULL);
  g_object_set (local->priv->vrtcpudpsink, "clients", clients[3]->str, NULL);

  ret = gst_element_set_state (local->transmit, GST_STATE_PLAYING);
  GST_DEBUG ("Transmitting to remote peers. Audio: %s Video: %s",
      clients[0]->str, clients[2]->str);

  g_string_free (clients[0], TRUE);
  g_string_free (clients[1], TRUE);
  g_string_free (clients[2], TRUE);
  g_string_free (clients[3], TRUE);
  g_free (clients);

  return ret != GST_STATE_CHANGE_FAILURE;
}

/* This is NOT a public symbol */
void
one_video_local_peer_add_remote_unlocked (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  g_ptr_array_add (local->priv->remote_peers, remote);
}

void
one_video_local_peer_add_remote (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  /* Add to our list of remote peers */
  g_mutex_lock (&local->priv->lock);
  one_video_local_peer_add_remote_unlocked (local, remote);
  g_mutex_unlock (&local->priv->lock);
}

static gboolean
one_video_local_peer_setup_remote (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  one_video_local_peer_setup_remote_receive (local, remote);
  one_video_local_peer_setup_remote_playback (local, remote);
  
  remote->state = ONE_VIDEO_REMOTE_STATE_SETUP;

  return TRUE;
}

/* Will send each remote peer the list of all other remote peers, and each
 * remote peer replies with the recv/send caps it supports. Once all the peers
 * have replied, we'll decide caps for everyone and send them to everyone. All
 * this will happen asynchronously. The caller should just call 
 * one_video_local_peer_start() when it wants to start the call, and it will 
 * start when everyone is ready. */
gboolean
one_video_local_peer_negotiate_async (OneVideoLocalPeer * local,
    GCancellable * cancellable, GAsyncReadyCallback callback,
    gpointer callback_data)
{
  GTask *task;

  /* Unlocked in _finish */
  g_mutex_lock (&local->priv->lock);
  if (local->state != ONE_VIDEO_LOCAL_STATE_INITIALISED) {
    GST_ERROR ("Our state is %u instead of INITIALISED", local->state);
    return FALSE;
  }

  local->state = ONE_VIDEO_LOCAL_STATE_NEGOTIATING;

  task = g_task_new (NULL, cancellable, callback, callback_data);
  g_task_set_task_data (task, local, NULL);
  g_task_set_return_on_cancel (task, TRUE);
  g_task_run_in_thread (task,
      (GTaskThreadFunc) one_video_local_peer_negotiate_thread);
  g_object_unref (task);

  return TRUE;
}

gboolean
one_video_local_peer_negotiate_finish (OneVideoLocalPeer * local,
    GAsyncResult * result, GError ** error)
{
  g_mutex_unlock (&local->priv->lock);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* NOT a public symbol */
gboolean
one_video_local_peer_start_unlocked (OneVideoLocalPeer * local)
{
  guint index;
  GstStateChangeReturn ret;
  OneVideoRemotePeer *remote;

  g_assert (one_video_local_peer_setup (local));

  if (local->state != ONE_VIDEO_LOCAL_STATE_SETUP) {
    GST_ERROR ("Negotiation hasn't been done yet!");
    return FALSE;
  }

  g_assert (one_video_local_peer_begin_transmit (local));

  for (index = 0; index < local->priv->remote_peers->len; index++) {
    remote = g_ptr_array_index (local->priv->remote_peers, index);
    
    /* Call details have all been set, so we can do the setup */
    g_assert (one_video_local_peer_setup_remote (local, remote));

    /* Start PLAYING the pipelines */
    ret = gst_element_set_state (remote->receive, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      goto recv_fail;
    }
    GST_DEBUG ("Ready to receive data from %s on ports %u, %u, %u, %u",
        remote->addr_s, remote->priv->recv_ports[0],
        remote->priv->recv_ports[1], remote->priv->recv_ports[2],
        remote->priv->recv_ports[3]);
    remote->state = ONE_VIDEO_REMOTE_STATE_PLAYING;
  }

  ret = gst_element_set_state (local->playback, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto play_fail;

  GST_DEBUG ("Ready to playback data from all remotes");
  local->state = ONE_VIDEO_LOCAL_STATE_PLAYING;
  return TRUE;

  play_fail: {
    GST_ERROR ("Unable to set local playback pipeline to PLAYING!");
    return FALSE;
  }

  recv_fail: {
    GST_ERROR ("Unable to set %s receive pipeline to PLAYING!", remote->addr_s);
    return FALSE;
  }
}

gboolean
one_video_local_peer_start (OneVideoLocalPeer * local)
{
  gboolean ret;

  g_mutex_lock (&local->priv->lock);
  ret = one_video_local_peer_start_unlocked (local);
  g_mutex_unlock (&local->priv->lock);
  return ret;
}
