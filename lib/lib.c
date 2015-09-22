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

GST_DEBUG_CATEGORY (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

/* We force the same audio formats everywhere for now, till we can do
 * peer format negotiation */
#define RAW_AUDIO_CAPS_STR "audio/x-raw, format=S16LE, channels=2, rate=48000, layout=interleaved"
#define VIDEO_CAPS_STR "width=1280, height=720, framerate=30/1"
#define RAW_VIDEO_CAPS_STR "video/x-raw, " VIDEO_CAPS_STR ", format=I420"

static GstCaps *raw_audio_caps = NULL;
static GstCaps *raw_video_caps = NULL;

struct _OneVideoLocalPeerPriv {
  /* Transmit GstRtpBin */
  GstElement *rtpbin;

  /* primary audio playback elements */
  GstElement *audiomixer;
  GstElement *audiosink;

  /* udpsinks transmitting RTP and RTCP */
  GstElement *audpsink;
  GstElement *artcpudpsink;
  GstElement *vudpsink;
  GstElement *vrtcpudpsink;

  /* Array of OneVideoRemotePeers: peers we want to connect to */
  GPtrArray *remote_peers;
  /* Lock to access non-thread-safe structures like GPtrArray */
  GMutex lock;
};

struct _OneVideoRemotePeerPriv {
  /*-- Receive pipeline --*/
  /* GstRtpBin inside the receive pipeline */
  GstElement *rtpbin;
  /* Depayloaders */
  GstElement *adepay;
  GstElement *vdepay;
  /* Audio/Video proxysinks */
  GstElement *audio_proxysink;
  GstElement *video_proxysink;
  /* Receive udpsrc ports in order:
   * {audio, audio_rtcp, video, video_rtcp} */
  guint recv_ports[4];

  /*-- Playback pipeline --*/
  /* playback bins */
  GstElement *aplayback;
  GstElement *vplayback;
  /* Audio/Video proxysrcs inside aplayback/vplayback */
  GstElement *audio_proxysrc;
  GstElement *video_proxysrc;
};

static gboolean _setup_transmit_pipeline (OneVideoLocalPeer *local,
    gchar *v4l2_device_path);
static gboolean _setup_playback_pipeline (OneVideoLocalPeer *local);
static void one_video_remote_peer_remove_nolock (OneVideoRemotePeer *remote);
static void one_video_local_peer_setup_remote_receive (OneVideoLocalPeer *local,
    OneVideoRemotePeer *remote);
static void one_video_local_peer_setup_remote_playback (OneVideoLocalPeer *local,
    OneVideoRemotePeer *remote);
static gboolean one_video_local_peer_begin_transmit (OneVideoLocalPeer *local);

static void
on_gst_bus_error (GstBus * bus, GstMessage * msg, gpointer data)
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

#define on_local_transmit_error on_gst_bus_error
#define on_local_playback_error on_gst_bus_error
#define on_remote_receive_error on_gst_bus_error

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
      (GFunc) one_video_remote_peer_remove_nolock, NULL);
  g_ptr_array_free (local->priv->remote_peers, TRUE);
  local->priv->remote_peers = g_ptr_array_new ();
  g_mutex_unlock (&local->priv->lock);

  g_assert (gst_element_set_state (local->playback, GST_STATE_NULL)
      == GST_STATE_CHANGE_SUCCESS);
  GST_DEBUG ("Stopped playback");
}

OneVideoLocalPeer *
one_video_local_peer_new (GInetSocketAddress * listen_addr,
    gchar * v4l2_device_path)
{
  OneVideoLocalPeer *local;

  g_return_val_if_fail (listen_addr != NULL, NULL);

  if (onevideo_debug == NULL)
    GST_DEBUG_CATEGORY_INIT (onevideo_debug, "onevideo", 0,
        "OneVideo VoIP library");
  if (raw_audio_caps == NULL)
    raw_audio_caps = gst_caps_from_string (RAW_AUDIO_CAPS_STR);
  if (raw_video_caps == NULL)
    raw_video_caps = gst_caps_from_string (RAW_VIDEO_CAPS_STR);

  local = g_new0 (OneVideoLocalPeer, 1);
  local->addr = listen_addr;
  g_object_ref (local->addr);
  local->state = ONE_VIDEO_LOCAL_STATE_NULL;
  local->priv = g_new0 (OneVideoLocalPeerPriv, 1);
  /* XXX: GPtrArray is not thread-safe, you must lock accesses to it */
  local->priv->remote_peers = g_ptr_array_new ();
  g_mutex_init (&local->priv->lock);

  /* Initialize transmit pipeline */
  g_assert (_setup_transmit_pipeline (local, v4l2_device_path));

  /* Setup components of the playback pipeline */
  g_assert (_setup_playback_pipeline (local));
  local->state = ONE_VIDEO_LOCAL_STATE_SETUP;

  return local;
}

void
one_video_local_peer_stop (OneVideoLocalPeer * local)
{
  one_video_local_peer_stop_transmit (local);
  one_video_local_peer_stop_playback (local);
  local->state = ONE_VIDEO_LOCAL_STATE_NULL;
}

void
one_video_local_peer_free (OneVideoLocalPeer * local)
{
  g_object_unref (local->transmit);
  g_object_unref (local->playback);
  g_object_unref (local->addr);

  g_ptr_array_free (local->priv->remote_peers, TRUE);
  g_mutex_clear (&local->priv->lock);
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
  guint ii, jj, start, nmemb;
  guint *used_ports;

  /* Start from the port right after the configured TCP communication port */
  start = 1 + g_inet_socket_address_get_port (local->addr);
  if (local->priv->remote_peers->len < 1)
    goto done;

  /* Variable-sized statically-allocated arrays aren't supported by msvc */
  nmemb = 4 * local->priv->remote_peers->len;
  used_ports = g_malloc0 (nmemb * sizeof (guint));

  for (ii = 0; ii < local->priv->remote_peers->len; ii++) {
    OneVideoRemotePeer *remote;
    remote = g_ptr_array_index (local->priv->remote_peers, ii);
    for (jj = 0; jj < sizeof (remote->priv->recv_ports); jj++)
      used_ports[4 * ii + jj] = remote->priv->recv_ports[jj];
  }

  qsort (used_ports, nmemb, sizeof (guint), compare_ints);

  /* Ports are always in sets of 4, so if we find a hole in the sorted list of
   * used ports, it definitely has 4 ports in it */
  for (ii = 0; ii < nmemb; ii++)
    if (used_ports[ii] == start)
      start++;

  /* TODO: Check whether these ports are actually available on the system */

  g_free (used_ports);

done:
  (*recv_ports)[0] = start;
  (*recv_ports)[1] = start + 1;
  (*recv_ports)[2] = start + 2;
  (*recv_ports)[3] = start + 3;
  return TRUE;
}

OneVideoRemotePeer *
one_video_remote_peer_new (OneVideoLocalPeer * local, const gchar * addr_s)
{
  gchar *name;
  GstBus *bus;
  OneVideoRemotePeer *remote;

  remote = g_new0 (OneVideoRemotePeer, 1);
  remote->state = ONE_VIDEO_REMOTE_STATE_NULL;
  remote->receive = gst_pipeline_new ("receive-%u");
  remote->local = local;
  remote->addr_s = g_strdup (addr_s);

  remote->priv = g_new0 (OneVideoRemotePeerPriv, 1);
  name = g_strdup_printf ("audio-playback-bin-%s", remote->addr_s);
  remote->priv->aplayback = gst_bin_new (name);
  g_free (name);
  name = g_strdup_printf ("video-playback-bin-%s", remote->addr_s);
  remote->priv->vplayback = gst_bin_new (name);
  g_free (name);

  g_mutex_lock (&local->priv->lock);
  g_assert (set_free_recv_ports (local, &remote->priv->recv_ports));
  /* Add to our list of remote peers */
  g_ptr_array_add (local->priv->remote_peers, remote);
  g_mutex_unlock (&local->priv->lock);

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
one_video_remote_peer_remove_nolock (OneVideoRemotePeer * remote)
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
  one_video_remote_peer_free (remote);
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

  one_video_remote_peer_remove_nolock (remote);
}

void
one_video_remote_peer_free (OneVideoRemotePeer * remote)
{
  g_free (remote->addr_s);
  g_free (remote->priv);
  g_free (remote);
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

static gboolean
_setup_playback_pipeline (OneVideoLocalPeer * local)
{
  GstBus *bus;
  GstElement *filter;

  if (local->playback != NULL && GST_IS_PIPELINE (local->playback))
    /* Already setup */
    return TRUE;

  /* Setup audio bits */
  local->playback = gst_pipeline_new ("playback-%u");
  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (local->playback), FALSE);
  local->priv->audiomixer = gst_element_factory_make ("audiomixer", NULL);
  filter = gst_element_factory_make ("capsfilter", "audiomixer-capsfilter");
  g_object_set (filter, "caps", raw_audio_caps, NULL);
  local->priv->audiosink = gst_element_factory_make ("pulsesink", NULL);

  /* FIXME: If there's no audio, this pipeline will mess up while going from
   * NULL -> PLAYING -> NULL -> PLAYING because of async state change bugs in
   * basesink. Fix this by only plugging a sink if audio is present. */
  gst_bin_add_many (GST_BIN (local->playback), local->priv->audiomixer,
      filter, local->priv->audiosink, NULL);
  g_assert (gst_element_link_many (local->priv->audiomixer, filter,
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

static gboolean
_setup_transmit_pipeline (OneVideoLocalPeer * local, gchar * v4l2_device_path)
{
  GstBus *bus;
  GstCaps *jpeg_video_caps;
  GstElement *asrc, *afilter, *aencode, *apay, *asink, *artcpsink;
  GstElement *vsrc, *vfilter, *vpay, *vsink, *vrtcpsink;
  GstPad *srcpad, *sinkpad;

  if (local->transmit != NULL && GST_IS_PIPELINE (local->transmit))
    /* Already setup */
    return TRUE;

  local->transmit = gst_pipeline_new ("transmit-pipeline");
  local->priv->rtpbin = gst_element_factory_make ("rtpbin", "transmit-rtpbin");
  g_object_set (local->priv->rtpbin, "latency", RTP_DEFAULT_LATENCY_MS, NULL);

  asrc = gst_element_factory_make ("pulsesrc", NULL);
  afilter = gst_element_factory_make ("capsfilter", "audio-transmit-caps");
  g_object_set (afilter, "caps", raw_audio_caps, NULL);
  aencode = gst_element_factory_make ("opusenc", NULL);
  apay = gst_element_factory_make ("rtpopuspay", NULL);
  asink = gst_element_factory_make ("udpsink", "adata-transmit-udpsink");
  artcpsink = gst_element_factory_make ("udpsink", "artcp-transmit-udpsink");

  /* FIXME: Use GstDevice* instead of a device path string
   * FIXME: We want to support JPEG, keyframe-only H264, and video/x-raw.
   * FIXME: Select the best format based on formats available on the camera */
  vsrc = gst_element_factory_make ("v4l2src", NULL);
  if (v4l2_device_path != NULL)
    g_object_set (vsrc, "device", v4l2_device_path, NULL);
  vfilter = gst_element_factory_make ("capsfilter", "video-transmit-caps");
  jpeg_video_caps = gst_caps_from_string ("image/jpeg, " VIDEO_CAPS_STR);
  g_object_set (vfilter, "caps", jpeg_video_caps, NULL);
  gst_caps_unref (jpeg_video_caps);
  vpay = gst_element_factory_make ("rtpjpegpay", NULL);
  vsink = gst_element_factory_make ("udpsink", "vdata-transmit-udpsink");
  vrtcpsink = gst_element_factory_make ("udpsink", "vrtcp-transmit-udpsink");

  gst_bin_add_many (GST_BIN (local->transmit), local->priv->rtpbin, asrc,
      afilter, aencode, apay, asink, vsrc, vfilter, vpay, vsink, NULL);

  /* Link audio branch */
  g_assert (gst_element_link_many (asrc, afilter, aencode, apay, NULL));

  srcpad = gst_element_get_static_pad (apay, "src");
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
  g_assert (gst_element_link_many (vsrc, vfilter, vpay, NULL));

  srcpad = gst_element_get_static_pad (vpay, "src");
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

static void
append_clients (gpointer data, gpointer user_data)
{
  OneVideoRemotePeer *remote = data;
  GString **clients = user_data;

  g_string_append_printf (clients[0], "%s:%u,", remote->addr_s,
      remote->priv->recv_ports[0]);
  g_string_append_printf (clients[1], "%s:%u,", remote->addr_s,
      remote->priv->recv_ports[1]);
  g_string_append_printf (clients[2], "%s:%u,", remote->addr_s,
      remote->priv->recv_ports[2]);
  g_string_append_printf (clients[3], "%s:%u,", remote->addr_s,
      remote->priv->recv_ports[3]);
}

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
  
  g_mutex_lock (&local->priv->lock);
  g_ptr_array_foreach (local->priv->remote_peers, append_clients, clients);
  g_object_set (local->priv->audpsink, "clients", clients[0]->str, NULL);
  g_object_set (local->priv->artcpudpsink, "clients", clients[1]->str, NULL);
  g_object_set (local->priv->vudpsink, "clients", clients[2]->str, NULL);
  g_object_set (local->priv->vrtcpudpsink, "clients", clients[3]->str, NULL);
  g_mutex_unlock (&local->priv->lock);

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

static void
one_video_local_peer_setup_remote_receive (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  gchar *local_addr_s;
  GstCaps *rtp_caps;
  GstElement *asrc, *artcpsrc, *adecode, *asink;
  GstElement *vsrc, *vrtcpsrc, *vdecode, *vconvert, *vsink;
  GstPad *srcpad, *sinkpad;

  local_addr_s =
    g_inet_address_to_string (g_inet_socket_address_get_address (local->addr));

  /* Setup pipeline (remote->receive) to recv & decode from a remote peer */

  remote->priv->rtpbin = gst_element_factory_make ("rtpbin", "recv-rtpbin-%u");
  g_object_set (remote->priv->rtpbin, "latency", RTP_DEFAULT_LATENCY_MS,
      "drop-on-latency", TRUE, NULL);

  /* FIXME: Fetch and set udpsrc caps using SDP over UDP
   * FIXME: Both audio and video should be optional once we have negotiation */
  asrc = gst_element_factory_make ("udpsrc", "adata-recv-udpsrc-%u");
  rtp_caps = gst_caps_from_string (RTP_AUDIO_CAPS_STR);
  g_object_set (asrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[0], "caps", rtp_caps, NULL);
  gst_caps_unref (rtp_caps);
  artcpsrc = gst_element_factory_make ("udpsrc", "artcp-recv-udpsrc-%u");
  g_object_set (artcpsrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[1], NULL);
  remote->priv->adepay = gst_element_factory_make ("rtpopusdepay", NULL);
  adecode = gst_element_factory_make ("opusdec", NULL);
  asink = gst_element_factory_make ("proxysink", "audio-proxysink-%u");
  g_assert (asink != NULL);

  vsrc = gst_element_factory_make ("udpsrc", "vdata-recv-udpsrc-%u");
  rtp_caps = gst_caps_from_string (RTP_VIDEO_CAPS_STR);
  g_object_set (vsrc, "address", local_addr_s, "port",
      remote->priv->recv_ports[2], "caps", rtp_caps, NULL);
  gst_caps_unref (rtp_caps);
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

static void
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

gboolean
one_video_local_peer_setup_remote (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  one_video_local_peer_setup_remote_receive (local, remote);
  one_video_local_peer_setup_remote_playback (local, remote);
  remote->state = ONE_VIDEO_REMOTE_STATE_SETUP;
  return TRUE;
}

gboolean
one_video_local_peer_start (OneVideoLocalPeer * local)
{
  guint index;
  GstStateChangeReturn ret;
  OneVideoRemotePeer *remote;

  g_assert (one_video_local_peer_begin_transmit (local));

  g_mutex_lock (&local->priv->lock);
  for (index = 0; index < local->priv->remote_peers->len; index++) {
    remote = g_ptr_array_index (local->priv->remote_peers, index);
    ret = gst_element_set_state (remote->receive, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      g_mutex_unlock (&local->priv->lock);
      goto recv_fail;
    }
    remote->state = ONE_VIDEO_REMOTE_STATE_PLAYING;
  }
  g_mutex_unlock (&local->priv->lock);

  ret = gst_element_set_state (local->playback, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto play_fail;

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
