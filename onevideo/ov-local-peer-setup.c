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
#include "incoming.h"
#include "discovery.h"
#include "ov-local-peer-priv.h"
#include "ov-local-peer-setup.h"

#include <string.h>

/* The default buffer size for kernel-side UDP send/recv buffers varies
 * between operating systems and installations. It's not unusual that
 * these are smaller than the size of a single jpeg from a HD webcam,
 * which is a problem, so try to make them larger if possible at all. */
#define OV_VIDEO_SEND_BUFSIZE (2 * 1024 * 1024)
#define OV_VIDEO_RECV_BUFSIZE (2 * 1024 * 1024)

void
ov_on_gst_bus_error (GstBus * bus, GstMessage * msg, gpointer user_data)
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

#define on_local_transmit_error ov_on_gst_bus_error
#define on_local_playback_error ov_on_gst_bus_error

GSocket *
ov_get_socket_for_addr (const gchar * addr_s, guint port)
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
ov_local_peer_setup_playback_pipeline (OvLocalPeer * local)
{
  GstBus *bus;
  gboolean ret;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  if (priv->playback != NULL && GST_IS_PIPELINE (priv->playback))
    /* Already setup */
    return TRUE;

  /* Setup audio bits */
  priv->playback = gst_object_ref_sink (gst_pipeline_new ("playback-%u"));
  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (priv->playback), FALSE);
  priv->audiomixer = gst_element_factory_make ("audiomixer", NULL);

#ifdef __linux__
  priv->audiosink = gst_element_factory_make ("pulsesink", NULL);
  /* These values give the lowest audio latency with the least chance of audio
   * artefacting with Pulseaudio on my machine. Setting buffer-time less than
   * 50ms gives audio artefacts. */
  g_object_set (priv->audiosink, "buffer-time", 50000, NULL);
#elif defined(__APPLE__) && defined(TARGET_OS_MAC)
  priv->audiosink = gst_element_factory_make ("osxaudiosink", NULL);
  /* These values give the lowest audio latency with the least chance of audio
   * artefacting with Pulseaudio on my machine. Setting buffer-time less than
   * 30ms gives audio artefacts. */
  g_object_set (priv->audiosink, "buffer-time", 30000, NULL);
#else
#error "Unsupported operating system"
#endif

  /* FIXME: If there's no audio, this pipeline will mess up while going from
   * NULL -> PLAYING -> NULL -> PLAYING because of async state change bugs in
   * basesink. Fix this by only plugging a sink if audio is present. */
  gst_bin_add_many (GST_BIN (priv->playback), priv->audiomixer,
      priv->audiosink, NULL);
  ret = gst_element_link_many (priv->audiomixer, priv->audiosink, NULL);
  g_assert (ret);

  /* Video bits are setup by each local */

  /* Use the system clock and explicitly reset the base/start times to ensure
   * that all the pipelines started by us have the same base/start times */
  gst_pipeline_use_clock (GST_PIPELINE (priv->playback),
      gst_system_clock_obtain());
  gst_element_set_base_time (priv->playback, 0);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->playback));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_playback_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to playback remote peers");

  return TRUE;
}

#if defined(__APPLE__) && defined(TARGET_OS_MAC)
static GstElement *
ov_pipeline_get_osxaudiosrcbin (const gchar * name)
{
  GstElement *src, *conv, *bin;
  GstPad *ghostpad, *srcpad;

  src = gst_element_factory_make ("osxaudiosrc", NULL);
  /* latency-time to 5 ms, we use the system clock */
  g_object_set (src, "latency-time", 5000, "provide-clock", FALSE, NULL);

  conv = gst_element_factory_make ("audioresample", NULL);

  bin = gst_bin_new (name);
  gst_bin_add_many (GST_BIN (bin), src, conv, NULL);

  gst_element_link (src, conv);

  srcpad = gst_element_get_static_pad (conv, "src");
  ghostpad = gst_ghost_pad_new ("src", srcpad);
  g_object_unref (srcpad);

  gst_pad_set_active (ghostpad, TRUE);
  gst_element_add_pad (bin, ghostpad);

  return bin;
}
#endif

gboolean
ov_local_peer_setup_transmit_pipeline (OvLocalPeer * local)
{
  GstBus *bus;
  GstCaps *vcaps, *raw_audio_caps;
  GstElement *asrc, *afilter, *aencode, *apay;
  GstElement *artpqueue, *asink, *artcpqueue, *artcpsink, *artcpsrc;
  GstElement *vsrc, *vfilter, *vqueue, *vpay;
  GstElement *vrtpqueue, *vsink, *vrtcpqueue, *vrtcpsink, *vrtcpsrc;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  gboolean ret;

  priv = ov_local_peer_get_private (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_STARTED) && !(state & OV_LOCAL_STATE_READY))
    return FALSE;

  if (priv->transmit != NULL && GST_IS_PIPELINE (priv->transmit)) {
    /* Wipe pre-existing transmit pipeline and recreate anew */
    gst_object_unref (priv->transmit);
  }

  priv->transmit = gst_object_ref_sink (gst_pipeline_new ("transmit-pipeline"));
  priv->rtpbin = gst_element_factory_make ("rtpbin", "transmit-rtpbin");
  g_object_set (priv->rtpbin, "latency", RTP_DEFAULT_LATENCY_MS, NULL);

#ifdef __linux__
  asrc = gst_element_factory_make ("pulsesrc", NULL);
  /* latency-time to 5 ms, we use the system clock */
  g_object_set (asrc, "latency-time", 5000, "provide-clock", FALSE, NULL);
#elif defined(__APPLE__) && defined(TARGET_OS_MAC)
  asrc = ov_pipeline_get_osxaudiosrcbin (NULL);
  /* same properties as above already set on the source element */
#else
#error "Unsupported operating system"
#endif

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

  if (priv->video_device == NULL) {
    vsrc = gst_element_factory_make ("videotestsrc", NULL);
    g_object_set (vsrc, "is-live", TRUE, NULL);
  } else {
    vsrc = gst_device_create_element (priv->video_device, NULL);
  }

  /* XXX: Perhaps make a new element that encodes to JPEG/H264 if necessary
   * or does passthrough if downstream supports the negotiated caps */
  if (priv->device_video_format == OV_VIDEO_FORMAT_JPEG ||
      priv->device_video_format == OV_VIDEO_FORMAT_H264) {
    /* Passthrough JPEG and H.264 */
    vqueue = gst_element_factory_make ("queue", "video-queue");
  } else if (priv->device_video_format == OV_VIDEO_FORMAT_YUY2 ||
      priv->device_video_format == OV_VIDEO_FORMAT_TEST) {
    /* We encode YUY2 to JPEG before sending */
    vqueue = gst_element_factory_make ("jpegenc", NULL);
    g_object_set (vqueue, "quality", 30, NULL);
  } else {
    /* It is a programmer error for this to be reached */
    g_assert_not_reached ();
  }

  GST_DEBUG ("Negotiated video caps that can be transmitted: %" GST_PTR_FORMAT,
      priv->send_vcaps);

  /* This has already been created in ov_local_peer_init() */
  vfilter = priv->transmit_vcapsfilter;

  /* If the application hasn't set the caps itself to some arbitrary supported
   * value, we will set them to the best possible quality */
  vcaps = ov_local_peer_get_transmit_video_caps (local);
  if (vcaps == NULL || gst_caps_is_any (vcaps)) {
    vcaps = gst_caps_fixate (gst_caps_copy (priv->send_vcaps));
    ov_local_peer_set_transmit_video_caps (local, vcaps);
  }
  g_clear_pointer (&vcaps, gst_caps_unref);

  if (priv->send_video_format == OV_VIDEO_FORMAT_H264) {
    vpay = gst_element_factory_make ("rtph264pay", NULL);
    /* If the mtu is too small, the payloader splits the NAL Unit across
     * multiple packets and that breaks depayloading for some reason. Let the
     * network layer handle splitting up and re-joining of packets. */
    g_object_set (vpay, "mtu", 20000, NULL);
  } else {
    vpay = gst_element_factory_make ("rtpjpegpay", NULL);
  }
  /* Send RTP video data */
  vrtpqueue = gst_element_factory_make ("queue", NULL);
  vsink = gst_element_factory_make ("udpsink", "vsend_rtp_sink");
  g_object_set (vsink, "buffer-size", OV_VIDEO_SEND_BUFSIZE,
      "enable-last-sample", FALSE, NULL);
  /* Send RTCP SR for video (same packets for all peers) */
  vrtcpqueue = gst_element_factory_make ("queue", NULL);
  vrtcpsink = gst_element_factory_make ("udpsink", "vsend_rtcp_sink");
  g_object_set (vrtcpsink, "sync", FALSE, "async", FALSE, NULL);
  /* Recv RTCP RR for video (same port for all peers) */
  vrtcpsrc = gst_element_factory_make ("udpsrc", "vrecv_rtcp_src");

  gst_bin_add_many (GST_BIN (priv->transmit), priv->rtpbin, asrc,
      afilter, aencode, apay, artpqueue, asink, artcpqueue, artcpsink, artcpsrc,
      vsrc, vqueue, vfilter, vpay, vrtpqueue, vsink, vrtcpqueue, vrtcpsink,
      vrtcpsrc, NULL);

  /* Link audio branch */
  ret = gst_element_link_many (asrc, afilter, aencode, apay, NULL);
  g_assert (ret);
  ret = gst_element_link (artcpqueue, artcpsink);
  g_assert (ret);
  priv->asend_rtcp_sink = artcpsink;
  ret = gst_element_link (artpqueue, asink);
  g_assert (ret);
  priv->asend_rtp_sink = asink;
  priv->arecv_rtcp_src = artcpsrc;

  /* Send RTP data */
  ret = gst_element_link_pads (apay, "src", priv->rtpbin,
      "send_rtp_sink_0");
  g_assert (ret);
  ret = gst_element_link_pads (priv->rtpbin, "send_rtp_src_0", artpqueue,
      "sink");
  g_assert (ret);

  /* Send RTCP SR */
  ret = gst_element_link_pads (priv->rtpbin, "send_rtcp_src_0", artcpqueue,
      "sink");
  g_assert (ret);

  /* Recv RTCP RR */
  ret = gst_element_link_pads (artcpsrc, "src", priv->rtpbin,
        "recv_rtcp_sink_0");
  g_assert (ret);

  /* Link video branch */
  ret = gst_element_link_many (vsrc, vqueue, vfilter, vpay, NULL);
  g_assert (ret);
  ret = gst_element_link (vrtcpqueue, vrtcpsink);
  g_assert (ret);
  priv->vsend_rtcp_sink = vrtcpsink;
  ret = gst_element_link (vrtpqueue, vsink);
  g_assert (ret);
  priv->vsend_rtp_sink = vsink;
  priv->vrecv_rtcp_src = vrtcpsrc;

  /* Send RTP data */
  ret = gst_element_link_pads (vpay, "src", priv->rtpbin, "send_rtp_sink_1");
  g_assert (ret);
  ret = gst_element_link_pads (priv->rtpbin, "send_rtp_src_1", vrtpqueue,
      "sink");
  g_assert (ret);

  /* Send RTCP SR */
  ret = gst_element_link_pads (priv->rtpbin, "send_rtcp_src_1", vrtcpqueue,
      "sink");
  g_assert (ret);

  /* Recv RTCP RR */
  ret = gst_element_link_pads (vrtcpsrc, "src", priv->rtpbin,
      "recv_rtcp_sink_1");
  g_assert (ret);

  /* All done */

  /* Use the system clock and explicitly reset the base/start times to ensure
   * that all the pipelines started by us have the same base/start times */
  gst_pipeline_use_clock (GST_PIPELINE (priv->transmit),
      gst_system_clock_obtain ());
  gst_element_set_base_time (priv->transmit, 0);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->transmit));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_transmit_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to transmit to remote peers");

  return TRUE;
}

gboolean
ov_local_peer_setup_comms (OvLocalPeer * local)
{
  GList *l;
  gboolean ret;
  gchar *addr_s;
  GSocket *mc_socket;
  GSource *mc_source;
  GInetAddress *mc_group;
  GSocketAddress *mc_addr;
  GInetSocketAddress *addr;
  OvLocalPeerPrivate *priv;
  GError *error = NULL;

  priv = ov_local_peer_get_private (local);

  /*-- Listen for incoming TCP connections --*/

  /* Threaded socket service since we use blocking TCP network reads
   * TODO: Use threads equal to number of remote peers? To ensure that peers
   * never wait while communicating. */
  priv->tcp_server = g_threaded_socket_service_new (10);

  g_object_get (OV_PEER (local), "address", &addr, "address-string", &addr_s,
      NULL);

  ret = g_socket_listener_add_address (G_SOCKET_LISTENER (priv->tcp_server),
      G_SOCKET_ADDRESS (addr), G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
      NULL, NULL, &error);
  if (!ret) {
    GST_ERROR ("Unable to setup TCP server (%s): %s", addr_s, error->message);
    g_error_free (error);
    goto out_early;
  }

  g_signal_connect (priv->tcp_server, "run",
      G_CALLBACK (on_incoming_peer_tcp_connection), local);

  g_socket_service_start (priv->tcp_server);
  GST_DEBUG ("Listening for incoming TCP connections on %s", addr_s);

  /*-- Listen for incoming UDP messages (multicast and unicast) --*/
  mc_group = g_inet_address_new_from_string (OV_MULTICAST_GROUP);
  /* Use this hard-coded port for UDP messages; it's our canonical port */
  mc_addr = g_inet_socket_address_new (mc_group, OV_DEFAULT_COMM_PORT);
  g_object_unref (mc_group);

  /* Create and bind multicast socket */
  mc_socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, NULL);
  if (!g_socket_bind (mc_socket, mc_addr, TRUE, &error)) {
    gchar *name =
      g_inet_address_to_string (g_inet_socket_address_get_address (addr));
    GST_ERROR ("Unable to bind to multicast addr/port (%s:%u): %s", name,
        OV_DEFAULT_COMM_PORT, error->message);
    g_error_free (error);
    g_free (name);
    goto out;
  }

  /* Attach an event source for incoming messages to the default main context */
  mc_source = g_socket_create_source (mc_socket, G_IO_IN, NULL);
  g_source_set_callback (mc_source, (GSourceFunc) on_incoming_udp_message,
      local, NULL);
  g_source_attach (mc_source, NULL);
  priv->mc_socket_source = mc_source;

  /* Join multicast groups on all interfaces */
  ret = FALSE;
  l = priv->mc_ifaces;
  while (l != NULL) {
    if (!g_socket_join_multicast_group (mc_socket, mc_group, FALSE, l->data,
        &error)) {
      GList *next = l->next;
      GST_WARNING ("Unable to setup a multicast listener on %s: %s",
          (gchar*) l->data, error->message);
      g_clear_error (&error);
      /* Not listening on this interface; remove it from the list */
      priv->mc_ifaces = g_list_delete_link (priv->mc_ifaces, l);
      l = next;
    } else {
      GST_DEBUG ("Listening for incoming multicast messages on %s",
          (gchar*) l->data);
      /* Return FALSE only if we couldn't listen on any interfaces */
      ret = TRUE;
      l = l->next;
    }
  }

  if (!ret)
    g_source_destroy (mc_source);
out:
  g_object_unref (mc_addr);
  g_object_unref (mc_socket);
out_early:
  g_object_unref (addr);
  g_free (addr_s);
  return ret;
}

/*-- REMOTE PEER SETUP --*/
static void
rtpbin_pad_added (GstElement * rtpbin, GstPad * srcpad,
    OvRemotePeer * remote)
{
  GstPad *sinkpad;
  GstElement *queue;
  GstPadLinkReturn ret;
  gchar *name = gst_pad_get_name (srcpad);
  guint len = G_N_ELEMENTS ("recv_rtp_src_");

  /* Match the session number to the correct branch (audio or video)
   * The session number is the first %u in the pad name of the form
   * 'recv_rtp_src_%u_%u_%u' */
  if (name[len-1] == '0')
    queue = remote->priv->aqueue;
  else if (name[len-1] == '1')
    queue = remote->priv->vqueue;
  else
    /* We only have two streams with known session numbers */
    g_assert_not_reached ();
  g_free (name);

  sinkpad = gst_element_get_static_pad (queue, "sink");
  ret = gst_pad_link (srcpad, sinkpad);
  g_assert (ret == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
}

static void
on_receiver_ssrc_active (GstElement * rtpbin, guint session, guint ssrc,
    OvRemotePeer * remote)
{
  GST_DEBUG ("ssrc %u, session %u, remote %s active", ssrc, session,
      remote->addr_s);
  remote->last_seen = g_get_monotonic_time ();
}

void
ov_local_peer_setup_remote_receive (OvLocalPeer * local, OvRemotePeer * remote)
{
  gboolean ret;
  GSocket *socket;
  GstElement *rtpbin;
  GstElement *asrc, *artcpsrc, *adecode, *asink, *artcpsink;
  GstElement *vsrc, *vrtcpsrc, *vdecode, *vsink, *vrtcpsink;
  GInetSocketAddress *local_addr;
  gchar *local_addr_s, *remote_addr_s;
  OvVideoFormat video_format;
  GstCaps *rtpcaps;

  g_assert (remote->priv->recv_acaps != NULL &&
      remote->priv->recv_vcaps != NULL && remote->priv->recv_ports[0] > 0 &&
      remote->priv->recv_ports[1] > 0 && remote->priv->recv_ports[2] > 0 &&
      remote->priv->recv_ports[3] > 0);

  g_object_get (OV_PEER (local), "address", &local_addr, NULL);
  local_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (local_addr));
  remote_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (remote->addr));
  g_object_unref (local_addr);

  /* Setup pipeline (remote->receive) to recv & decode from a remote peer */

  video_format = ov_caps_to_video_format (remote->priv->recv_vcaps);

  rtpbin = gst_element_factory_make ("rtpbin", "recv-rtpbin-%u");
  g_object_set (rtpbin, "latency", RTP_DEFAULT_LATENCY_MS, "drop-on-latency",
      TRUE, NULL);

  /* TODO: Both audio and video should be optional */

  /* Recv RTP audio data */
  socket = ov_get_socket_for_addr (local_addr_s, remote->priv->recv_ports[0]);
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
  socket = ov_get_socket_for_addr (local_addr_s, remote->priv->recv_ports[1]);
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
  vsrc = gst_element_factory_make ("udpsrc", "vrecv_rtp_src-%u");
  /* The depayloader will detect the height/width/framerate on the fly
   * This allows us to change that without communicating new caps
   * TODO: Use decodebin instead of hard-coding elements */
  if (video_format == OV_VIDEO_FORMAT_JPEG) {
    rtpcaps = gst_caps_from_string (RTP_JPEG_VIDEO_CAPS_STR);
    remote->priv->vdepay = gst_element_factory_make ("rtpjpegdepay", NULL);
    vdecode = gst_element_factory_make ("jpegdec", NULL);
  } else if (video_format == OV_VIDEO_FORMAT_H264) {
    rtpcaps = gst_caps_from_string (RTP_H264_VIDEO_CAPS_STR);
    remote->priv->vdepay = gst_element_factory_make ("rtph264depay", NULL);
    vdecode = gst_element_factory_make ("avdec_h264", NULL);
  } else {
    g_assert_not_reached ();
  }
  remote->priv->aqueue = gst_element_factory_make ("queue", "aqueue");
  remote->priv->vqueue = gst_element_factory_make ("queue", "vqueue");
  /* Pre-depayloader queues. Ensures decoupling of depayloading/decoding into
   * threads separate from the jitterbuffer. */
  g_object_set (remote->priv->aqueue, "max-size-buffers", 0, "max-size-bytes", 0,
      "max-size-time", 100 * GST_MSECOND, NULL);
  g_object_set (remote->priv->vqueue, "max-size-buffers", 0, "max-size-bytes", 0,
      "max-size-time", 100 * GST_MSECOND, NULL);
  socket = ov_get_socket_for_addr (local_addr_s, remote->priv->recv_ports[2]);
  g_object_set (vsrc, "buffer-size", OV_VIDEO_RECV_BUFSIZE, "socket", socket,
      "caps", rtpcaps, NULL);
  gst_caps_unref (rtpcaps);
  g_object_unref (socket);

  vsink = gst_element_factory_make ("proxysink", "video-proxysink-%u");
  g_assert (vsink != NULL);
  /* Recv RTCP SR for video */
  socket = ov_get_socket_for_addr (local_addr_s, remote->priv->recv_ports[3]);
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
      asrc, remote->priv->aqueue, remote->priv->adepay, adecode, asink,
      vsrc, remote->priv->vqueue, remote->priv->vdepay, vdecode, vsink,
      artcpsink, artcpsrc, vrtcpsink, vrtcpsrc, NULL);

  /* Link audio branch via rtpbin */
  ret = gst_element_link_many (remote->priv->aqueue, remote->priv->adepay,
      adecode, asink, NULL);
  g_assert (ret);

  /* Recv audio RTP and send to rtpbin */
  ret = gst_element_link_pads (asrc, "src", rtpbin, "recv_rtp_sink_0");
  g_assert (ret);

  /* Recv audio RTCP SR etc and send to rtpbin */
  ret = gst_element_link_pads (artcpsrc, "src", rtpbin, "recv_rtcp_sink_0");
  g_assert (ret);

  /* Send audio RTCP RR etc from rtpbin */
  ret = gst_element_link_pads (rtpbin, "send_rtcp_src_0", artcpsink, "sink");
  g_assert (ret);

  /* Link video branch via rtpbin */
  ret = gst_element_link_many (remote->priv->vqueue, remote->priv->vdepay,
      vdecode, vsink, NULL);
  g_assert (ret);

  /* Recv video RTP and send to rtpbin */
  ret = gst_element_link_pads (vsrc, "src", rtpbin, "recv_rtp_sink_1");
  g_assert (ret);

  /* Recv video RTCP SR etc and send to rtpbin */
  ret = gst_element_link_pads (vrtcpsrc, "src", rtpbin, "recv_rtcp_sink_1");
  g_assert (ret);

  /* Send video RTCP RR etc from rtpbin */
  ret = gst_element_link_pads (rtpbin, "send_rtcp_src_1", vrtcpsink, "sink");
  g_assert (ret);

  /* When recv_rtp_src_%u_%u_%u pads corresponding to the above recv_rtp_sink_%u
   * sinkpads are added when the pipeline pre-rolls, 'pad-added' will be called
   * and we'll finish linking the pipeline */
  g_signal_connect (rtpbin, "pad-added", G_CALLBACK (rtpbin_pad_added), remote);

  /* The remote is timed out if this isn't invoked for the timeout duration */
  g_signal_connect (rtpbin, "on-ssrc-active",
      G_CALLBACK (on_receiver_ssrc_active), remote);

  /* This is what exposes video/audio data from this remote peer */
  remote->priv->audio_proxysink = asink;
  remote->priv->video_proxysink = vsink;

  GST_DEBUG ("Setup pipeline to receive from remote");
  g_free (remote_addr_s);
  g_free (local_addr_s);
}

void
ov_local_peer_setup_remote_playback (OvLocalPeer * local, OvRemotePeer * remote)
{
  GstPad *ghostpad, *srcpad, *sinkpad;
  GstPadLinkReturn ret;
  gboolean res;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  /* Setup pipeline (priv->playback) to aggregate audio from all remote peers
   * to audiomixer and then render using the provided audio sink
   *  [ proxysrc ! audiomixer ] */
  if (remote->priv->audio_proxysink) {
    remote->priv->audio_proxysrc =
      gst_element_factory_make ("proxysrc", "audio-proxysrc-%u");
    g_assert (remote->priv->audio_proxysrc != NULL);

    /* Link the two pipelines */
    g_object_set (remote->priv->audio_proxysrc, "proxysink",
        remote->priv->audio_proxysink, NULL);

    sinkpad = gst_element_get_request_pad (priv->audiomixer, "sink_%u");

    gst_bin_add_many (GST_BIN (remote->priv->aplayback),
        remote->priv->audio_proxysrc, NULL);
    res = gst_bin_add (GST_BIN (priv->playback), remote->priv->aplayback);
    g_assert (res);

    srcpad = gst_element_get_static_pad (remote->priv->audio_proxysrc, "src");
    ghostpad = gst_ghost_pad_new ("audiopad", srcpad);
    res = gst_pad_set_active (ghostpad, TRUE);
    g_assert (res);
    res = gst_element_add_pad (remote->priv->aplayback, ghostpad);
    g_assert (res);
    gst_object_unref (srcpad);

    ret = gst_pad_link (ghostpad, sinkpad);
    g_assert (ret == GST_PAD_LINK_OK);
    gst_object_unref (sinkpad);
  }

  /* Setup pipeline (priv->playback) to render video from each local to the
   * provided video sink */
  if (remote->priv->video_proxysink) {
    remote->priv->video_proxysrc =
      gst_element_factory_make ("proxysrc", "video-proxysrc-%u");
    g_assert (remote->priv->video_proxysrc != NULL);

    /* Link the two pipelines */
    g_object_set (remote->priv->video_proxysrc, "proxysink",
        remote->priv->video_proxysink, NULL);

    /* If a remote_peer_add_sink wasn't used, use a fallback (xv|gl)imagesink */
    if (remote->priv->video_sink == NULL) {
      /* On Linux (Mesa), using multiple GL output windows leads to a
       * crash due to a bug in Mesa related to multiple GLX contexts */
      if (_ov_opengl_is_mesa ())
        remote->priv->video_sink =
          gst_parse_bin_from_description ("videoconvert ! xvimagesink", TRUE,
              NULL);
      else
        remote->priv->video_sink =
          gst_element_factory_make ("glimagesink", NULL);
    }

    gst_bin_add_many (GST_BIN (remote->priv->vplayback),
        remote->priv->video_proxysrc, remote->priv->video_sink, NULL);
    res = gst_bin_add (GST_BIN (priv->playback), remote->priv->vplayback);
    g_assert (res);
    res = gst_element_link (remote->priv->video_proxysrc,
        remote->priv->video_sink);
    g_assert (res);
  }

  GST_DEBUG ("Setup local pipeline to playback remote");
}
