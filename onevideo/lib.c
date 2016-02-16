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

/* Default timeout for remote peers */
#define OV_REMOTE_PEER_TIMEOUT_SECONDS 10

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
  /* Each call has a new transmit pipeline */
  g_clear_object (&priv->transmit);
  /* Clear capsfilter for new pipeline */
  g_object_set (priv->transmit_vcapsfilter, "caps", NULL, NULL);
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
compare_uint16s (const void * a, const void * b)
{
  return (*(const guint16*)a - *(const guint16*)b);
}

/* Called with the lock TAKEN */
static gboolean
set_free_recv_ports (OvLocalPeer * local, guint16 (*recv_ports)[4])
{
  guint ii, start;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  /* Start from the port right after the video RTCP recv port */
  start = 1 + priv->recv_rtcp_ports[1];

  g_array_sort (priv->used_ports, compare_uint16s);

  /* Recv ports are always in contiguous sets of 4, so if we
   * find a hole in the sorted list of used ports, it has 4 unused ports */
  for (ii = 0; ii < priv->used_ports->len; ii++)
    if (g_array_index (priv->used_ports, guint16, ii) == start)
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

static gboolean
ov_get_gtkglsink (GstElement ** out_sink, gpointer * out_widget)
{
  GstElement *sink, *bin;

  g_return_val_if_fail (out_sink != NULL, FALSE);
  g_return_val_if_fail (out_widget != NULL, FALSE);

  /* Default failure return */
  *out_sink = NULL; *out_widget = NULL;

  if (!(sink = gst_element_factory_make ("gtkglsink", NULL)))
    return FALSE;

  if (!(bin = gst_element_factory_make ("glsinkbin", NULL)))
    return FALSE;

  g_object_set (bin, "sink", sink, NULL);

  g_object_get (sink, "widget", out_widget, NULL);
  *out_sink = bin;
  return TRUE;
}

static gboolean
ov_get_gtksink (GstElement ** out_sink, gpointer * out_widget)
{
  GstElement *sink, *bin, *convert;
  GstPad *ghostpad, *sinkpad;

  g_return_val_if_fail (out_sink != NULL, FALSE);
  g_return_val_if_fail (out_widget != NULL, FALSE);

  /* Default failure return */
  *out_sink = NULL; *out_widget = NULL;

  if (!(sink = gst_element_factory_make ("gtksink", NULL)))
    return FALSE;

  bin = gst_bin_new (NULL);
  if (!gst_bin_add (GST_BIN (bin), sink))
    return FALSE;

  convert = gst_element_factory_make ("videoconvert", NULL);
  if (!gst_bin_add (GST_BIN (bin), convert))
    return FALSE;

  if (!gst_element_link (convert, sink))
    return FALSE;

  sinkpad = gst_element_get_static_pad (convert, "sink");
  ghostpad = gst_ghost_pad_new ("sink", sinkpad);
  g_object_unref (sinkpad);

  if (!gst_pad_set_active (ghostpad, TRUE))
    return FALSE;
  if (!gst_element_add_pad (bin, ghostpad))
    return FALSE;

  g_object_get (sink, "widget", out_widget, NULL);
  *out_sink = bin;
  return TRUE;
}

gboolean
_ov_opengl_is_mesa (void)
{
#ifndef __linux__
  return FALSE;
#else
  gboolean ret;
  gchar *output = NULL;
  g_spawn_command_line_sync ("glxinfo", &output, NULL, NULL, NULL);
  if (output == NULL)
    return FALSE;
  ret = g_regex_match_simple ("OpenGL version.*Mesa", output, 0, 0);
  g_free (output);
  return ret;
#endif
}

gpointer
ov_remote_peer_add_gtksink (OvRemotePeer * remote)
{
  gpointer widget;

  if (_ov_opengl_is_mesa ()) {
    g_printerr ("Running under Mesa OpenGL which is buggy when using multiple"
        " GL contexts. Falling back to software rendering.\n");
    goto software_only;
  }

  /* On Linux (Mesa), using multiple GL output windows leads to a
   * crash due to a bug in Mesa related to multiple GLX contexts */
  if (ov_get_gtkglsink (&remote->priv->video_sink, &widget))
    return widget;

  g_printerr ("Unable to create gtkglsink bin; falling back to gtksink\n");

software_only:
  if (ov_get_gtksink (&remote->priv->video_sink, &widget))
    return widget;

  g_printerr ("Unable to use gtksink; falling back to non-embedded video sink\n");
  return NULL;
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

void
ov_remote_peer_set_muted (OvRemotePeer * remote, gboolean muted)
{
  GstPad *srcpad, *sinkpad;

  g_return_if_fail (remote != NULL);

  srcpad = gst_element_get_static_pad (remote->priv->aplayback, "audiopad");
  g_assert (srcpad);

  sinkpad = gst_pad_get_peer (srcpad);
  g_object_unref (srcpad);

  g_object_set (sinkpad, "mute", muted, NULL);
  g_object_unref (sinkpad);
}

gboolean
ov_remote_peer_get_muted (OvRemotePeer * remote)
{
  gboolean muted;
  GstPad *srcpad, *sinkpad;

  g_return_val_if_fail (remote != NULL, FALSE);

  srcpad = gst_element_get_static_pad (remote->priv->aplayback, "audiopad");
  g_assert (srcpad);

  sinkpad = gst_pad_get_peer (srcpad);
  g_object_unref (srcpad);

  g_object_get (sinkpad, "mute", &muted, NULL);
  g_object_unref (sinkpad);
  return muted;
}

/* Does not do any operations that involve taking the OvLocalPeer lock.
 * See: ov_local_peer_remove_remote()
 *
 * NOT a public symbol */
static void
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

GstCaps *
ov_video_format_to_caps (OvVideoFormat type)
{
  switch (type) {
    case OV_VIDEO_FORMAT_JPEG:
      return gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);
    case OV_VIDEO_FORMAT_YUY2:
    case OV_VIDEO_FORMAT_TEST:
      return gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING,
          "YUY2", NULL);
    case OV_VIDEO_FORMAT_H264:
      return gst_caps_new_empty_simple (VIDEO_FORMAT_H264);
    default:
      g_assert_not_reached ();
  }
  return NULL;
}

OvVideoFormat
ov_caps_to_video_format (const GstCaps * caps)
{
  const gchar *name;
  GstStructure *s = gst_caps_get_structure (caps, 0);

  name = gst_structure_get_name (s);

  if (g_strcmp0 (name, "image/jpeg") == 0)
    return OV_VIDEO_FORMAT_JPEG;
  
  if (g_strcmp0 (name, "video/x-h264") == 0)
    return OV_VIDEO_FORMAT_H264;

  if (g_strcmp0 (name, "video/x-raw") == 0) {
    gboolean ret;
    s = gst_structure_new ("video/x-raw", "format", G_TYPE_STRING, "YUY2", NULL);
    ret = gst_caps_is_subset_structure (caps, s);
    gst_structure_free (s);
    if (ret)
      return OV_VIDEO_FORMAT_YUY2;
  }

  return OV_VIDEO_FORMAT_UNKNOWN;
}

/* Get the caps from the device and extract the useful caps from it
 * Useful caps are those that are high-def and high framerate, or if none such
 * are found, high-def and low-framerate, then low-def and high-framerate, then
 * low-def and low-framerate */
static GstCaps *
ov_device_get_usable_caps (GstDevice * device, OvVideoFormat *device_format)
{
  gint ii, len;
  OvVideoFormat next_format, formats = 0;
  GstCaps *tmpcaps, *devcaps, *retcaps, *mediacaps = NULL;

  devcaps = gst_device_get_caps (device);
  retcaps = gst_caps_new_empty ();

  /* Check for the best quality (H264) first, then JPEG, then YUY2, then fail */
  next_format = OV_VIDEO_FORMAT_H264;

retry:
  g_clear_pointer (&mediacaps, gst_caps_unref);

  switch (next_format) {
    case OV_VIDEO_FORMAT_H264:
    case OV_VIDEO_FORMAT_JPEG:
    case OV_VIDEO_FORMAT_YUY2:
      /* Check if the device supports this format. If so, add it to the list of
       * supported media types and merge the caps in our list of caps. Else, try
       * the next-best video format. */
      mediacaps = ov_video_format_to_caps (next_format);
      tmpcaps = gst_caps_intersect (devcaps, mediacaps);
      if (!gst_caps_is_empty (tmpcaps)) {
        gst_caps_append (retcaps, tmpcaps);
        formats |= next_format;
      } else {
        gst_caps_unref (tmpcaps);
      }
      next_format >>= 1;
      /* If the device does not support JPEG/H264; try YUY2. We ignore other RAW
       * formats because those are all faked by libv4l2 by converting/decoding
       * one of these. We will encode YUY2 to JPEG before transmitting. */
      if (next_format == OV_VIDEO_FORMAT_YUY2 &&
          formats != OV_VIDEO_FORMAT_UNKNOWN) {
        /* Ignore YUY2 formats if we got JPEG and/or H264 */
        gst_caps_unref (mediacaps);
        break; /* done */
      }
      goto retry;

    case OV_VIDEO_FORMAT_SENTINEL:
      if (formats >= OV_VIDEO_FORMAT_YUY2)
        break; /* done */

      GST_ERROR ("Unsupported video output formats! %" GST_PTR_FORMAT, devcaps);
      gst_caps_unref (devcaps);
      gst_caps_unref (retcaps);
      return NULL; /* fail */
    default:
      g_assert_not_reached ();
  }

  gst_caps_unref (devcaps);

  if (formats == OV_VIDEO_FORMAT_YUY2) {
    *device_format = OV_VIDEO_FORMAT_YUY2;
    GST_WARNING ("Device does not provide compressed output! Trying YUY2 "
        "(lower quality, higher CPU usage)");
  }
  /* We now have a useful subset of the original device caps */

  /* Transform device caps to rtp caps */
  len = gst_caps_get_size (retcaps);
  for (ii = 0; ii < len; ii++) {
    GstStructure *s, *tmp;
    gint n1, n2;
    gdouble dest;

    s = gst_caps_get_structure (retcaps, ii);

    /* Fixate device caps and remove extraneous fields */
    gst_structure_remove_fields (s, "pixel-aspect-ratio", "colorimetry",
        "interlace-mode", "format", NULL);

    /* Remove formats smaller than 240p */
    gst_structure_get_int (s, "height", &n1);
    if (n1 < 240)
      goto remove;

    /* Remove caps that *only* have framerates less than 15 */
    tmp = gst_structure_copy (s);
    /* We will probably not get valid framerates higher than this */
    gst_structure_fixate_field_nearest_fraction (tmp, "framerate",
        30, 1);
    gst_structure_get_fraction (tmp, "framerate", &n1, &n2);
    gst_structure_free (tmp);
    gst_util_fraction_to_double (n1, n2, &dest);
    if ((formats >= OV_VIDEO_FORMAT_JPEG && dest < 15) ||
        (formats == OV_VIDEO_FORMAT_YUY2 && dest < 15))
      goto remove;

    /* The raw video will be encoded to JPEG, so in reality our supported video
     * caps are JPEG, not raw */
    if (g_strcmp0 (gst_structure_get_name (s), "video/x-raw") == 0)
      gst_structure_set_name (s, "image/jpeg");

    continue;
remove:
    gst_caps_remove_structure (retcaps, ii);
    ii--; len--;
  }

  GST_DEBUG ("Supported video output formats %" GST_PTR_FORMAT, retcaps);
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
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;

  priv = ov_local_peer_get_private (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_STARTED)) {
    GST_ERROR ("Can't set video device before being started!");
    return FALSE;
  }

  if (device) {
    priv->supported_send_vcaps = ov_device_get_usable_caps (device,
        &priv->device_video_format);
  } else {
    priv->supported_send_vcaps = gst_caps_from_string (
        VIDEO_FORMAT_JPEG CAPS_FIELD_SEP TEST_VIDEO_CAPS_720P_STR CAPS_STRUC_SEP
        VIDEO_FORMAT_JPEG CAPS_FIELD_SEP TEST_VIDEO_CAPS_360P_STR CAPS_STRUC_SEP
        VIDEO_FORMAT_JPEG CAPS_FIELD_SEP TEST_VIDEO_CAPS_240P_STR);
    priv->device_video_format = OV_VIDEO_FORMAT_TEST;
  }

  GST_DEBUG ("Supported send vcaps: %" GST_PTR_FORMAT,
      priv->supported_send_vcaps);

  if (priv->supported_send_vcaps == NULL)
    return FALSE;

  if (gst_caps_is_empty (priv->supported_send_vcaps)) {
    gst_caps_unref (priv->supported_send_vcaps);
    priv->supported_send_vcaps = NULL;
    return FALSE;
  }

  /* Setup transmit pipeline */
  priv->video_device = device ? g_object_ref (device) : NULL; // testsrc if device == NULL, don't ref it
  return TRUE;
}

/* Expects a normalized/fixated structure with no lists of values */
static OvVideoQuality
ov_structure_to_video_quality (const GstStructure * s)
{
  gint height, fps_n, fps_d;
  OvVideoQuality quality = 0;

  if (gst_structure_has_field_typed (s, "height", G_TYPE_INT) &&
      gst_structure_get_int (s, "height", &height)) {
    if (height >= 1080)
      quality |= OV_VIDEO_QUALITY_1080P;
    else if (height >= 720)
      quality |= OV_VIDEO_QUALITY_720P;
    else if (height >= 480)
      quality |= OV_VIDEO_QUALITY_480P;
    else if (height >= 360)
      quality |= OV_VIDEO_QUALITY_360P;
    else if (height >= 240)
      quality |= OV_VIDEO_QUALITY_240P;
    /* Not having a height is not fatal */
  }

  if (gst_structure_has_field_typed (s, "framerate", GST_TYPE_FRACTION) &&
      gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d) &&
      fps_d == 1) {
    if (fps_n >= 60)
      quality |= OV_VIDEO_QUALITY_60FPS;
    else if (fps_n >= 45)
      quality |= OV_VIDEO_QUALITY_45FPS;
    else if (fps_n >= 30)
      quality |= OV_VIDEO_QUALITY_30FPS;
    else if (fps_n >= 25)
      quality |= OV_VIDEO_QUALITY_15FPS;
    else if (fps_n >= 20)
      quality |= OV_VIDEO_QUALITY_15FPS;
    else if (fps_n >= 15)
      quality |= OV_VIDEO_QUALITY_15FPS;
    else if (fps_n >= 10)
      quality |= OV_VIDEO_QUALITY_10FPS;
    else if (fps_n >= 5)
      quality |= OV_VIDEO_QUALITY_5FPS;
    /* Not having a framerate is not fatal */
  }

  return quality;
}

gchar *
ov_video_quality_to_string (OvVideoQuality quality)
{
  gchar *ret;
  GString *str;
  
  if (quality == OV_VIDEO_QUALITY_INVALID)
    return g_strdup ("Invalid");

  str = g_string_new ("");

  if ((quality & OV_VIDEO_QUALITY_RESO_RANGE) == OV_VIDEO_QUALITY_1080P)
    g_string_append (str, "1080p");
  else if ((quality & OV_VIDEO_QUALITY_RESO_RANGE) == OV_VIDEO_QUALITY_720P)
    g_string_append (str, "720p");
  else if ((quality & OV_VIDEO_QUALITY_RESO_RANGE) == OV_VIDEO_QUALITY_480P)
    g_string_append (str, "480p");
  else if ((quality & OV_VIDEO_QUALITY_RESO_RANGE) == OV_VIDEO_QUALITY_360P)
    g_string_append (str, "360p");
  else if ((quality & OV_VIDEO_QUALITY_RESO_RANGE) == OV_VIDEO_QUALITY_240P)
    g_string_append (str, "240p");
  else
    g_string_append (str, "???p");

  if (quality & OV_VIDEO_QUALITY_5FPS)
    g_string_append (str, "5/");
  if (quality & OV_VIDEO_QUALITY_10FPS)
    g_string_append (str, "10/");
  if (quality & OV_VIDEO_QUALITY_15FPS)
    g_string_append (str, "15/");
  if (quality & OV_VIDEO_QUALITY_20FPS)
    g_string_append (str, "20/");
  if (quality & OV_VIDEO_QUALITY_25FPS)
    g_string_append (str, "25/");
  if (quality & OV_VIDEO_QUALITY_30FPS)
    g_string_append (str, "30/");
  if (quality & OV_VIDEO_QUALITY_45FPS)
    g_string_append (str, "45/");
  if (quality & OV_VIDEO_QUALITY_60FPS)
    g_string_append (str, "60/");

  if (str->str[str->len - 1] == '/')
    /* Remove trailing ',' */
    g_string_truncate (str, str->len - 1);
  else
    /* No FPS param could be found */
    g_string_append (str, "??");

  ret = str->str;
  g_string_free (str, FALSE);
  return ret;
}

/* Returns a 0-terminated array of OvVideoQuality enums
 * The array might have duplicate enums because the mapping from enum to video
 * caps is not bijective.
 *
 * Returns NULL if video caps haven't been negotiated yet */
OvVideoQuality *
ov_local_peer_get_negotiated_video_qualities (OvLocalPeer * local)
{
  guint ii, len;
  GstCaps *normalized;
  OvVideoQuality *qualities;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  if (priv->send_vcaps == NULL)
    return NULL;

  normalized = gst_caps_normalize (gst_caps_copy (priv->send_vcaps));

  len = gst_caps_get_size (normalized);
  qualities = g_new0 (OvVideoQuality, len + 1);

  for (ii = 0; ii < len; ii++) {
    GstStructure *s = gst_caps_get_structure (normalized, ii);
    qualities[ii] = ov_structure_to_video_quality (s);
    GST_TRACE ("%i: %" GST_PTR_FORMAT ", ", qualities[ii], s);
  }

  gst_caps_unref (normalized);

  return qualities;
}

/* Returns OV_VIDEO_QUALITY_INVALID if video caps haven't been negotiated yet */
OvVideoQuality
ov_local_peer_get_video_quality (OvLocalPeer * local)
{
  GstCaps *caps;
  OvVideoQuality quality;

  caps = ov_local_peer_get_transmit_video_caps (local);
  if (caps == NULL)
    return OV_VIDEO_QUALITY_INVALID;
  if (gst_caps_is_any (caps)) {
    gst_caps_unref (caps);
    return OV_VIDEO_QUALITY_INVALID;
  }

  quality = ov_structure_to_video_quality (gst_caps_get_structure (caps, 0));
  gst_caps_unref (caps);

  return quality;
}

/* Returns FALSE if video caps haven't been negotiated yet */
gboolean
ov_local_peer_set_video_quality (OvLocalPeer * local, OvVideoQuality quality)
{
  gint ii, len;
  GstCaps *matching, *normalized;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  if (priv->send_vcaps == NULL)
    return FALSE;

  if (quality == ov_local_peer_get_video_quality (local))
    /* Nothing to do */
    return TRUE;

  normalized = gst_caps_normalize (gst_caps_copy (priv->send_vcaps));

  len = gst_caps_get_size (normalized);

  for (ii = 0; ii < len; ii++) {
    GstStructure *s;
    OvVideoQuality nthquality;
    
    s = gst_caps_get_structure (normalized, ii);
    
    nthquality = ov_structure_to_video_quality (s);

    if (nthquality == quality) {
      matching = gst_caps_new_full (gst_structure_copy (s), NULL);
      matching = gst_caps_fixate (matching);
      ov_local_peer_set_transmit_video_caps (local, matching);
      gst_caps_unref (matching);
      gst_caps_unref (normalized);
      return TRUE;
    }
  }
  
  gst_caps_unref (normalized);
  return FALSE;
}

static gboolean
ov_local_peer_discovery_send (OvLocalPeer * local, GError ** error)
{
  gboolean ret;

  /* Broadcast to the entire subnet to find listening peers */
  ret = ov_discovery_send_multicast_discover (local, NULL, error);
  if (!ret)
    return ret;

  g_signal_emit_by_name (local, "discovery-sent");

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
    GST_DEBUG ("Error reading discovery reply: %s", error->message);
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
    GST_TRACE ("Invalid discovery reply: %u", msg.type);
    ret = G_SOURCE_CONTINUE;
    goto out;
  }

  d = ov_discovered_peer_new (G_INET_SOCKET_ADDRESS (from));
  g_object_get (d, "address-string", &addr_s, NULL);
  GST_TRACE ("Found a remote peer: %s; emitting signal", addr_s);
  g_free (addr_s);

  g_signal_emit_by_name (local, "peer-discovered", d);
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
  if (ret == GST_STATE_CHANGE_FAILURE)
    GST_ERROR ("Unable to begin transmitting; state change failed");
  else
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

void
ov_local_peer_remove_remote (OvLocalPeer * local, OvRemotePeer * remote)
{
  OvLocalPeerPrivate *local_priv;

  ov_local_peer_lock (local);
  local_priv = ov_local_peer_get_private (local);
  /* Remove from the peers list first so nothing else tries to use it */
  g_ptr_array_remove (local_priv->remote_peers, remote);
  ov_local_peer_unlock (local);

  ov_remote_peer_remove_not_array (remote);
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

  /* Empty capsfilter; we'll set the caps on this later with
   * ov_local_peer_set_transmit_video_caps(). The rest of the transmit pipeline
   * is setup in ov_local_peer_call_start() once we have negotiated caps */
  priv->transmit_vcapsfilter =
    gst_element_factory_make ("capsfilter", "video-transmit-caps");
  /* We own a ref to this element */
  g_object_ref_sink (priv->transmit_vcapsfilter);

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

static gboolean
ov_local_peer_check_timeouts (OvLocalPeer * local)
{
  guint ii;
  gint64 current_time;
  GPtrArray *remotes;
  GPtrArray *timedout = g_ptr_array_new ();
  GPtrArray *removed = g_ptr_array_new ();
  gboolean all_remotes_gone = FALSE;

  GST_DEBUG ("Checking for remote timeouts...");

  ov_local_peer_lock (local);

  if (ov_local_peer_get_state (local) == OV_LOCAL_STATE_STOPPED) {
    GST_DEBUG ("Already stopped; skipping timeout check");
    return G_SOURCE_REMOVE;
  }

  current_time = g_get_monotonic_time ();
  remotes = ov_local_peer_get_remotes (local);
  for (ii = 0; ii < remotes->len; ii++) {
    OvRemotePeer *remote = g_ptr_array_index (remotes, ii);
    if ((current_time - remote->last_seen) >
        OV_REMOTE_PEER_TIMEOUT_SECONDS * G_USEC_PER_SEC)
      g_ptr_array_add (timedout, remote);
  }

  if (timedout->len == remotes->len)
    all_remotes_gone = TRUE;

  for (ii = 0; ii < timedout->len; ii++) {
    OvRemotePeer *remote = g_ptr_array_index (timedout, ii);
    OvPeer *peer = ov_peer_new (remote->addr);
    GST_DEBUG ("Remote peer %s timed out, removing...", remote->addr_s);
    ov_local_peer_remove_remote (local, remote);
    g_ptr_array_add (removed, peer);
  }

  ov_local_peer_unlock (local);
  g_ptr_array_free (timedout, TRUE);

  for (ii = 0; ii < removed->len; ii++) {
    OvPeer *peer = g_ptr_array_index (removed, ii);
    g_signal_emit_by_name (local, "call-remote-gone", peer, TRUE);
    g_object_unref (peer);
  }
  g_ptr_array_free (removed, TRUE);

  if (!all_remotes_gone)
    return G_SOURCE_CONTINUE;

  g_signal_emit_by_name (local, "call-all-remotes-gone");
  return G_SOURCE_REMOVE;
}

gboolean
ov_local_peer_call_start (OvLocalPeer * local)
{
  guint index;
  gboolean res;
  gint current_time;
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

  /* We can only setup the transmit pipeline once we know whether we will be
   * transmitting H264 or JPEG */
  res = ov_local_peer_setup_transmit_pipeline (local);
  g_assert (res);
  res = ov_local_peer_begin_transmit (local);
  g_assert (res);

  current_time = g_get_monotonic_time ();
  for (index = 0; index < priv->remote_peers->len; index++) {
    remote = g_ptr_array_index (priv->remote_peers, index);
    remote->last_seen = current_time;

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

  priv->remotes_timeout_source =
    g_timeout_source_new_seconds (OV_REMOTE_PEER_TIMEOUT_SECONDS/2);
  g_source_set_callback (priv->remotes_timeout_source,
      (GSourceFunc) ov_local_peer_check_timeouts, local, NULL);
  g_source_attach (priv->remotes_timeout_source, NULL);

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
