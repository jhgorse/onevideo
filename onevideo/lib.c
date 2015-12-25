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

#include "lib-priv.h"
#include "comms.h"
#include "utils.h"
#include "outgoing.h"
#include "discovery.h"

#include "ov-local-peer.h"
#include "ov-local-peer-priv.h"
#include "ov-local-peer-setup.h"

#include <string.h>

GST_DEBUG_CATEGORY (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

static gboolean ov_local_peer_begin_transmit (OvLocalPeer *local);

#define on_remote_receive_error ov_on_gst_bus_error

static void
ov_local_peer_stop_transmit (OvLocalPeer * local)
{
  GstStateChangeReturn ret;
  OvLocalPeerPrivate *priv;
  
  priv = ov_local_peer_get_private (local);

  if (priv->transmit != NULL) {
    ret = gst_element_set_state (priv->transmit, GST_STATE_NULL);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  }
  /* WORKAROUND: We re-setup the transmit pipeline on repeat transmits */
  g_clear_object (&priv->transmit);
  GST_DEBUG ("Stopped transmitting");
}

static void
ov_local_peer_stop_playback (OvLocalPeer * local)
{
  GstStateChangeReturn ret;
  OvLocalPeerPrivate *priv;
  
  priv = ov_local_peer_get_private (local);
  ret = gst_element_set_state (priv->playback, GST_STATE_NULL);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  GST_DEBUG ("Stopped playback");
}

static gint
compare_ints (const void * a, const void * b)
{
  return (*(const unsigned int*)a - *(const unsigned int*)b);
}

/* Called with the lock TAKEN */
static gboolean
set_free_recv_ports (OvLocalPeer * local, guint (*recv_ports)[4])
{
  guint ii, start;
  OvLocalPeerPrivate *priv;
  
  priv = ov_local_peer_get_private (local);

  /* Start from the port right after the video RTCP recv port */
  start = 1 + priv->recv_rtcp_ports[1];

  g_array_sort (priv->used_ports, compare_ints);

  /* Recv ports are always in contiguous sets of 4, so if we
   * find a hole in the sorted list of used ports, it has 4 unused ports */
  for (ii = 0; ii < priv->used_ports->len; ii++)
    if (g_array_index (priv->used_ports, guint, ii) == start)
      start++;
    else
      break;

  /* TODO: Check whether these ports are actually available on the system */

  for (ii = 0; ii < 4; ii++)
    (*recv_ports)[ii] = start + ii;
  g_array_append_vals (priv->used_ports, recv_ports, 4);
  return TRUE;
}

OvRemotePeer *
ov_remote_peer_new (OvLocalPeer * local, GInetSocketAddress * addr)
{
  gchar *name;
  GstBus *bus;
  gboolean ret;
  OvRemotePeer *remote;
  
  remote = g_new0 (OvRemotePeer, 1);
  remote->state = OV_REMOTE_STATE_NULL;
  remote->local = local;
  remote->addr = g_object_ref (addr);
  remote->addr_s = ov_inet_socket_address_to_string (remote->addr);

  name = g_strdup_printf ("receive-%s", remote->addr_s);
  remote->receive = gst_object_ref_sink (gst_pipeline_new (name));
  g_free (name);

  remote->priv = g_new0 (OvRemotePeerPrivate, 1);
  name = g_strdup_printf ("audio-playback-bin-%s", remote->addr_s);
  remote->priv->aplayback = gst_bin_new (name);
  g_free (name);
  name = g_strdup_printf ("video-playback-bin-%s", remote->addr_s);
  remote->priv->vplayback = gst_bin_new (name);
  g_free (name);

  /* We need a lock for set_free_recv_ports() which manipulates
   * local->priv->used_ports */
  ov_local_peer_lock (local);
  ret = set_free_recv_ports (local, &remote->priv->recv_ports);
  g_assert (ret);
  ov_local_peer_unlock (local);

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

  remote->state = OV_REMOTE_STATE_ALLOCATED;

  return remote;
}

OvRemotePeer *
ov_remote_peer_new_from_string (OvLocalPeer * local, const gchar * addr_s)
{
  GInetSocketAddress *addr;
  OvRemotePeer *remote;
  
  addr = ov_inet_socket_address_from_string (addr_s);
  remote = ov_remote_peer_new (local, addr);
  g_object_unref (addr);

  return remote;
}

gpointer
ov_remote_peer_add_gtkglsink (OvRemotePeer * remote)
{
  gpointer widget;

  remote->priv->video_sink = gst_element_factory_make ("gtkglsink", NULL);
  if (!remote->priv->video_sink) {
    GST_ERROR ("Unable to create gtkglsink; falling back to glimagesink");
    return NULL;
  }

  g_object_get (remote->priv->video_sink, "widget", &widget, NULL);
  return widget;
}

void
ov_remote_peer_pause (OvRemotePeer * remote)
{
  GstStateChangeReturn ret;
  gchar *addr_only;
  OvLocalPeerPrivate *local_priv;
  
  local_priv = ov_local_peer_get_private (remote->local);

  g_assert (remote->state == OV_REMOTE_STATE_PLAYING);

  /* Stop transmitting */
  addr_only = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));
  g_signal_emit_by_name (local_priv->asend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[0]);
  g_signal_emit_by_name (local_priv->asend_rtcp_sink, "remove", addr_only,
      remote->priv->send_ports[1]);
  g_signal_emit_by_name (local_priv->vsend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[3]);
  g_signal_emit_by_name (local_priv->vsend_rtcp_sink, "remove", addr_only,
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

    gst_element_release_request_pad (local_priv->audiomixer, sinkpad);
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

  remote->state = OV_REMOTE_STATE_PAUSED;
  GST_DEBUG ("Fully paused remote peer %s", remote->addr_s);
}

void
ov_remote_peer_resume (OvRemotePeer * remote)
{
  gboolean res;
  GstStateChangeReturn ret;
  gchar *addr_only;
  OvLocalPeerPrivate *local_priv;
  
  local_priv = ov_local_peer_get_private (remote->local);

  g_assert (remote->state == OV_REMOTE_STATE_PAUSED);

  /* Start transmitting */
  addr_only = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));
  g_signal_emit_by_name (local_priv->asend_rtp_sink, "add", addr_only,
      remote->priv->send_ports[0]);
  g_signal_emit_by_name (local_priv->asend_rtcp_sink, "add", addr_only,
      remote->priv->send_ports[1]);
  g_signal_emit_by_name (local_priv->vsend_rtp_sink, "add", addr_only,
      remote->priv->send_ports[3]);
  g_signal_emit_by_name (local_priv->vsend_rtcp_sink, "add", addr_only,
      remote->priv->send_ports[4]);
  g_free (addr_only);

  if (remote->priv->audio_proxysrc != NULL) {
    res = gst_element_link_pads (remote->priv->aplayback, "audiopad",
          local_priv->audiomixer, "sink_%u");
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
  remote->state = OV_REMOTE_STATE_PLAYING;
  GST_DEBUG ("Fully resumed remote peer %s", remote->addr_s);
}

/* Does not do any operations that involve taking the OvLocalPeer lock.
 * See: ov_remote_peer_remove() 
 *
 * NOT a public symbol */
void
ov_remote_peer_remove_not_array (OvRemotePeer * remote)
{
  gboolean res;
  GstStateChangeReturn ret;
  gchar *tmp, *addr_only;
  OvLocalPeerPrivate *local_priv;
  
  local_priv = ov_local_peer_get_private (remote->local);

  /* Stop transmitting */
  addr_only = g_inet_address_to_string (
      g_inet_socket_address_get_address (remote->addr));
  g_signal_emit_by_name (local_priv->asend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[0]);
  g_signal_emit_by_name (local_priv->asend_rtcp_sink, "remove", addr_only,
      remote->priv->send_ports[1]);
  g_signal_emit_by_name (local_priv->vsend_rtp_sink, "remove", addr_only,
      remote->priv->send_ports[3]);
  g_signal_emit_by_name (local_priv->vsend_rtcp_sink, "remove", addr_only,
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

      gst_element_release_request_pad (local_priv->audiomixer, sinkpad);
      gst_object_unref (sinkpad);
      GST_DEBUG ("Released audiomixer sinkpad of %s", remote->addr_s);
    } else {
      GST_DEBUG ("Remote %s wasn't playing", remote->addr_s);
    }
    gst_object_unref (srcpad);

    ret = gst_element_set_state (remote->priv->aplayback, GST_STATE_NULL);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    res =
      gst_bin_remove (GST_BIN (local_priv->playback), remote->priv->aplayback);
    g_assert (res);
    remote->priv->aplayback = NULL;
    GST_DEBUG ("Released audio playback bin of remote %s", remote->addr_s);
  }

  if (remote->priv->video_proxysrc != NULL) {
    ret = gst_element_set_state (remote->priv->vplayback, GST_STATE_NULL);
    g_assert (ret == GST_STATE_CHANGE_SUCCESS);
    res =
      gst_bin_remove (GST_BIN (local_priv->playback), remote->priv->vplayback);
    g_assert (res);
    remote->priv->vplayback = NULL;
    GST_DEBUG ("Released video playback bin of remote %s", remote->addr_s);
  }

  /* Stop receiving */
  ret = gst_element_set_state (remote->receive, GST_STATE_NULL);
  g_assert (ret == GST_STATE_CHANGE_SUCCESS);
  remote->state = OV_REMOTE_STATE_NULL;

  tmp = g_strdup (remote->addr_s);
  ov_remote_peer_free (remote);
  GST_DEBUG ("Freed everything for remote peer %s", tmp);
  g_free (tmp);
}

void
ov_remote_peer_remove (OvRemotePeer * remote)
{
  OvLocalPeerPrivate *local_priv;
  
  ov_local_peer_lock (remote->local);
  local_priv = ov_local_peer_get_private (remote->local);
  /* Remove from the peers list first so nothing else tries to use it */
  g_ptr_array_remove (local_priv->remote_peers, remote);
  ov_local_peer_unlock (remote->local);

  ov_remote_peer_remove_not_array (remote);
}

void
ov_remote_peer_free (OvRemotePeer * remote)
{
  guint ii;
  OvLocalPeerPrivate *local_priv;
  
  ov_local_peer_lock (remote->local);
  local_priv = ov_local_peer_get_private (remote->local);

  GST_DEBUG ("Freeing remote %s", remote->addr_s);
  for (ii = 0; ii < local_priv->used_ports->len; ii++)
    /* Port numbers are unique, sorted, and contiguous. So if we find the first
     * port, we've found all of them. */
    if (g_array_index (local_priv->used_ports, guint, ii) ==
        remote->priv->recv_ports[0])
      g_array_remove_range (local_priv->used_ports, ii, 4);
  ov_local_peer_unlock (remote->local);

  /* Free relevant bins and pipelines */
  g_clear_object (&remote->priv->aplayback);
  g_clear_object (&remote->priv->vplayback);
  /* Valgrind tells me this results in a double-unref (invalid write)
   * but I'm not sure how that works. Just commenting it out for now. */
  //g_clear_object (&remote->receive);

  if (remote->priv->recv_acaps)
    gst_caps_unref (remote->priv->recv_acaps);
  if (remote->priv->recv_vcaps)
    gst_caps_unref (remote->priv->recv_vcaps);
  g_object_unref (remote->addr);
  g_free (remote->addr_s);
  g_free (remote->id);
  g_free (remote->priv);
  g_free (remote);
}

static GstCaps *
ov_media_type_to_caps (OvMediaType type)
{
  switch (type) {
    case OV_MEDIA_TYPE_JPEG:
      return gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);
    case OV_MEDIA_TYPE_YUY2:
      return gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING,
          "YUY2", NULL);
    case OV_MEDIA_TYPE_H264:
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
ov_device_get_usable_caps (GstDevice * device, OvMediaType * type)
{
  gchar *tmp;
  gint ii, len;
  GstCaps *retcaps, *caps1, *caps2;

  retcaps = gst_device_get_caps (device);

  /* Try extracting jpeg-only structures first */
  *type = OV_MEDIA_TYPE_JPEG;

extract_caps:
  caps2 = ov_media_type_to_caps (*type);
  caps1 = gst_caps_intersect (retcaps, caps2);
  g_clear_pointer (&caps2, gst_caps_unref);

  if (gst_caps_is_empty (caps1)) {
    gst_caps_unref (caps1);
    switch (*type) {
      /* Device does not support JPEG, try YUY2
       * We don't try other RAW formats because those are all emulated by libv4l2
       * by converting/decoding from JPEG or YUY2 */
      case OV_MEDIA_TYPE_JPEG:
        /* TODO: Not supported yet (needs support in the transmit pipeline) */
        g_assert_not_reached ();
        /* With YUY2, we will encode to JPEG before transmitting */
        *type = OV_MEDIA_TYPE_YUY2;
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
    if ((*type == OV_MEDIA_TYPE_JPEG && dest < 30) ||
        (*type == OV_MEDIA_TYPE_YUY2 && dest < 15))
      goto remove;

    continue;
remove:
    gst_caps_remove_structure (retcaps, ii);
    ii--; len--;
  }

  return retcaps;
}

GList *
ov_local_peer_get_video_devices (OvLocalPeer * local)
{
  OvLocalPeerState state;
  OvLocalPeerPrivate *priv;
  
  priv = ov_local_peer_get_private (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_STARTED)) {
    GST_ERROR ("Can't get video devices before being started!");
    return NULL;
  }

  return gst_device_monitor_get_devices (priv->dm);
}

gboolean
ov_local_peer_set_video_device (OvLocalPeer * local,
    GstDevice * device)
{
  gchar *caps;
  OvMediaType video_media_type;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  
  priv = ov_local_peer_get_private (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_STARTED)) {
    GST_ERROR ("Can't set video device before being started!");
    return FALSE;
  }

  /* TODO: Currently, we can only get a device that outputs JPEG and our
   * transmit code assumes that. When we fix that to also support YUY2 and
   * H.264, we need to fix all this code too. */
  if (device) {
    priv->supported_send_vcaps =
      ov_device_get_usable_caps (device, &video_media_type);
  } else {
    priv->supported_send_vcaps =
      gst_caps_from_string (VIDEO_FORMAT_JPEG CAPS_SEP VIDEO_CAPS_STR);
  }

  caps = gst_caps_to_string (priv->supported_send_vcaps);
  GST_DEBUG ("Supported send vcaps: %s", caps);
  g_free (caps);

  /* Setup transmit pipeline */
  priv->video_device = device;
  return ov_local_peer_setup_transmit_pipeline (local);
}

static gboolean
ov_local_peer_discovery_send (OvLocalPeer * local, GError ** error)
{
  gboolean ret;

  /* Broadcast to the entire subnet to find listening peers */
  ret = ov_discovery_send_multicast_discover (local, NULL, error);
  if (!ret)
    return ret;

  g_signal_emit (local, ov_local_peer_signals[DISCOVERY_SENT], 0);

  return G_SOURCE_CONTINUE;
}

static gboolean
discovery_send_cb (OvLocalPeer * local)
{
  OvLocalPeerPrivate *priv;
  
  priv = ov_local_peer_get_private (local);

  if (priv->discover_socket_source == NULL ||
      g_source_is_destroyed (priv->discover_socket_source))
    return G_SOURCE_REMOVE;

  ov_local_peer_discovery_send (local, NULL);

  /* We must always return CONTINUE here unless the source has been destroyed */
  return G_SOURCE_CONTINUE;
}

static gboolean
on_incoming_discovery_reply (GSocket * socket, GIOCondition condition,
    OvLocalPeer * local)
{
  gboolean ret;
  OvUdpMsg msg;
  GSocketAddress *from;
  OvDiscoveredPeer *d;
  gchar *addr_s, *tmp;
  GError *error = NULL;

  ret = ov_udp_msg_read_message_from (&msg, &from, socket,
      NULL, &error);
  if (!ret) {
    GST_WARNING ("Error reading discovery reply: %s", error->message);
    g_clear_error (&error);
    return G_SOURCE_CONTINUE;
  }

  tmp = ov_inet_socket_address_to_string (G_INET_SOCKET_ADDRESS (from));
  GST_TRACE ("Incoming potential discovery reply from %s", tmp);
  g_free (tmp);

  /* We don't care about the payload of the message */
  if (msg.size > 0)
    g_free (msg.data);

  if (msg.type != OV_UDP_MSG_TYPE_UNICAST_HI_THERE) {
    GST_WARNING ("Invalid discovery reply: %u", msg.type);
    ret = G_SOURCE_CONTINUE;
    goto out;
  }

  d = ov_discovered_peer_new (G_INET_SOCKET_ADDRESS (from));
  g_object_get (d, "address-string", &addr_s, NULL);
  GST_TRACE ("Found a remote peer: %s; emitting signal", addr_s);
  g_free (addr_s);

  g_signal_emit (local, ov_local_peer_signals[PEER_DISCOVERED], 0, d);
  g_object_unref (d);

out:
  g_object_unref (from);
  return ret;
}

/**
 * ov_local_peer_discovery_start:
 * @local: the local peer
 * @interval: time in seconds between each discovery probe
 *
 * Enables emission of the #OvLocalPeer::peer_discovered signal, which is
 * emitted whenever a peer is discovered using multicast UDP probes.
 *
 * Connect to that signal before calling this and set @interval to the number of
 * seconds between multicast UDP probes for peers. Setting @interval to 0 uses
 * the default duration (5 seconds).
 *
 * Returns: %TRUE if discovery was started successfully, %FALSE otherwise
 */
gboolean
ov_local_peer_discovery_start (OvLocalPeer * local, guint interval,
    GError ** error)
{
  gboolean ret;
  GSocket *recv_socket;
  GInetSocketAddress *addr;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  recv_socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, error);
  if (!recv_socket)
    return FALSE;
  
  g_object_get (OV_PEER (local), "address", &addr, NULL);
  ret = g_socket_bind (recv_socket, G_SOCKET_ADDRESS (addr), TRUE, error);
  g_object_unref (addr);
  if (!ret) {
    g_object_unref (recv_socket);
    return FALSE;
  }

  priv->discover_socket_source =
    g_socket_create_source (recv_socket, G_IO_IN, NULL);
  g_source_set_callback (priv->discover_socket_source,
      (GSourceFunc) on_incoming_discovery_reply, local, NULL);
  g_source_set_priority (priv->discover_socket_source, G_PRIORITY_HIGH);
  g_source_attach (priv->discover_socket_source, NULL);
  g_object_unref (recv_socket);

  GST_DEBUG ("Searching for remote peers");
  ret = ov_local_peer_discovery_send (local, error);
  if (!ret) {
    g_clear_pointer (&priv->discover_socket_source, g_source_destroy);
    return FALSE;
  }

  g_timeout_add_seconds (interval ? interval : 5,
      (GSourceFunc) discovery_send_cb, local);

  return ret;
}

void
ov_local_peer_discovery_stop (OvLocalPeer * local)
{
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  if (priv->discover_socket_source == NULL ||
      g_source_is_destroyed (priv->discover_socket_source))
    return;

  /* TODO: We don't use g_source_destroy() here because of a strange bug that
   * I wasn't able to track down. Destroying the source was causing all future
   * sources attached to the same socket address to not fire events for
   * incoming unicast UDP messages. It would still fire for incoming multicast
   * messages. */
  g_source_remove (g_source_get_id (priv->discover_socket_source));
  g_clear_pointer (&priv->discover_socket_source, g_source_unref);
}

GPtrArray *
ov_local_peer_get_remotes (OvLocalPeer * local)
{
  /* XXX: Access to this is not thread-safe
   * Perhaps we should return a copy with OvPeers? */
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (local);
  return priv->remote_peers;
}

OvRemotePeer *
ov_local_peer_get_remote_by_id (OvLocalPeer * local,
    const gchar * id)
{
  guint ii;
  OvRemotePeer *remote;
  OvLocalPeerPrivate *priv;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);

  for (ii = 0; ii < priv->remote_peers->len; ii++) {
    remote = g_ptr_array_index (priv->remote_peers, ii);
    if (g_strcmp0 (id, remote->id))
      break;
  }
  ov_local_peer_unlock (local);

  return remote;
}

static void
append_clients (gpointer data, gpointer user_data)
{
  OvRemotePeer *remote = data;
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
ov_local_peer_begin_transmit (OvLocalPeer * local)
{
  GSocket *socket;
  GString **clients;
  gchar *local_addr_s;
  GInetSocketAddress *addr;
  GstStateChangeReturn ret;
  OvLocalPeerPrivate *priv;
  
  priv = ov_local_peer_get_private (local);

  /* {audio RTP, audio RTCP SR, video RTP, video RTCP SR} */
  clients = g_malloc0_n (sizeof (GString*), 4);
  clients[0] = g_string_new ("");
  clients[1] = g_string_new ("");
  clients[2] = g_string_new ("");
  clients[3] = g_string_new ("");
  g_ptr_array_foreach (priv->remote_peers, append_clients, clients);

  g_object_get (OV_PEER (local), "address", &addr, NULL);
  local_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (addr));
  g_object_unref (addr);

  /* Send audio RTP to all remote peers */
  g_object_set (priv->asend_rtp_sink, "clients", clients[0]->str, NULL);
  /* Send audio RTCP SRs to all remote peers */
  socket = ov_get_socket_for_addr (local_addr_s, priv->recv_rtcp_ports[0]);
  g_object_set (priv->asend_rtcp_sink, "clients", clients[1]->str,
      "socket", socket, NULL);
  /* Recv audio RTCP RRs from all remote peers (same socket as above) */
  g_object_set (priv->arecv_rtcp_src, "socket", socket, NULL);
  g_object_unref (socket);

  /* Send video RTP to all remote peers */
  g_object_set (priv->vsend_rtp_sink, "clients", clients[2]->str, NULL);
  /* Send video RTCP SRs to all remote peers */
  socket = ov_get_socket_for_addr (local_addr_s, priv->recv_rtcp_ports[1]);
  g_object_set (priv->vsend_rtcp_sink, "clients", clients[3]->str,
      "socket", socket, NULL);
  /* Recv video RTCP RRs from all remote peers (same socket as above) */
  g_object_set (priv->vrecv_rtcp_src, "socket", socket, NULL);
  g_object_unref (socket);

  ret = gst_element_set_state (priv->transmit, GST_STATE_PLAYING);
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
ov_local_peer_add_remote (OvLocalPeer * local, OvRemotePeer * remote)
{
  OvLocalPeerPrivate *priv;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);
  /* Add to our list of remote peers */
  g_ptr_array_add (priv->remote_peers, remote);
  ov_local_peer_unlock (local);
}

static gboolean
ov_local_peer_setup_remote (OvLocalPeer * local, OvRemotePeer * remote)
{
  ov_local_peer_setup_remote_receive (local, remote);
  ov_local_peer_setup_remote_playback (local, remote);
  
  remote->state = OV_REMOTE_STATE_READY;

  return TRUE;
}

gboolean
ov_local_peer_start (OvLocalPeer * local)
{
  gboolean ret;
  OvLocalPeerState state;
  OvLocalPeerPrivate *priv;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);
  state = ov_local_peer_get_state (local);

  if (state != OV_LOCAL_STATE_NULL) {
    GST_ERROR ("Local peer has already been started!");
    ret = FALSE;
    goto out;
  }

  /* Set interfaces, or auto-detect them */
  if (priv->iface)
    priv->mc_ifaces = g_list_append (NULL, g_strdup (priv->iface));
  else
    priv->mc_ifaces = ov_get_network_interfaces ();

  GST_DEBUG ("Starting device monitor");
  /* Start probing devices asynchronously. We don't listen to the bus messages
   * for this right now, and just get the list of all devices later. */
  gst_device_monitor_start (priv->dm);

  /*-- Setup various pipelines and resources --*/

  /* Transmit pipeline is setup in set_video_device() */

  /* Setup components of the playback pipeline */
  if (!ov_local_peer_setup_playback_pipeline (local))
    goto err;

  /* Setup negotiation/comms */
  if (!ov_local_peer_setup_comms (local))
    goto err;

  ov_local_peer_set_state (local, OV_LOCAL_STATE_STARTED);

  ret = TRUE;
out:
  ov_local_peer_unlock (local);
  return ret;
err:
  ret = FALSE;
  gst_device_monitor_stop (priv->dm);
  g_list_free_full (priv->mc_ifaces, g_free);
  priv->mc_ifaces = NULL;
  goto out;
}

gboolean
ov_local_peer_call_start (OvLocalPeer * local)
{
  guint index;
  gboolean res;
  GstStateChangeReturn ret;
  OvRemotePeer *remote;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);
  state = ov_local_peer_get_state (local);

  if (!(state & OV_LOCAL_STATE_READY)) {
    GST_ERROR ("Negotiation hasn't been done yet!");
    ov_local_peer_unlock (local);
    return FALSE;
  }

  if (priv->transmit == NULL) {
    /* WORKAROUND: We re-setup the transmit pipeline on repeat transmits */
    res = ov_local_peer_setup_transmit_pipeline (local);
    g_assert (res);
  }
  res = ov_local_peer_begin_transmit (local);
  g_assert (res);

  for (index = 0; index < priv->remote_peers->len; index++) {
    remote = g_ptr_array_index (priv->remote_peers, index);
    
    /* Call details have all been set, so we can do the setup */
    res = ov_local_peer_setup_remote (local, remote);
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
    remote->state = OV_REMOTE_STATE_PLAYING;
  }

  ret = gst_element_set_state (priv->playback, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto play_fail;

  GST_DEBUG ("Ready to playback data from all remotes");
  /* The difference between negotiator and negotiatee ends with playback */
  ov_local_peer_set_state (local, OV_LOCAL_STATE_PLAYING);
  ov_local_peer_unlock (local);
  return TRUE;

  play_fail: {
    GST_ERROR ("Unable to set local playback pipeline to PLAYING!");
    ov_local_peer_unlock (local);
    return FALSE;
  }

  recv_fail: {
    GST_ERROR ("Unable to set %s receive pipeline to PLAYING!", remote->addr_s);
    ov_local_peer_unlock (local);
    return FALSE;
  }
}

/* Resets the local peer to a state equivalent to after callign
 * ov_local_peer_start() */
void
ov_local_peer_call_hangup (OvLocalPeer * local)
{
  OvLocalPeerState state;
  OvLocalPeerPrivate *priv;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);
  state = ov_local_peer_get_state (local);

  /* Signal end of call if we're in a call and haven't ended the call */
  if (state >= OV_LOCAL_STATE_READY && priv->remote_peers->len > 0)
    ov_local_peer_send_end_call (local);

  GST_DEBUG ("Ending call on local peer");
  /* Remove all the remote peers added to the local peer */
  if (priv->remote_peers->len > 0) {
    g_ptr_array_foreach (priv->remote_peers,
        (GFunc) ov_remote_peer_remove_not_array, NULL);
    g_ptr_array_free (priv->remote_peers, TRUE);
    priv->remote_peers = g_ptr_array_new ();
  }

  if (state >= OV_LOCAL_STATE_PLAYING) {
    GST_DEBUG ("Stopping transmit and playback");
    ov_local_peer_stop_transmit (local);
    ov_local_peer_stop_playback (local);
  }

  g_clear_pointer (&priv->send_acaps, gst_caps_unref);
  g_clear_pointer (&priv->send_vcaps, gst_caps_unref);
  /* Revert state to STARTED */
  ov_local_peer_set_state (local, OV_LOCAL_STATE_STARTED);
  ov_local_peer_unlock (local);
}

void
ov_local_peer_stop (OvLocalPeer * local)
{
  OvLocalPeerState state;
  OvLocalPeerPrivate *priv;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);
  state = ov_local_peer_get_state (local);

  GST_DEBUG ("Stopping local peer");
  /* Stop negotiating if negotiating */
  if (state & OV_LOCAL_STATE_NEGOTIATING)
    ov_local_peer_negotiate_abort (local);

  if (state >= OV_LOCAL_STATE_READY)
    ov_local_peer_call_hangup (local);

  if (state >= OV_LOCAL_STATE_STARTED) {
    /* Stop video device monitor */
    gst_device_monitor_stop (priv->dm);

    /* Stop and free TCP server */
    g_signal_handlers_disconnect_by_data (priv->tcp_server, local);
    g_socket_service_stop (priv->tcp_server);
    g_clear_object (&priv->tcp_server);

    /* Stop and destroy multicast socket sources */
    g_clear_pointer (&priv->mc_socket_source, g_source_destroy);

    /* Free and unset detected interfaces */
    g_list_free_full (priv->mc_ifaces, g_free);
    priv->mc_ifaces = NULL;
  }

  ov_local_peer_set_state (local, OV_LOCAL_STATE_STOPPED);
  ov_local_peer_unlock (local);
}
