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
#include "discovery.h"

#include <string.h>

GST_DEBUG_CATEGORY (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

static gboolean one_video_local_peer_begin_transmit (OneVideoLocalPeer *local);

#define on_remote_receive_error one_video_on_gst_bus_error

static void
one_video_local_peer_stop_transmit (OneVideoLocalPeer * local)
{
  GstStateChangeReturn ret;
  ret = gst_element_set_state (local->transmit, GST_STATE_NULL);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  GST_DEBUG ("Stopped transmitting");
}

static void
one_video_local_peer_stop_playback (OneVideoLocalPeer * local)
{
  GstStateChangeReturn ret;
  ret = gst_element_set_state (local->playback, GST_STATE_NULL);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  GST_DEBUG ("Stopped playback");
}

static void
one_video_local_peer_stop_comms (OneVideoLocalPeer * local)
{
  g_signal_handlers_disconnect_by_data (local->priv->tcp_server, local);
  g_socket_service_stop (local->priv->tcp_server);
}

OneVideoLocalPeer *
one_video_local_peer_new (GSocketAddress * listen_addr)
{
  gchar *guid;
  GstCaps *vcaps;
  OneVideoLocalPeer *local;
  guint16 tcp_port;
  gboolean ret;

  if (onevideo_debug == NULL)
    GST_DEBUG_CATEGORY_INIT (onevideo_debug, "onevideo", 0,
        "OneVideo VoIP library");

  if (listen_addr == NULL) {
    GInetAddress *addr;
    //addr = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
    addr = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    listen_addr = g_inet_socket_address_new (addr, ONE_VIDEO_DEFAULT_COMM_PORT);
    g_object_unref (addr);
  } else {
    /* Take a ref because we will be using this directly */
    g_object_ref (listen_addr);
  }

  local = g_new0 (OneVideoLocalPeer, 1);
  local->addr = G_INET_SOCKET_ADDRESS (listen_addr);
  local->addr_s = one_video_inet_socket_address_to_string (local->addr);
  guid = g_dbus_generate_guid (); /* Generate a UUIDesque string */
  local->id = g_strdup_printf ("%s:%u-%s", g_get_host_name (),
      g_inet_socket_address_get_port (local->addr), guid);
  g_free (guid);
  local->state = ONE_VIDEO_LOCAL_STATE_NULL;
  local->priv = g_new0 (OneVideoLocalPeerPriv, 1);

  /* Allocate ports for recv RTCP RRs from all remotes */
  tcp_port = g_inet_socket_address_get_port (local->addr);
  local->priv->recv_rtcp_ports[0] = tcp_port + 1;
  local->priv->recv_rtcp_ports[1] = tcp_port + 2;

  /* Initialize the V4L2 device monitor */
  /* We only want native formats: JPEG, (and later) YUY2 and H.264 */
  vcaps = gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);
  /*gst_caps_append (vcaps,
      gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "YUY2"));
  gst_caps_append (vcaps, gst_caps_new_empty_simple (VIDEO_FORMAT_H264));*/
  local->priv->dm = gst_device_monitor_new ();
  gst_device_monitor_add_filter (local->priv->dm, "Video/Source", vcaps);
  gst_caps_unref (vcaps);
  GST_DEBUG ("Starting device monitor");
  /* Start probing devices asynchronously. We don't listen to the bus messages
   * for this right now, and just get the list of all devices later. */
  gst_device_monitor_start (local->priv->dm);

  /* NOTE: GArray and GPtrArray are not thread-safe; we must lock accesses */
  local->priv->used_ports = g_array_sized_new (FALSE, TRUE, sizeof (guint), 4);
  local->priv->remote_peers = g_ptr_array_new ();
  g_rec_mutex_init (&local->priv->lock);

  /*-- Initialize (non-RTP) caps supported by us --*/
  /* NOTE: Caps negotiated/exchanged between peers are always non-RTP caps */
  /* We will only ever use 48KHz Opus */
  local->priv->supported_send_acaps =
    gst_caps_from_string (AUDIO_FORMAT_OPUS CAPS_SEP AUDIO_CAPS_STR);
  /* supported_send_vcaps is set in set_video_device() */

  /* We will only ever use 48KHz Opus */
  local->priv->supported_recv_acaps =
    gst_caps_new_empty_simple (AUDIO_FORMAT_OPUS);
  /* For now, only support JPEG.
   * TODO: Add other supported formats here */
  local->priv->supported_recv_vcaps =
    gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);

  /*-- Setup various pipelines and resources --*/

  /* Transmit pipeline is setup in set_video_device() */

  /* Setup components of the playback pipeline */
  ret = one_video_local_peer_setup_playback_pipeline (local);
  g_assert (ret);

  /* Setup negotiation/comms */
  ret = one_video_local_peer_setup_tcp_comms (local);
  g_assert (ret);

  local->state = ONE_VIDEO_LOCAL_STATE_INITIALISED;

  return local;
}

void
one_video_local_peer_free (OneVideoLocalPeer * local)
{
  GST_DEBUG ("Stopping TCP communication");
  one_video_local_peer_stop_comms (local);
  GST_DEBUG ("Freeing local peer");
  g_ptr_array_free (local->priv->remote_peers, TRUE);
  g_array_free (local->priv->used_ports, TRUE);
  g_rec_mutex_clear (&local->priv->lock);

  gst_device_monitor_stop (local->priv->dm);
  g_object_unref (local->priv->dm);

  g_source_unref (local->priv->mc_socket_source);
  g_object_unref (local->priv->mc_socket);

  g_object_unref (local->priv->tcp_server);

  gst_caps_unref (local->priv->supported_send_acaps);
  gst_caps_unref (local->priv->supported_send_vcaps);
  gst_caps_unref (local->priv->supported_recv_acaps);
  gst_caps_unref (local->priv->supported_recv_vcaps);

  g_object_unref (local->transmit);
  g_object_unref (local->playback);
  g_object_unref (local->addr);
  g_free (local->addr_s);
  g_free (local->id);

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

  /* Start from the port right after the video RTCP recv port */
  start = 1 + local->priv->recv_rtcp_ports[1];

  g_array_sort (local->priv->used_ports, compare_ints);

  /* Recv ports are always in contiguous sets of 4, so if we
   * find a hole in the sorted list of used ports, it has 4 unused ports */
  for (ii = 0; ii < local->priv->used_ports->len; ii++)
    if (g_array_index (local->priv->used_ports, guint, ii) == start)
      start++;
    else
      break;

  /* TODO: Check whether these ports are actually available on the system */

  for (ii = 0; ii < 4; ii++)
    (*recv_ports)[ii] = start + ii;
  g_array_append_vals (local->priv->used_ports, recv_ports, 4);
  return TRUE;
}

OneVideoRemotePeer *
one_video_remote_peer_new (OneVideoLocalPeer * local,
    GInetSocketAddress * addr)
{
  gchar *name;
  GstBus *bus;
  gboolean ret;
  OneVideoRemotePeer *remote;

  remote = g_new0 (OneVideoRemotePeer, 1);
  remote->state = ONE_VIDEO_REMOTE_STATE_NULL;
  remote->receive = gst_pipeline_new ("receive-%u");
  remote->local = local;
  remote->addr = g_object_ref (addr);
  remote->addr_s = one_video_inet_socket_address_to_string (remote->addr);

  remote->priv = g_new0 (OneVideoRemotePeerPriv, 1);
  name = g_strdup_printf ("audio-playback-bin-%s", remote->addr_s);
  remote->priv->aplayback = gst_bin_new (name);
  g_free (name);
  name = g_strdup_printf ("video-playback-bin-%s", remote->addr_s);
  remote->priv->vplayback = gst_bin_new (name);
  g_free (name);

  /* We need a lock for set_free_recv_ports() which manipulates
   * local->priv->used_ports */
  g_rec_mutex_lock (&local->priv->lock);
  ret = set_free_recv_ports (local, &remote->priv->recv_ports);
  g_assert (ret);
  g_rec_mutex_unlock (&local->priv->lock);

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
one_video_remote_peer_new_from_string (OneVideoLocalPeer * local,
    const gchar * addr_s)
{
  GInetSocketAddress *addr;
  OneVideoRemotePeer *remote;
  
  addr = one_video_inet_socket_address_from_string (addr_s);
  remote = one_video_remote_peer_new (local, addr);
  g_object_unref (addr);

  return remote;
}

void
one_video_remote_peer_pause (OneVideoRemotePeer * remote)
{
  GstStateChangeReturn ret;
  gchar *addr_only;
  OneVideoLocalPeer *local = remote->local;

  g_assert (remote->state == ONE_VIDEO_REMOTE_STATE_PLAYING);

  /* Stop transmitting */
  addr_only = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));
  g_signal_emit_by_name (local->priv->asend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[0]);
  g_signal_emit_by_name (local->priv->asend_rtcp_sink, "remove", addr_only,
      remote->priv->send_ports[1]);
  g_signal_emit_by_name (local->priv->vsend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[3]);
  g_signal_emit_by_name (local->priv->vsend_rtcp_sink, "remove", addr_only,
      remote->priv->send_ports[4]);
  g_free (addr_only);

  /* Pause receiving */
  ret = gst_element_set_state (remote->receive, GST_STATE_PAUSED);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);

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

    ret = gst_element_set_state (remote->priv->aplayback, GST_STATE_PAUSED);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Paused audio of %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    ret = gst_element_set_state (remote->priv->vplayback, GST_STATE_PAUSED);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Paused video of %s", remote->addr_s);
  }

  remote->state = ONE_VIDEO_REMOTE_STATE_PAUSED;
  GST_DEBUG ("Fully paused remote peer %s", remote->addr_s);
}

void
one_video_remote_peer_resume (OneVideoRemotePeer * remote)
{
  gboolean res;
  GstStateChangeReturn ret;
  gchar *addr_only;
  OneVideoLocalPeer *local = remote->local;

  g_assert (remote->state == ONE_VIDEO_REMOTE_STATE_PAUSED);

  /* Start transmitting */
  addr_only = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));
  g_signal_emit_by_name (local->priv->asend_rtp_sink, "add", addr_only,
      remote->priv->send_ports[0]);
  g_signal_emit_by_name (local->priv->asend_rtcp_sink, "add", addr_only,
      remote->priv->send_ports[1]);
  g_signal_emit_by_name (local->priv->vsend_rtp_sink, "add", addr_only,
      remote->priv->send_ports[3]);
  g_signal_emit_by_name (local->priv->vsend_rtcp_sink, "add", addr_only,
      remote->priv->send_ports[4]);
  g_free (addr_only);

  if (remote->priv->audio_proxysrc != NULL) {
    res = gst_element_link_pads (remote->priv->aplayback, "audiopad",
          local->priv->audiomixer, "sink_%u");
    g_assert (res);
    ret = gst_element_set_state (remote->priv->aplayback, GST_STATE_PLAYING);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Resumed audio of %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    ret = gst_element_set_state (remote->priv->vplayback, GST_STATE_PLAYING);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    GST_DEBUG ("Resumed video of %s", remote->addr_s);
  }

  /* Resume receiving */
  ret = gst_element_set_state (remote->receive, GST_STATE_PLAYING);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  remote->state = ONE_VIDEO_REMOTE_STATE_PLAYING;
  GST_DEBUG ("Fully resumed remote peer %s", remote->addr_s);
}

/* Does not do any operations that involve taking the OneVideoLocalPeer lock.
 * See: one_video_remote_peer_remove() 
 *
 * NOT a public symbol */
void
one_video_remote_peer_remove_not_array (OneVideoRemotePeer * remote)
{
  gboolean res;
  GstStateChangeReturn ret;
  gchar *tmp, *addr_only;
  OneVideoLocalPeer *local = remote->local;

  /* Stop transmitting */
  addr_only = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));
  g_signal_emit_by_name (local->priv->asend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[0]);
  g_signal_emit_by_name (local->priv->asend_rtcp_sink, "remove", addr_only,
      remote->priv->send_ports[1]);
  g_signal_emit_by_name (local->priv->vsend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[3]);
  g_signal_emit_by_name (local->priv->vsend_rtcp_sink, "remove", addr_only,
      remote->priv->send_ports[4]);
  g_free (addr_only);

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

    ret = gst_element_set_state (remote->priv->aplayback, GST_STATE_NULL);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    res = gst_bin_remove (GST_BIN (local->playback), remote->priv->aplayback);
    g_assert (res);
    remote->priv->aplayback = NULL;
    GST_DEBUG ("Released audio playback bin of remote %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    ret = gst_element_set_state (remote->priv->vplayback, GST_STATE_NULL);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    res = gst_bin_remove (GST_BIN (local->playback), remote->priv->vplayback);
    g_assert (res);
    remote->priv->vplayback = NULL;
    GST_DEBUG ("Released video playback bin of remote %s", remote->addr_s);
  }

  /* Stop receiving */
  ret = gst_element_set_state (remote->receive, GST_STATE_NULL);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  remote->state = ONE_VIDEO_REMOTE_STATE_NULL;

  tmp = g_strdup (remote->addr_s);
  one_video_remote_peer_free (remote);
  GST_DEBUG ("Freed everything for remote peer %s", tmp);
  g_free (tmp);
}

void
one_video_remote_peer_remove (OneVideoRemotePeer * remote)
{
  /* Remove from the peers list first so nothing else tries to use it */
  g_rec_mutex_lock (&remote->local->priv->lock);
  g_ptr_array_remove (remote->local->priv->remote_peers, remote);
  g_rec_mutex_unlock (&remote->local->priv->lock);

  one_video_remote_peer_remove_not_array (remote);
}

void
one_video_remote_peer_free (OneVideoRemotePeer * remote)
{
  guint ii;
  OneVideoLocalPeer *local = remote->local;

  GST_DEBUG ("Freeing remote %s", remote->addr_s);
  g_rec_mutex_lock (&local->priv->lock);
  for (ii = 0; ii < local->priv->used_ports->len; ii++)
    /* Port numbers are unique, sorted, and contiguous. So if we find the first
     * port, we've found all of them. */
    if (g_array_index (local->priv->used_ports, guint, ii) ==
        remote->priv->recv_ports[0])
      g_array_remove_range (local->priv->used_ports, ii, 4);
  g_rec_mutex_unlock (&local->priv->lock);

  /* Free relevant bins and pipelines */
  if (remote->priv->aplayback)
    gst_object_unref (remote->priv->aplayback);
  if (remote->priv->vplayback)
    gst_object_unref (remote->priv->vplayback);
  gst_object_unref (remote->receive);

  if (remote->priv->recv_acaps)
    gst_caps_unref (remote->priv->recv_acaps);
  if (remote->priv->recv_vcaps)
    gst_caps_unref (remote->priv->recv_vcaps);
  g_object_unref (remote->addr);
  g_free (remote->addr_s);
  g_free (remote->priv);
  g_free (remote);
}

OneVideoDiscoveredPeer *
one_video_discovered_peer_new (GInetSocketAddress * addr)
{
  OneVideoDiscoveredPeer *d;

  d = g_new0 (OneVideoDiscoveredPeer, 1);
  d->addr = g_object_ref (addr);
  d->discover_time = g_get_monotonic_time ();

  if (g_inet_socket_address_get_port (addr) == ONE_VIDEO_DEFAULT_COMM_PORT)
    d->addr_s =
      g_inet_address_to_string (g_inet_socket_address_get_address (addr));
  else
    d->addr_s =
      one_video_inet_socket_address_to_string (addr);

  return d;
}

void
one_video_discovered_peer_free (OneVideoDiscoveredPeer * peer)
{
  g_object_unref (peer->addr);
  g_free (peer->addr_s);
  g_free (peer);
}

static GstCaps *
one_video_media_type_to_caps (OneVideoMediaType type)
{
  switch (type) {
    case ONE_VIDEO_MEDIA_TYPE_JPEG:
      return gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);
    case ONE_VIDEO_MEDIA_TYPE_YUY2:
      return gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING,
          "YUY2", NULL);
    case ONE_VIDEO_MEDIA_TYPE_H264:
      return gst_caps_new_empty_simple (VIDEO_FORMAT_H264);
    default:
      g_assert_not_reached ();
  }
  return NULL;
}

/* Get the caps from the device and extract the useful caps from it
 * Useful caps are those that are high-def and high framerate, or if none such
 * are found, high-def and low-framerate, then low-def and high-framerate, then
 * low-def and low-framerate.
 *
 * Currently only returns image/jpeg caps */
static GstCaps *
one_video_device_get_usable_caps (GstDevice * device, OneVideoMediaType * type)
{
  gchar *tmp;
  gint ii, len;
  GstCaps *retcaps, *caps1, *caps2;

  retcaps = gst_device_get_caps (device);

  /* Try extracting jpeg-only structures first */
  *type = ONE_VIDEO_MEDIA_TYPE_JPEG;

extract_caps:
  caps2 = one_video_media_type_to_caps (*type);
  caps1 = gst_caps_intersect (retcaps, caps2);
  g_clear_pointer (&caps2, gst_caps_unref);

  if (gst_caps_is_empty (caps1)) {
    gst_caps_unref (caps1);
    switch (*type) {
      /* Device does not support JPEG, try YUY2
       * We don't try other RAW formats because those are all emulated by libv4l2
       * by converting/decoding from JPEG or YUY2 */
      case ONE_VIDEO_MEDIA_TYPE_JPEG:
        /* TODO: Not supported yet (needs support in the transmit pipeline) */
        g_assert_not_reached ();
        /* With YUY2, we will encode to JPEG before transmitting */
        *type = ONE_VIDEO_MEDIA_TYPE_YUY2;
        goto extract_caps; /* try again */
      default:
        tmp = gst_caps_to_string (retcaps);
        GST_ERROR ("Device doesn't support JPEG or YUY2! Supported caps: %s",
            tmp);
        g_free (tmp);
        g_clear_pointer (&retcaps, gst_caps_unref);
        return NULL; /* fail */
    }
  }

  /* We now have a useful subset of the original device caps */
  gst_caps_replace (&retcaps, caps1);
  g_clear_pointer (&caps1, gst_caps_unref);

  /* Transform device caps to rtp caps */
  len = gst_caps_get_size (retcaps);
  for (ii = 0; ii < len; ii++) {
    GstStructure *s;
    gint n1, n2;
    gdouble dest;

    s = gst_caps_get_structure (retcaps, ii);

    /* Skip caps structures that aren't 16:9 */
    gst_structure_get_int (s, "width", &n1);
    gst_structure_get_int (s, "height", &n2);
    if ((n1 * 9 - n2 * 16) != 0)
      goto remove;

    /* Fixate device caps and remove extraneous fields */
    gst_structure_remove_fields (s, "pixel-aspect-ratio", "colorimetry",
        "interlace-mode", NULL);
    gst_structure_fixate (s);

    /* Remove framerates less than 15; those look too choppy */
    gst_structure_get_fraction (s, "framerate", &n1, &n2);
    gst_util_fraction_to_double (n1, n2, &dest);
    if ((*type == ONE_VIDEO_MEDIA_TYPE_JPEG && dest < 30) ||
        (*type == ONE_VIDEO_MEDIA_TYPE_YUY2 && dest < 15))
      goto remove;

    continue;
remove:
    gst_caps_remove_structure (retcaps, ii);
    ii--; len--;
  }

  return retcaps;
}

GList *
one_video_local_peer_get_video_devices (OneVideoLocalPeer * local)
{
  return gst_device_monitor_get_devices (local->priv->dm);
}

gboolean
one_video_local_peer_set_video_device (OneVideoLocalPeer * local,
    GstDevice * device)
{
  gchar *caps;
  OneVideoMediaType video_media_type;

  /* TODO: Currently, we can only get a device that outputs JPEG and our
   * transmit code assumes that. When we fix that to also support YUY2 and
   * H.264, we need to fix all this code too. */
  if (device) {
    local->priv->supported_send_vcaps =
      one_video_device_get_usable_caps (device, &video_media_type);
  } else {
    local->priv->supported_send_vcaps =
      gst_caps_from_string (VIDEO_FORMAT_JPEG CAPS_SEP VIDEO_CAPS_STR);
  }

  caps = gst_caps_to_string (local->priv->supported_send_vcaps);
  GST_DEBUG ("Supported send vcaps: %s", caps);
  g_free (caps);

  /* Setup transmit pipeline */
  return one_video_local_peer_setup_transmit_pipeline (local, device);
}

typedef struct {
  OneVideoRemoteFoundCallback callback;
  gpointer callback_data;
  GCancellable *cancellable;
} OneVideoDiscoveryReplyData;

static gboolean
recv_discovery_reply (GSocket * socket, GIOCondition condition,
    gpointer user_data)
{
  gboolean ret;
  OneVideoUdpMsg msg;
  GSocketAddress *from;
  OneVideoDiscoveredPeer *d;
  OneVideoDiscoveryReplyData *data = user_data;
  GCancellable *cancellable = data->cancellable;
  GError *error = NULL;

  if (g_cancellable_is_cancelled (cancellable))
    return G_SOURCE_REMOVE;

  GST_DEBUG ("Incoming potential discovery reply");

  ret = one_video_udp_msg_read_message_from (&msg, &from, socket,
      cancellable, &error);
  if (!ret) {
    GST_WARNING ("Error reading discovery reply: %s", error->message);
    g_clear_error (&error);
    return G_SOURCE_CONTINUE;
  }

  /* We don't care about the payload of the message */
  if (msg.size > 0)
    g_free (msg.data);

  if (msg.type != ONE_VIDEO_UDP_MSG_TYPE_UNICAST_HI_THERE) {
    GST_WARNING ("Invalid discovery reply: %u", msg.type);
    ret = G_SOURCE_CONTINUE;
    goto out;
  }

  d = one_video_discovered_peer_new (G_INET_SOCKET_ADDRESS (from));
  GST_DEBUG ("Found a remote peer: %s. Calling user-provided callback.",
      d->addr_s);

  /* Call user-provided callback */
  ret = data->callback (d, data->callback_data);

out:
  g_object_unref (from);
  return ret;
}

/**
 * one_video_local_peer_find_remotes_create_source:
 * @local: a #OneVideoLocalPeer
 * @cancellable: a #GCancellable
 * @callback: a #GFunc called for every remote peer found
 * @callback_data: the data passed to @callback
 * @error: a #GError
 *
 * Creates and returns a #GSource that calls the passed-in @callback for every
 * remote peer found with the #GSocketAddress of the remote peer as the first
 * argument and @callback_data as the second argument. The source is already
 * setup, so you do not need to do anything.
 *
 * To stop searching, call g_cancellable_cancel() on @cancellable, free the
 * source with g_source_unref(), or return %FALSE from @callback. The caller
 * keeps full ownership of @callback_data.
 *
 * On failure to initiate searching for peers, %NULL is returned and @error is
 * set.
 *
 * Returns: (transfer full): a newly allocated #GSource, free with
 * g_source_unref()
 */
/* FIXME: @callback should be a custom type, not GFunc since we want it to
 * return gboolean not void */
GSource *
one_video_local_peer_find_remotes_create_source (OneVideoLocalPeer * local,
    GCancellable * cancellable, OneVideoRemoteFoundCallback callback,
    gpointer callback_data, GError ** error)
{
  gboolean ret;
  GSource *source;
  GSocket *recv_socket;
  OneVideoDiscoveryReplyData *reply_data;

  recv_socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, error);
  if (!recv_socket)
    return NULL;

  ret =
    g_socket_bind (recv_socket, G_SOCKET_ADDRESS (local->addr), TRUE, error);
  if (!ret) {
    g_object_unref (recv_socket);
    return NULL;
  }

  reply_data = g_new0 (OneVideoDiscoveryReplyData, 1);
  reply_data->callback = callback;
  reply_data->callback_data = callback_data;
  reply_data->cancellable = cancellable;

  source = g_socket_create_source (recv_socket, G_IO_IN, cancellable);
  g_source_set_callback (source, (GSourceFunc) recv_discovery_reply, reply_data,
      g_free);
  g_source_set_priority (source, G_PRIORITY_HIGH);
  g_source_attach (source, NULL);
  g_object_unref (recv_socket);
  GST_DEBUG ("Searching for remote peers");

  /* Broadcast to the entire subnet to find listening peers */
  ret = one_video_discovery_send_multicast_discover (local, cancellable, error);
  if (!ret) {
    g_source_unref (source);
    return NULL;
  }

  return source;
}

OneVideoRemotePeer *
one_video_local_peer_get_remote_by_id (OneVideoLocalPeer * local,
    const gchar * id)
{
  guint ii;
  OneVideoRemotePeer *remote;

  g_rec_mutex_lock (&local->priv->lock);
  for (ii = 0; ii < local->priv->remote_peers->len; ii++) {
    remote = g_ptr_array_index (local->priv->remote_peers, ii);
    if (g_strcmp0 (id, remote->id))
      break;
  }
  g_rec_mutex_unlock (&local->priv->lock);

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
      remote->priv->send_ports[3]);
  g_string_append_printf (clients[3], "%s:%u,", addr_s,
      remote->priv->send_ports[4]);

  g_free (addr_s);
}

/* Called with the lock TAKEN */
static gboolean
one_video_local_peer_begin_transmit (OneVideoLocalPeer * local)
{
  GSocket *socket;
  GString **clients;
  gchar *local_addr_s;
  GstStateChangeReturn ret;

  /* {audio RTP, audio RTCP SR, video RTP, video RTCP SR} */
  clients = g_malloc0_n (sizeof (GString*), 4);
  clients[0] = g_string_new ("");
  clients[1] = g_string_new ("");
  clients[2] = g_string_new ("");
  clients[3] = g_string_new ("");
  g_ptr_array_foreach (local->priv->remote_peers, append_clients, clients);

  local_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (local->addr));

  /* Send audio RTP to all remote peers */
  g_object_set (local->priv->asend_rtp_sink, "clients", clients[0]->str, NULL);
  /* Send audio RTCP SRs to all remote peers */
  socket = one_video_get_socket_for_addr (local_addr_s,
      local->priv->recv_rtcp_ports[0]);
  g_object_set (local->priv->asend_rtcp_sink, "clients", clients[1]->str,
      "socket", socket, NULL);
  /* Recv audio RTCP RRs from all remote peers (same socket as above) */
  g_object_set (local->priv->arecv_rtcp_src, "socket", socket, NULL);
  g_object_unref (socket);

  /* Send video RTP to all remote peers */
  g_object_set (local->priv->vsend_rtp_sink, "clients", clients[2]->str, NULL);
  /* Send video RTCP SRs to all remote peers */
  socket = one_video_get_socket_for_addr (local_addr_s,
      local->priv->recv_rtcp_ports[1]);
  g_object_set (local->priv->vsend_rtcp_sink, "clients", clients[3]->str,
      "socket", socket, NULL);
  /* Recv video RTCP RRs from all remote peers (same socket as above) */
  g_object_set (local->priv->vrecv_rtcp_src, "socket", socket, NULL);
  g_object_unref (socket);

  ret = gst_element_set_state (local->transmit, GST_STATE_PLAYING);
  GST_DEBUG ("Transmitting to remote peers. Audio: %s Video: %s",
      clients[0]->str, clients[2]->str);

  g_string_free (clients[0], TRUE);
  g_string_free (clients[1], TRUE);
  g_string_free (clients[2], TRUE);
  g_string_free (clients[3], TRUE);
  g_free (clients);
  g_free (local_addr_s);

  return ret != GST_STATE_CHANGE_FAILURE;
}

void
one_video_local_peer_add_remote (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  /* Add to our list of remote peers */
  g_rec_mutex_lock (&local->priv->lock);
  g_ptr_array_add (local->priv->remote_peers, remote);
  g_rec_mutex_unlock (&local->priv->lock);
}

static gboolean
one_video_local_peer_setup_remote (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  one_video_local_peer_setup_remote_receive (local, remote);
  one_video_local_peer_setup_remote_playback (local, remote);
  
  remote->state = ONE_VIDEO_REMOTE_STATE_READY;

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
  GCancellable *our_cancellable;

  if (local->state != ONE_VIDEO_LOCAL_STATE_INITIALISED) {
    GST_ERROR ("Our state is %u instead of INITIALISED", local->state);
    return FALSE;
  }

  if (cancellable)
    our_cancellable = g_object_ref (cancellable);
  else
    our_cancellable = g_cancellable_new ();

  task = g_task_new (NULL, our_cancellable, callback, callback_data);
  g_task_set_task_data (task, local, NULL);
  g_task_set_return_on_cancel (task, TRUE);
  g_task_run_in_thread (task,
      (GTaskThreadFunc) one_video_local_peer_negotiate_thread);
  local->priv->negotiator_task = task;
  g_object_unref (our_cancellable); /* Hand over ref to the task */
  g_object_unref (task);

  return TRUE;
}

gboolean
one_video_local_peer_negotiate_finish (OneVideoLocalPeer * local,
    GAsyncResult * result, GError ** error)
{
  local->priv->negotiator_task = NULL;
  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
one_video_local_peer_negotiate_stop (OneVideoLocalPeer * local)
{
  g_rec_mutex_lock (&local->priv->lock);

  if (!(local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATING) &&
      !(local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATED)) {
    GST_ERROR ("Can't stop negotiating when not negotiating");
    g_rec_mutex_unlock (&local->priv->lock);
    return FALSE;
  }

  if (local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATOR) {
    g_assert (local->priv->negotiator_task != NULL);
    GST_DEBUG ("Stopping negotiation as the negotiator");
    g_cancellable_cancel (
        g_task_get_cancellable (local->priv->negotiator_task));
    /* Unlock mutex so that the other thread gets access */
  } else if (local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATEE) {
    GST_DEBUG ("Stopping negotiation as the negotiatee");
    g_source_remove (local->priv->negotiate->check_timeout_id);
    g_clear_pointer (&local->priv->negotiate->remotes,
        (GDestroyNotify) g_hash_table_unref);
    g_clear_pointer (&local->priv->negotiate, g_free);
    /* Reset state so we accept incoming connections again */
    local->state = ONE_VIDEO_LOCAL_STATE_INITIALISED;
  } else {
    g_assert_not_reached ();
  }

  local->state |= ONE_VIDEO_LOCAL_STATE_FAILED;

  g_rec_mutex_unlock (&local->priv->lock);
  return TRUE;
}

gboolean
one_video_local_peer_start (OneVideoLocalPeer * local)
{
  guint index;
  gboolean res;
  GstStateChangeReturn ret;
  OneVideoRemotePeer *remote;

  if (!(local->state & ONE_VIDEO_LOCAL_STATE_READY)) {
    GST_ERROR ("Negotiation hasn't been done yet!");
    return FALSE;
  }

  g_rec_mutex_lock (&local->priv->lock);
  res = one_video_local_peer_begin_transmit (local);
  g_assert (res);

  for (index = 0; index < local->priv->remote_peers->len; index++) {
    remote = g_ptr_array_index (local->priv->remote_peers, index);
    
    /* Call details have all been set, so we can do the setup */
    res = one_video_local_peer_setup_remote (local, remote);
    g_assert (res);

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
  /* The difference between negotiator and negotiatee ends with playback */
  local->state = ONE_VIDEO_LOCAL_STATE_PLAYING;
  g_rec_mutex_unlock (&local->priv->lock);
  return TRUE;

  play_fail: {
    GST_ERROR ("Unable to set local playback pipeline to PLAYING!");
    g_rec_mutex_unlock (&local->priv->lock);
    return FALSE;
  }

  recv_fail: {
    GST_ERROR ("Unable to set %s receive pipeline to PLAYING!", remote->addr_s);
    g_rec_mutex_unlock (&local->priv->lock);
    return FALSE;
  }
}

void
one_video_local_peer_stop (OneVideoLocalPeer * local)
{
  GST_DEBUG ("Stopping local peer");
  g_rec_mutex_lock (&local->priv->lock);
  /* Stop negotiating if negotiating */
  if (local->state & ONE_VIDEO_LOCAL_STATE_NEGOTIATING) {
    GST_DEBUG ("Cancelling call negotiation");
    one_video_local_peer_negotiate_stop (local);
  }

  /* Signal end of call if we're in a call */
  if (local->state >= ONE_VIDEO_LOCAL_STATE_READY &&
      local->priv->remote_peers->len > 0) {
    GST_DEBUG ("Sending END_CALL to remote peers");
    one_video_local_peer_end_call (local);
  }

  /* Remove all the remote peers added to the local peer */
  if (local->priv->remote_peers->len > 0) {
    g_ptr_array_foreach (local->priv->remote_peers,
        (GFunc) one_video_remote_peer_remove_not_array, NULL);
    g_ptr_array_free (local->priv->remote_peers, TRUE);
    local->priv->remote_peers = g_ptr_array_new ();
  }

  if (local->state >= ONE_VIDEO_LOCAL_STATE_PLAYING) {
    GST_DEBUG ("Stopping transmit and playback");
    one_video_local_peer_stop_transmit (local);
    one_video_local_peer_stop_playback (local);
  }

  local->state = ONE_VIDEO_LOCAL_STATE_STOPPED;
  g_clear_pointer (&local->priv->send_acaps, gst_caps_unref);
  g_clear_pointer (&local->priv->send_vcaps, gst_caps_unref);
  g_rec_mutex_unlock (&local->priv->lock);
}
