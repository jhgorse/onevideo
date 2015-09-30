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
#include "lib-setup.h"
#include "incoming.h"
#include "comms.h"

#include <string.h>

void
one_video_on_gst_bus_error (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  char *tmp = NULL;
  GError *error = NULL;
  const char *name = GST_OBJECT_NAME (msg->src);

  gst_message_parse_error (msg, &error, &tmp);
  g_printerr ("ERROR from element %s: %s\n",
              name, error->message);
  g_printerr ("Debug info: %s\n", tmp);
  g_error_free (error);
  g_free (tmp);
}

#define on_local_transmit_error one_video_on_gst_bus_error
#define on_local_playback_error one_video_on_gst_bus_error

/*-- LOCAL PEER SETUP --*/
gboolean
one_video_local_peer_setup_playback_pipeline (OneVideoLocalPeer * local)
{
  GstBus *bus;

  if (local->playback != NULL && GST_IS_PIPELINE (local->playback))
    /* Already setup */
    return TRUE;

  /* Setup audio bits */
  local->playback = gst_pipeline_new ("playback-%u");
  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (local->playback), FALSE);
  local->priv->audiomixer = gst_element_factory_make ("audiomixer", NULL);
  local->priv->audiosink = gst_element_factory_make ("pulsesink", NULL);
  /* These values give the lowest audio latency with the least chance of audio
   * artefacting. Setting buffer-time less than 50ms gives audio artefacts. */
  g_object_set (local->priv->audiosink, "buffer-time", 50000, NULL);
    
  /* FIXME: If there's no audio, this pipeline will mess up while going from
   * NULL -> PLAYING -> NULL -> PLAYING because of async state change bugs in
   * basesink. Fix this by only plugging a sink if audio is present. */
  gst_bin_add_many (GST_BIN (local->playback), local->priv->audiomixer,
      local->priv->audiosink, NULL);
  g_assert (gst_element_link_many (local->priv->audiomixer,
        local->priv->audiosink, NULL));

  /* Video bits are setup by each local */

  /* Use the system clock and explicitly reset the base/start times to ensure
   * that all the pipelines started by us have the same base/start times */
  gst_pipeline_use_clock (GST_PIPELINE (local->playback),
      gst_system_clock_obtain());
  gst_element_set_base_time (local->playback, 0);

  bus = gst_pipeline_get_bus (GST_PIPELINE (local->playback));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_playback_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to playback remote peers");

  return TRUE;
}

gboolean
one_video_local_peer_setup_transmit_pipeline (OneVideoLocalPeer * local)
{
  GstBus *bus;
  GstCaps *jpeg_video_caps, *raw_audio_caps;
  GstElement *asrc, *afilter, *aencode, *apay, *apaycaps, *asink, *artcpsink;
  GstElement *vsrc, *vfilter, *vqueue, *vpay, *vpaycaps, *vsink, *vrtcpsink;
  GstPad *srcpad, *sinkpad;

  if (local->transmit != NULL && GST_IS_PIPELINE (local->transmit))
    /* Already setup */
    return TRUE;

  local->transmit = gst_pipeline_new ("transmit-pipeline");
  local->priv->rtpbin = gst_element_factory_make ("rtpbin", "transmit-rtpbin");
  g_object_set (local->priv->rtpbin, "latency", RTP_DEFAULT_LATENCY_MS, NULL);

  asrc = gst_element_factory_make ("pulsesrc", NULL);
  /* latency-time to 5 ms, we use the system clock */
  g_object_set (asrc, "latency-time", 5000, "provide-clock", FALSE, NULL);
  afilter = gst_element_factory_make ("capsfilter", "audio-transmit-caps");
  raw_audio_caps = gst_caps_from_string ("audio/x-raw, " AUDIO_CAPS_STR);
  g_object_set (afilter, "caps", raw_audio_caps, NULL);
  gst_caps_unref (raw_audio_caps);
  aencode = gst_element_factory_make ("opusenc", NULL);
  g_object_set (aencode, "frame-size", 2, NULL);
  apay = gst_element_factory_make ("rtpopuspay", NULL);
  apaycaps = gst_element_factory_make ("capsfilter", "audio-rtp-transmit-caps");
  g_object_set (apaycaps, "caps", local->priv->send_acaps, NULL);
  asink = gst_element_factory_make ("udpsink", "adata-transmit-udpsink");
  artcpsink = gst_element_factory_make ("udpsink", "artcp-transmit-udpsink");

  /* FIXME: Use GstDevice* instead of a device path string
   * FIXME: We want to support JPEG, keyframe-only H264, and video/x-raw.
   * FIXME: Select the best format based on formats available on the camera */
  vsrc = gst_element_factory_make ("v4l2src", NULL);
  if (local->priv->v4l2_path != NULL)
    g_object_set (vsrc, "device", local->priv->v4l2_path, NULL);
  vfilter = gst_element_factory_make ("capsfilter", "video-transmit-caps");
  jpeg_video_caps = gst_caps_from_string ("image/jpeg, " VIDEO_CAPS_STR);
  g_object_set (vfilter, "caps", jpeg_video_caps, NULL);
  gst_caps_unref (jpeg_video_caps);
  vqueue = gst_element_factory_make ("queue", "v4l2-queue");
  vpay = gst_element_factory_make ("rtpjpegpay", NULL);
  vpaycaps = gst_element_factory_make ("capsfilter", "video-rtp-transmit-caps");
  g_object_set (vpaycaps, "caps", local->priv->send_vcaps, NULL);
  vsink = gst_element_factory_make ("udpsink", "vdata-transmit-udpsink");
  vrtcpsink = gst_element_factory_make ("udpsink", "vrtcp-transmit-udpsink");

  gst_bin_add_many (GST_BIN (local->transmit), local->priv->rtpbin, asrc,
      afilter, aencode, apay, apaycaps, asink, artcpsink, vsrc, vfilter, vqueue,
      vpay, vpaycaps, vsink, vrtcpsink, NULL);

  /* Link audio branch */
  g_assert (gst_element_link_many (asrc, afilter, aencode, apay, apaycaps,
        NULL));

  srcpad = gst_element_get_static_pad (apaycaps, "src");
  sinkpad = gst_element_get_request_pad (local->priv->rtpbin, "send_rtp_sink_0");
  gst_pad_link (srcpad, sinkpad);
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  srcpad = gst_element_get_static_pad (local->priv->rtpbin, "send_rtp_src_0");
  sinkpad = gst_element_get_static_pad (asink, "sink");
  gst_pad_link (srcpad, sinkpad);
  local->priv->audpsink = asink;
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  srcpad = gst_element_get_request_pad (local->priv->rtpbin, "send_rtcp_src_0");
  sinkpad = gst_element_get_static_pad (artcpsink, "sink");
  gst_pad_link (srcpad, sinkpad);
  local->priv->artcpudpsink = artcpsink;
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  /* Link video branch */
  g_assert (gst_element_link_many (vsrc, vfilter, vqueue, vpay, vpaycaps,
        NULL));

  srcpad = gst_element_get_static_pad (vpaycaps, "src");
  sinkpad = gst_element_get_request_pad (local->priv->rtpbin, "send_rtp_sink_1");
  gst_pad_link (srcpad, sinkpad);
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  srcpad = gst_element_get_static_pad (local->priv->rtpbin, "send_rtp_src_1");
  sinkpad = gst_element_get_static_pad (vsink, "sink");
  gst_pad_link (srcpad, sinkpad);
  local->priv->vudpsink = vsink;
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  srcpad = gst_element_get_request_pad (local->priv->rtpbin, "send_rtcp_src_1");
  sinkpad = gst_element_get_static_pad (vrtcpsink, "sink");
  gst_pad_link (srcpad, sinkpad);
  local->priv->vrtcpudpsink = vrtcpsink;
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  /* All done */

  /* Use the system clock and explicitly reset the base/start times to ensure
   * that all the pipelines started by us have the same base/start times */
  gst_pipeline_use_clock (GST_PIPELINE (local->transmit),
      gst_system_clock_obtain());
  gst_element_set_base_time (local->transmit, 0);

  bus = gst_pipeline_get_bus (GST_PIPELINE (local->transmit));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_transmit_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to transmit to remote local");

  return TRUE;
}

gboolean
one_video_local_peer_setup_tcp_comms (OneVideoLocalPeer * local)
{
  gboolean ret;
  GError *error = NULL;

  ret = g_socket_listener_add_address (
      G_SOCKET_LISTENER (local->priv->tcp_server),
      G_SOCKET_ADDRESS (local->addr), G_SOCKET_TYPE_STREAM,
      G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error);
  if (!ret) {
    gchar *name =
      g_inet_address_to_string (g_inet_socket_address_get_address (local->addr));
    GST_ERROR ("Unable to setup TCP server (%s:%u): %s", name,
        g_inet_socket_address_get_port (local->addr), error->message);
    g_free (name);
    return FALSE;
  }

  g_signal_connect (local->priv->tcp_server, "run",
      G_CALLBACK (on_incoming_peer_tcp_connection), local);

  g_socket_service_start (local->priv->tcp_server);

  return TRUE;
}

/*-- REMOTE PEER SETUP --*/
static void
rtpbin_pad_added (GstElement * rtpbin, GstPad * srcpad,
    OneVideoRemotePeer * remote)
{
  gchar *name;
  GstPad *sinkpad;
  GstElement *depay;

  name = gst_pad_get_name (srcpad);
  /* Match the session number to the correct branch (audio or video) */ 
  if (name[13] == '0')
    depay = remote->priv->adepay;
  else if (name[13] == '1')
    depay = remote->priv->vdepay;
  else
    /* We only have two streams with known session numbers */
    g_assert_not_reached ();
  g_free (name);

  sinkpad = gst_element_get_static_pad (depay, "sink");
  g_assert (gst_pad_link (srcpad, sinkpad) == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
}

void
one_video_local_peer_setup_remote_receive (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  gchar *local_addr_s;
  GstElement *asrc, *artcpsrc, *adecode, *asink;
  GstElement *vsrc, *vrtcpsrc, *vdecode, *vconvert, *vsink;
  GstPad *srcpad, *sinkpad;

  g_assert (remote->priv->recv_acaps != NULL &&
      remote->priv->recv_vcaps != NULL && remote->priv->recv_ports[0] > 0 &&
      remote->priv->recv_ports[1] > 0 && remote->priv->recv_ports[2] > 0 &&
      remote->priv->recv_ports[3] > 0);

  local_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (local->addr));

  /* Setup pipeline (remote->receive) to recv & decode from a remote peer */

  remote->priv->rtpbin = gst_element_factory_make ("rtpbin", "recv-rtpbin-%u");
  g_object_set (remote->priv->rtpbin, "latency", RTP_DEFAULT_LATENCY_MS,
      "drop-on-latency", TRUE, NULL);

  /* FIXME: Fetch and set udpsrc caps using SDP over UDP
   * FIXME: Both audio and video should be optional once we have negotiation */
  asrc = gst_element_factory_make ("udpsrc", "adata-recv-udpsrc-%u");
  g_object_set (asrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[0], "caps", remote->priv->recv_acaps, NULL);
  artcpsrc = gst_element_factory_make ("udpsrc", "artcp-recv-udpsrc-%u");
  g_object_set (artcpsrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[1], NULL);
  remote->priv->adepay = gst_element_factory_make ("rtpopusdepay", NULL);
  adecode = gst_element_factory_make ("opusdec", NULL);
  asink = gst_element_factory_make ("proxysink", "audio-proxysink-%u");
  g_assert (asink != NULL);

  vsrc = gst_element_factory_make ("udpsrc", "vdata-recv-udpsrc-%u");
  g_object_set (vsrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[2], "caps", remote->priv->recv_vcaps, NULL);
  vrtcpsrc = gst_element_factory_make ("udpsrc", "vrtcp-recv-udpsrc-%u");
  g_object_set (vrtcpsrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[3], NULL);
  remote->priv->vdepay = gst_element_factory_make ("rtpjpegdepay", NULL);
  vdecode = gst_element_factory_make ("jpegdec", NULL);
  /* We need this despite setting caps everywhere because the jpeg might have
   * been encoded by the webcam, in which case it could be in any raw format */
  vconvert = gst_element_factory_make ("videoconvert", NULL);
  vsink = gst_element_factory_make ("proxysink", "video-proxysink-%u");
  g_assert (vsink != NULL);

  gst_bin_add_many (GST_BIN (remote->receive), asrc, remote->priv->adepay,
      adecode, asink, vsrc, remote->priv->vdepay, vdecode, vconvert, vsink,
      artcpsrc, vrtcpsrc, remote->priv->rtpbin, NULL);

  /* Link audio branch via rtpbin */
  g_assert (gst_element_link_many (remote->priv->adepay, adecode, asink, NULL));

  srcpad = gst_element_get_static_pad (asrc, "src");
  sinkpad =
    gst_element_get_request_pad (remote->priv->rtpbin, "recv_rtp_sink_0");
  g_assert (gst_pad_link (srcpad, sinkpad) == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  srcpad = gst_element_get_static_pad (artcpsrc, "src");
  sinkpad =
    gst_element_get_request_pad (remote->priv->rtpbin, "recv_rtcp_sink_0");
  g_assert (gst_pad_link (srcpad, sinkpad) == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  /* Link video branch via rtpbin */
  g_assert (gst_element_link_many (remote->priv->vdepay, vdecode, vconvert,
        vsink, NULL));

  srcpad = gst_element_get_static_pad (vsrc, "src");
  sinkpad =
    gst_element_get_request_pad (remote->priv->rtpbin, "recv_rtp_sink_1");
  g_assert (gst_pad_link (srcpad, sinkpad) == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  srcpad = gst_element_get_static_pad (vrtcpsrc, "src");
  sinkpad =
    gst_element_get_request_pad (remote->priv->rtpbin, "recv_rtcp_sink_1");
  g_assert (gst_pad_link (srcpad, sinkpad) == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  /* When recv_rtp_src_%u_%u_%u pads corresponding to the above recv_rtp_sink_%u
   * sinkpads are added when the pipeline pre-rolls, 'pad-added' will be called
   * and we'll finish linking the pipeline */
  g_signal_connect (remote->priv->rtpbin, "pad-added",
      G_CALLBACK (rtpbin_pad_added), remote);

  /* This is what exposes video/audio data from this remote peer */
  remote->priv->audio_proxysink = asink;
  remote->priv->video_proxysink = vsink;

  GST_DEBUG ("Setup pipeline to receive from remote");
  g_free (local_addr_s);
}

void
one_video_local_peer_setup_remote_playback (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  GstPad *ghostpad, *srcpad, *sinkpad;
  GstPadLinkReturn ret;

  /* Setup pipeline (local->playback) to aggregate audio from all remote peers
   * to audiomixer and then render using the provided audio sink
   *  [ proxysrc ! audiomixer ] */
  if (remote->priv->audio_proxysink) {
    remote->priv->audio_proxysrc =
      gst_element_factory_make ("proxysrc", "audio-proxysrc-%u");
    g_assert (remote->priv->audio_proxysrc != NULL);

    /* Link the two pipelines */
    g_object_set (remote->priv->audio_proxysrc, "proxysink",
        remote->priv->audio_proxysink, NULL);

    sinkpad = gst_element_get_request_pad (local->priv->audiomixer, "sink_%u");

    gst_bin_add_many (GST_BIN (remote->priv->aplayback),
        remote->priv->audio_proxysrc, NULL);
    g_assert (gst_bin_add (GST_BIN (local->playback), remote->priv->aplayback));

    srcpad = gst_element_get_static_pad (remote->priv->audio_proxysrc, "src");
    ghostpad = gst_ghost_pad_new ("audiopad", srcpad);
    g_assert (gst_pad_set_active (ghostpad, TRUE));
    g_assert (gst_element_add_pad (remote->priv->aplayback, ghostpad));
    gst_object_unref (srcpad);

    ret = gst_pad_link (ghostpad, sinkpad);
    g_assert (ret == GST_PAD_LINK_OK);
    gst_object_unref (sinkpad);
  }

  /* Setup pipeline (local->playback) to render video from each local to the
   * provided video sink */
  if (remote->priv->video_proxysink) {
    GstElement *sink;

    remote->priv->video_proxysrc = 
      gst_element_factory_make ("proxysrc", "video-proxysrc-%u");
    g_assert (remote->priv->video_proxysrc != NULL);

    /* Link the two pipelines */
    g_object_set (remote->priv->video_proxysrc, "proxysink",
        remote->priv->video_proxysink, NULL);

    sink = gst_element_factory_make ("xvimagesink", NULL);
    gst_bin_add_many (GST_BIN (remote->priv->vplayback),
        remote->priv->video_proxysrc, sink, NULL);
    g_assert (gst_bin_add (GST_BIN (local->playback), remote->priv->vplayback));
    g_assert (gst_element_link_many (remote->priv->video_proxysrc, sink, NULL));
  }

  GST_DEBUG ("Setup pipeline to playback remote local");
}
