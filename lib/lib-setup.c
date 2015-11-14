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
#include "discovery.h"

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

GSocket *
one_video_get_socket_for_addr (const gchar * addr_s, guint port)
{
  GSocketAddress *sock_addr;
  GSocket *socket = NULL;
  GError *error = NULL;

  sock_addr = g_inet_socket_address_new_from_string (addr_s, port);

  socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, &error);
  if (!socket) {
    GST_ERROR ("Unable to create new socket: %s", error->message);
    goto err;
  }

  if (!g_socket_bind (socket, sock_addr, TRUE, &error)) {
    GST_ERROR ("Unable to create new socket: %s", error->message);
    goto err;
  }

out:
  g_object_unref (sock_addr);
  return socket;
err:
  g_clear_pointer (&socket, g_object_unref);
  g_error_free (error);
  goto out;
}

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
one_video_local_peer_setup_transmit_pipeline (OneVideoLocalPeer * local,
    GstDevice * video_device)
{
  GstBus *bus;
  GstCaps *video_caps, *raw_audio_caps;
  GstElement *asrc, *afilter, *aencode, *apay;
  GstElement *artpqueue, *asink, *artcpqueue, *artcpsink, *artcpsrc;
  GstElement *vsrc, *vfilter, *vqueue, *vqueue2, *vpay;
  GstElement *vrtpqueue, *vsink, *vrtcpqueue, *vrtcpsink, *vrtcpsrc;

  if (!(local->state & ONE_VIDEO_LOCAL_STATE_INITIALISED))
    return FALSE;

  if (local->transmit != NULL && GST_IS_PIPELINE (local->transmit)) {
    /* Wipe pre-existing transmit pipeline and recreate anew */
    gst_object_unref (local->transmit);
  }

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
  g_object_set (aencode, "frame-size", 10, NULL);
  apay = gst_element_factory_make ("rtpopuspay", NULL);
  /* Send RTP audio data */
  artpqueue = gst_element_factory_make ("queue", NULL);
  asink = gst_element_factory_make ("udpsink", "asend_rtp_sink");
  /* Send RTCP SR for audio (same packets for all peers) */
  artcpqueue = gst_element_factory_make ("queue", NULL);
  artcpsink = gst_element_factory_make ("udpsink", "asend_rtcp_sink");
  g_object_set (artcpsink, "sync", FALSE, "async", FALSE, NULL);
  /* Recv RTCP RR for audio (same port for all peers) */
  artcpsrc = gst_element_factory_make ("udpsrc", "arecv_rtcp_src");

  /* FIXME: We want to support JPEG, keyframe-only H264, and video/x-raw.
   * FIXME: Select the best format based on formats available on the camera */
  if (video_device == NULL) {
    vsrc = gst_element_factory_make ("videotestsrc", NULL);
    g_object_set (vsrc, "is-live", TRUE, NULL);
    vfilter = gst_element_factory_make ("capsfilter", "video-transmit-caps");
    video_caps = gst_caps_from_string ("video/x-raw, " VIDEO_CAPS_STR);
    g_object_set (vfilter, "caps", video_caps, NULL);
    gst_caps_unref (video_caps);
    vqueue = gst_element_factory_make ("queue", "v4l2-queue");
  } else {
    vsrc = gst_device_create_element (video_device, NULL);
    vfilter = gst_element_factory_make ("capsfilter", "video-transmit-caps");
    /* These have already been fixated */
    g_object_set (vfilter, "caps", local->priv->send_vcaps, NULL);
    vqueue = gst_element_factory_make ("jpegdec", "v4l2-queue");
  }
  vqueue2 = gst_element_factory_make ("jpegenc", NULL);
  g_object_set (vqueue2, "quality", 30, NULL);
  vpay = gst_element_factory_make ("rtpjpegpay", NULL);
  /* Send RTP video data */
  vrtpqueue = gst_element_factory_make ("queue", NULL);
  vsink = gst_element_factory_make ("udpsink", "vsend_rtp_sink");
  /* Send RTCP SR for video (same packets for all peers) */
  vrtcpqueue = gst_element_factory_make ("queue", NULL);
  vrtcpsink = gst_element_factory_make ("udpsink", "vsend_rtcp_sink");
  g_object_set (vrtcpsink, "sync", FALSE, "async", FALSE, NULL);
  /* Recv RTCP RR for video (same port for all peers) */
  vrtcpsrc = gst_element_factory_make ("udpsrc", "vrecv_rtcp_src");

  gst_bin_add_many (GST_BIN (local->transmit), local->priv->rtpbin, asrc,
      afilter, aencode, apay, artpqueue, asink, artcpqueue, artcpsink, artcpsrc,
      vsrc, vfilter, vqueue, vqueue2, vpay, vrtpqueue, vsink, vrtcpqueue,
      vrtcpsink, vrtcpsrc, NULL);

  /* Link audio branch */
  g_assert (gst_element_link_many (asrc, afilter, aencode, apay, NULL));
  g_assert (gst_element_link (artcpqueue, artcpsink));
  local->priv->asend_rtcp_sink = artcpsink;
  g_assert (gst_element_link (artpqueue, asink));
  local->priv->asend_rtp_sink = asink;
  local->priv->arecv_rtcp_src = artcpsrc;

  /* Send RTP data */
  g_assert (gst_element_link_pads (apay, "src", local->priv->rtpbin,
        "send_rtp_sink_0"));
  g_assert (gst_element_link_pads (local->priv->rtpbin, "send_rtp_src_0",
        artpqueue, "sink"));

  /* Send RTCP SR */
  g_assert (gst_element_link_pads (local->priv->rtpbin, "send_rtcp_src_0",
        artcpqueue, "sink"));

  /* Recv RTCP RR */
  g_assert (gst_element_link_pads (artcpsrc, "src", local->priv->rtpbin,
        "recv_rtcp_sink_0"));

  /* Link video branch */
  g_assert (gst_element_link_many (vsrc, vfilter, vqueue, vqueue2, vpay, NULL));
  g_assert (gst_element_link (vrtcpqueue, vrtcpsink));
  local->priv->vsend_rtcp_sink = vrtcpsink;
  g_assert (gst_element_link (vrtpqueue, vsink));
  local->priv->vsend_rtp_sink = vsink;
  local->priv->vrecv_rtcp_src = vrtcpsrc;

  /* Send RTP data */
  g_assert (gst_element_link_pads (vpay, "src", local->priv->rtpbin,
        "send_rtp_sink_1"));
  g_assert (gst_element_link_pads (local->priv->rtpbin, "send_rtp_src_1",
        vrtpqueue, "sink"));

  /* Send RTCP SR */
  g_assert (gst_element_link_pads (local->priv->rtpbin, "send_rtcp_src_1",
        vrtcpqueue, "sink"));

  /* Recv RTCP RR */
  g_assert (gst_element_link_pads (vrtcpsrc, "src", local->priv->rtpbin,
        "recv_rtcp_sink_1"));

  /* All done */

  /* Use the system clock and explicitly reset the base/start times to ensure
   * that all the pipelines started by us have the same base/start times */
  gst_pipeline_use_clock (GST_PIPELINE (local->transmit),
      gst_system_clock_obtain ());
  gst_element_set_base_time (local->transmit, 0);

  bus = gst_pipeline_get_bus (GST_PIPELINE (local->transmit));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_transmit_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to transmit to remote peers");

  return TRUE;
}

gboolean
one_video_local_peer_setup_tcp_comms (OneVideoLocalPeer * local)
{
  gboolean ret;
  GInetAddress *multicast_group;
  GSocketAddress *multicast_addr;
  GError *error = NULL;

  /*-- Listen for incoming TCP connections --*/

  /* Threaded socket service since we use blocking TCP network reads
   * TODO: Use threads equal to number of remote peers? To ensure that peers
   * never wait while communicating. */
  local->priv->tcp_server = g_threaded_socket_service_new (10);

  ret = g_socket_listener_add_address (
      G_SOCKET_LISTENER (local->priv->tcp_server),
      G_SOCKET_ADDRESS (local->addr), G_SOCKET_TYPE_STREAM,
      G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error);
  if (!ret) {
    GST_ERROR ("Unable to setup TCP server (%s): %s", local->addr_s,
        error->message);
    g_error_free (error);
    goto out_nofree;
  }

  g_signal_connect (local->priv->tcp_server, "run",
      G_CALLBACK (on_incoming_peer_tcp_connection), local);

  g_socket_service_start (local->priv->tcp_server);
  GST_DEBUG ("Listening for incoming TCP connections");

  /*-- Listen for incoming UDP messages (multicast and unicast) --*/
  local->priv->mc_socket = g_socket_new (G_SOCKET_FAMILY_IPV4,
      G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  multicast_group = g_inet_address_new_from_string (ONE_VIDEO_MULTICAST_GROUP);
  multicast_addr = g_inet_socket_address_new (multicast_group,
      g_inet_socket_address_get_port (local->addr));
  ret = g_socket_bind (local->priv->mc_socket, multicast_addr, TRUE, &error);
  if (!ret) {
    gchar *name =
      g_inet_address_to_string (g_inet_socket_address_get_address (local->addr));
    GST_ERROR ("Unable to bind to multicast addr/port (%s:%u): %s", name,
        g_inet_socket_address_get_port (local->addr), error->message);
    g_error_free (error);
    goto out;
  }

  g_socket_set_broadcast (local->priv->mc_socket, TRUE);

  ret = g_socket_join_multicast_group (local->priv->mc_socket, multicast_group,
      FALSE, NULL, &error);
  if (!ret) {
    GST_ERROR ("Unable to join multicast group %s: %s",
        ONE_VIDEO_MULTICAST_GROUP, error->message);
    g_error_free (error);
    goto out;
  }

  /* Take ownership of the socket */
  g_object_ref (local->priv->mc_socket);
  /* Attach an event source to the default main context */
  local->priv->mc_socket_source =
    g_socket_create_source (local->priv->mc_socket, G_IO_IN, NULL);
  g_source_set_callback (local->priv->mc_socket_source,
      (GSourceFunc) on_incoming_udp_message, local, NULL);
  g_source_attach (local->priv->mc_socket_source, NULL);
  GST_DEBUG ("Listening for incoming UDP messages");

out:
  g_object_unref (local->priv->mc_socket);
  g_object_unref (multicast_group);
  g_object_unref (multicast_addr);
out_nofree:
  return ret;
}

/*-- REMOTE PEER SETUP --*/
static void
rtpbin_pad_added (GstElement * rtpbin, GstPad * srcpad,
    OneVideoRemotePeer * remote)
{
  GstPad *sinkpad;
  GstElement *depay;
  gchar *name = gst_pad_get_name (srcpad);
  guint len = G_N_ELEMENTS ("recv_rtp_src_");

  /* Match the session number to the correct branch (audio or video)
   * The session number is the first %u in the pad name of the form
   * 'recv_rtp_src_%u_%u_%u' */ 
  if (name[len-1] == '0')
    depay = remote->priv->adepay;
  else if (name[len-1] == '1')
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
  GSocket *socket;
  GstElement *rtpbin;
  GstElement *asrc, *artcpsrc, *adecode, *asink, *artcpsink;
  GstElement *vsrc, *vrtcpsrc, *vdecode, *vconvert, *vsink, *vrtcpsink;
  gchar *local_addr_s, *remote_addr_s;
  GstCaps *rtpcaps;

  g_assert (remote->priv->recv_acaps != NULL &&
      remote->priv->recv_vcaps != NULL && remote->priv->recv_ports[0] > 0 &&
      remote->priv->recv_ports[1] > 0 && remote->priv->recv_ports[2] > 0 &&
      remote->priv->recv_ports[3] > 0);

  local_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (local->addr));
  remote_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (remote->addr));

  /* Setup pipeline (remote->receive) to recv & decode from a remote peer */

  rtpbin = gst_element_factory_make ("rtpbin", "recv-rtpbin-%u");
  g_object_set (rtpbin, "latency", RTP_DEFAULT_LATENCY_MS, "drop-on-latency",
      TRUE, NULL);

  /* TODO: Both audio and video should be optional */

  /* Recv RTP audio data */
  socket = one_video_get_socket_for_addr (local_addr_s,
      remote->priv->recv_ports[0]);
  asrc = gst_element_factory_make ("udpsrc", "arecv_rtp_src-%u");
  /* We always use the same caps for sending audio */
  rtpcaps = gst_caps_from_string (RTP_ALL_AUDIO_CAPS_STR);
  g_object_set (asrc, "socket", socket, "caps", rtpcaps, NULL);
  gst_caps_unref (rtpcaps);
  g_object_unref (socket);
  remote->priv->adepay = gst_element_factory_make ("rtpopusdepay", NULL);
  adecode = gst_element_factory_make ("opusdec", NULL);
  asink = gst_element_factory_make ("proxysink", "audio-proxysink-%u");
  g_assert (asink != NULL);
  /* Recv RTCP SR for audio */
  socket = one_video_get_socket_for_addr (local_addr_s,
      remote->priv->recv_ports[1]);
  artcpsrc = gst_element_factory_make ("udpsrc", "arecv_rtcp_src-%u");
  g_object_set (artcpsrc, "socket", socket, NULL);
  /* Send RTCP RR for audio using the same port as recv RTCP SR for audio
   * NOTE: on the other end of this connection, the same port that we send
   * these RTCP RRs to is also used to send us the RTCP SR packets that we
   * receive above */
  artcpsink = gst_element_factory_make ("udpsink", "asend_rtcp_sink-%u");
  g_object_set (artcpsink, "socket", socket, "sync", FALSE, "async", FALSE,
      /* Remote peer transport address */
      "host", remote_addr_s, "port", remote->priv->send_ports[2], NULL);
  g_object_unref (socket);

  /* Recv RTP video data */
  socket = one_video_get_socket_for_addr (local_addr_s,
      remote->priv->recv_ports[2]);
  vsrc = gst_element_factory_make ("udpsrc", "vrecv_rtp_src-%u");
  /* The depayloader will detect the height/width/framerate on the fly
   * This allows us to change that without communicating new caps
   * TODO: This hard-codes JPEG right now. Choose based on priv->recv_vcaps. */
  rtpcaps = gst_caps_from_string (RTP_JPEG_VIDEO_CAPS_STR);
  g_object_set (vsrc, "socket", socket, "caps", rtpcaps, NULL);
  gst_caps_unref (rtpcaps);
  g_object_unref (socket);
  remote->priv->vdepay = gst_element_factory_make ("rtpjpegdepay", NULL);
  vdecode = gst_element_factory_make ("jpegdec", NULL);
  /* We need this despite setting caps everywhere because the jpeg might have
   * been encoded by the webcam, in which case it could be in any raw format */
  vconvert = gst_element_factory_make ("videoconvert", NULL);
  vsink = gst_element_factory_make ("proxysink", "video-proxysink-%u");
  g_assert (vsink != NULL);
  /* Recv RTCP SR for video */
  socket = one_video_get_socket_for_addr (local_addr_s,
      remote->priv->recv_ports[3]);
  vrtcpsrc = gst_element_factory_make ("udpsrc", "vrecv_rtcp_src-%u");
  g_object_set (vrtcpsrc, "socket", socket, NULL);
  /* Send RTCP RR for video using the same port as recv RTCP SR for video
   * NOTE: on the other end of this connection, the same port that we send
   * these RTCP RRs to is also used to send us the RTCP SR packets that we
   * receive above */
  vrtcpsink = gst_element_factory_make ("udpsink", "vsend_rtcp_sink-%u");
  g_object_set (vrtcpsink, "socket", socket, "sync", FALSE, "async", FALSE,
      /* Remote peer transport address */
      "host", remote_addr_s, "port", remote->priv->send_ports[5], NULL);
  g_object_unref (socket);

  gst_bin_add_many (GST_BIN (remote->receive), rtpbin,
      asrc, remote->priv->adepay, adecode, asink, artcpsink, artcpsrc,
      vsrc, remote->priv->vdepay, vdecode, vconvert, vsink, vrtcpsink, vrtcpsrc,
      NULL);

  /* Link audio branch via rtpbin */
  g_assert (gst_element_link_many (remote->priv->adepay, adecode, asink, NULL));

  /* Recv audio RTP and send to rtpbin */
  g_assert (gst_element_link_pads (asrc, "src", rtpbin, "recv_rtp_sink_0"));

  /* Recv audio RTCP SR etc and send to rtpbin */
  g_assert (gst_element_link_pads (artcpsrc, "src", rtpbin,
        "recv_rtcp_sink_0"));

  /* Send audio RTCP RR etc from rtpbin */
  g_assert (gst_element_link_pads (rtpbin, "send_rtcp_src_0", artcpsink,
        "sink"));

  /* Link video branch via rtpbin */
  g_assert (gst_element_link_many (remote->priv->vdepay, vdecode, vconvert,
        vsink, NULL));

  /* Recv video RTP and send to rtpbin */
  g_assert (gst_element_link_pads (vsrc, "src", rtpbin, "recv_rtp_sink_1"));

  /* Recv video RTCP SR etc and send to rtpbin */
  g_assert (gst_element_link_pads (vrtcpsrc, "src", rtpbin,
        "recv_rtcp_sink_1"));

  /* Send video RTCP RR etc from rtpbin */
  g_assert (gst_element_link_pads (rtpbin, "send_rtcp_src_1", vrtcpsink,
        "sink"));

  /* When recv_rtp_src_%u_%u_%u pads corresponding to the above recv_rtp_sink_%u
   * sinkpads are added when the pipeline pre-rolls, 'pad-added' will be called
   * and we'll finish linking the pipeline */
  g_signal_connect (rtpbin, "pad-added", G_CALLBACK (rtpbin_pad_added), remote);

  /* This is what exposes video/audio data from this remote peer */
  remote->priv->audio_proxysink = asink;
  remote->priv->video_proxysink = vsink;

  GST_DEBUG ("Setup pipeline to receive from remote");
  g_free (remote_addr_s);
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
