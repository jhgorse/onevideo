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

/* FIXME: Need to force formats everywhere because of appsrc/appsink.
 * Fix by replacing with intervideosink/src */
#define RAW_AUDIO_CAPS_STR "audio/x-raw, format=S16LE, channels=2, rate=48000, layout=interleaved"
#define VIDEO_CAPS_STR "width=1280, height=720, framerate=30/1"
#define RAW_VIDEO_CAPS_STR "video/x-raw, " VIDEO_CAPS_STR ", format=I420"

static GstCaps *raw_audio_caps = NULL;
static GstCaps *raw_video_caps = NULL;

struct _OneVideoLocalPeerPriv {
  /* primary audio playback elements */
  GstElement *audiomixer;
  GstElement *audiosink;

  /* udpsinks transmitting RTP media */
  GstElement *audpsink;
  GstElement *vudpsink;

  /* Array of OneVideoRemotePeers: peers we want to connect to */
  GPtrArray *remote_peers;
  /* Lock to access non-thread-safe structures like GPtrArray */
  GMutex lock;
};

struct _OneVideoRemotePeerPriv {
  /* Audio/Video appsinks inside the receive pipeline */
  GstElement *audio_appsink;
  GstElement *video_appsink;

  /* playback bins; are inside the playback pipeline in OneVideoLocalPeer */
  GstElement *aplayback;
  GstElement *vplayback;
  /* Audio/Video appsrcs inside the playback pipelines; aplayback/vplayback */
  GstElement *audio_appsrc;
  GstElement *video_appsrc;
};

static gboolean _setup_transmit_pipeline (OneVideoLocalPeer *local);
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
one_video_local_peer_new (GInetAddress *addr)
{
  OneVideoLocalPeer *local;

  if (onevideo_debug == NULL)
    GST_DEBUG_CATEGORY_INIT (onevideo_debug, "onevideo", 0,
        "OneVideo VoIP library");
  if (raw_audio_caps == NULL)
    raw_audio_caps = gst_caps_from_string (RAW_AUDIO_CAPS_STR);
  if (raw_video_caps == NULL)
    raw_video_caps = gst_caps_from_string (RAW_VIDEO_CAPS_STR);

  local = g_new0 (OneVideoLocalPeer, 1);
  local->state = ONE_VIDEO_STATE_NULL;
  local->addr = addr;
  local->priv = g_new0 (OneVideoLocalPeerPriv, 1);
  /* XXX: GPtrArray is not thread-safe, you must lock accesses to it */
  local->priv->remote_peers = g_ptr_array_new ();
  g_mutex_init (&local->priv->lock);

  /* Initialize transmit pipeline */
  g_assert (_setup_transmit_pipeline (local));

  /* Setup components of the playback pipeline */
  g_assert (_setup_playback_pipeline (local));
  local->state = ONE_VIDEO_STATE_READY;

  return local;
}

void
one_video_local_peer_stop (OneVideoLocalPeer * local)
{
  one_video_local_peer_stop_transmit (local);
  one_video_local_peer_stop_playback (local);
  local->state = ONE_VIDEO_STATE_NULL;
}

void
one_video_local_peer_free (OneVideoLocalPeer * local)
{
  g_object_unref (local->transmit);
  g_object_unref (local->playback);
  if (local->addr)
    g_object_unref (local->addr);

  g_ptr_array_free (local->priv->remote_peers, TRUE);
  g_mutex_clear (&local->priv->lock);
  g_free (local->priv);

  g_free (local);
}

OneVideoRemotePeer *
one_video_remote_peer_new (OneVideoLocalPeer * local, const gchar * addr_s,
    guint audio_port, guint video_port)
{
  gchar *name;
  GstBus *bus;
  OneVideoRemotePeer *remote;

  remote = g_new0 (OneVideoRemotePeer, 1);
  remote->receive = gst_pipeline_new ("receive-%u");
  remote->local = local;
  remote->addr_s = g_strdup (addr_s);
  remote->audio_port = audio_port ? audio_port : UDPCLIENT_AUDIO_PORT;
  remote->video_port = video_port ? video_port : UDPCLIENT_VIDEO_PORT;

  remote->priv = g_new0 (OneVideoRemotePeerPriv, 1);
  name = g_strdup_printf ("audio-playback-bin-%s", remote->addr_s);
  remote->priv->aplayback = gst_bin_new (name);
  g_free (name);
  name = g_strdup_printf ("video-playback-bin-%s", remote->addr_s);
  remote->priv->vplayback = gst_bin_new (name);
  g_free (name);

  bus = gst_pipeline_get_bus (GST_PIPELINE (remote->receive));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_remote_receive_error), remote);
  g_object_unref (bus);

  return remote;
}

static void
one_video_remote_peer_remove_nolock (OneVideoRemotePeer * remote)
{
  gchar *tmp;
  OneVideoLocalPeer *local = remote->local;

  /* Stop receiving */
  g_assert (gst_element_set_state (remote->receive, GST_STATE_NULL)
      == GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (remote->receive);
  remote->receive = NULL;

  if (remote->priv->audio_appsrc != NULL) {
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

  if (remote->priv->video_appsrc != NULL) {
    g_assert (gst_element_set_state (remote->priv->vplayback, GST_STATE_NULL)
        == GST_STATE_CHANGE_SUCCESS);
    g_assert (gst_bin_remove (GST_BIN (local->playback),
          remote->priv->vplayback));
    GST_DEBUG ("Released video playback bin of remote %s", remote->addr_s);
  }

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
  GstElement *filter, *aconvert, *audiovis, *convert;

  if (local->playback != NULL && GST_IS_PIPELINE (local->playback))
    /* Already setup */
    return TRUE;

  /* Setup audio bits */
  local->playback = gst_pipeline_new ("playback-%u");
  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (local->playback), FALSE);
  local->priv->audiomixer = gst_element_factory_make ("audiomixer", NULL);
  filter = gst_element_factory_make ("capsfilter", "audiomixer-capsfilter");
  g_object_set (filter, "caps", raw_audio_caps, NULL);
  /* XXX: aconvert isn't needed without wavescope */
  aconvert = gst_element_factory_make ("audioconvert", NULL);
  audiovis = gst_element_factory_make ("wavescope", NULL);
  convert = gst_element_factory_make ("videoconvert", NULL);
  local->priv->audiosink = gst_element_factory_make ("xvimagesink", NULL);

  /* FIXME: If there's no audio, this pipeline will mess up while going from
   * NULL -> PLAYING -> NULL -> PLAYING because of async state change bugs in
   * basesink. Fix this by only plugging a sink if audio is present. */
  gst_bin_add_many (GST_BIN (local->playback), local->priv->audiomixer,
      filter, aconvert, audiovis, convert, local->priv->audiosink, NULL);
  g_assert (gst_element_link_many (local->priv->audiomixer, filter, aconvert,
        audiovis, convert, local->priv->audiosink, NULL));

  /* Video bits are setup by each local */

  bus = gst_pipeline_get_bus (GST_PIPELINE (local->playback));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_playback_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to playback remote peers");

  return TRUE;
}

static gboolean
_setup_transmit_pipeline (OneVideoLocalPeer * local)
{
  GstBus *bus;
  GstCaps *jpeg_video_caps;
  GstElement *asrc, *afilter, *aencode, *apay, *asink;
  GstElement *vsrc, *vfilter, *vpay, *vsink;

  if (local->transmit != NULL && GST_IS_PIPELINE (local->transmit))
    /* Already setup */
    return TRUE;

  local->transmit = gst_pipeline_new ("transmit-%u");

  asrc = gst_element_factory_make ("pulsesrc", NULL);
  afilter = gst_element_factory_make ("capsfilter", "audio-transmit-caps-%u");
  g_object_set (afilter, "caps", raw_audio_caps, NULL);
  aencode = gst_element_factory_make ("opusenc", NULL);
  apay = gst_element_factory_make ("rtpopuspay", NULL);
  asink = gst_element_factory_make ("udpsink", "audio-transmit-udpsink");
  local->priv->audpsink = asink;

  /* FIXME: Make this configurable (use GstDevice*)
   * FIXME: We want to support JPEG, keyframe-only H264, and video/x-raw.
   * FIXME: Select the best format based on formats available on the camera */
  vsrc = gst_element_factory_make ("v4l2src", NULL);
  vfilter = gst_element_factory_make ("capsfilter", "video-transmit-caps-%u");
  jpeg_video_caps = gst_caps_from_string ("image/jpeg, " VIDEO_CAPS_STR);
  g_object_set (vfilter, "caps", jpeg_video_caps, NULL);
  gst_caps_unref (jpeg_video_caps);
  vpay = gst_element_factory_make ("rtpjpegpay", NULL);
  vsink = gst_element_factory_make ("udpsink", "video-transmit");
  local->priv->vudpsink = vsink;

  gst_bin_add_many (GST_BIN (local->transmit), asrc, afilter, aencode, apay,
      asink, vsrc, vfilter, vpay, vsink, NULL);
  g_assert (gst_element_link_many (asrc, afilter, aencode, apay, asink, NULL));
  g_assert (gst_element_link_many (vsrc, vfilter, vpay, vsink, NULL));

  bus = gst_pipeline_get_bus (GST_PIPELINE (local->transmit));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error",
      G_CALLBACK (on_local_transmit_error), local);
  g_object_unref (bus);

  GST_DEBUG ("Setup pipeline to transmit to remote local");

  return TRUE;
}

static void
append_string (gchar * addr_s, GString * clients, const gchar * port)
{
  gchar *client;

  client = g_strdup_printf ("%s:%s,", addr_s, port);
  g_string_append (clients, client);
  g_free (client);
}

static void
append_aclients (gpointer data, gpointer user_data)
{
  OneVideoRemotePeer *remote = data;
  append_string (remote->addr_s, user_data, STR(UDPCLIENT_AUDIO_PORT));
}

static void
append_vclients (gpointer data, gpointer user_data)
{
  OneVideoRemotePeer *remote = data;
  append_string (remote->addr_s, user_data, STR(UDPCLIENT_VIDEO_PORT));
}

static gboolean
one_video_local_peer_begin_transmit (OneVideoLocalPeer * local)
{
  GstStateChangeReturn ret;
  GString *aclients, *vclients;

  aclients = g_string_new ("");
  vclients = g_string_new ("");
  
  g_mutex_lock (&local->priv->lock);
  g_ptr_array_foreach (local->priv->remote_peers, append_aclients, aclients);
  g_ptr_array_foreach (local->priv->remote_peers, append_vclients, vclients);
  g_object_set (local->priv->audpsink, "clients", aclients->str, NULL);
  g_object_set (local->priv->vudpsink, "clients", vclients->str, NULL);
  g_mutex_unlock (&local->priv->lock);

  g_string_free (aclients, TRUE);
  g_string_free (vclients, TRUE);

  ret = gst_element_set_state (local->transmit, GST_STATE_PLAYING);
  GST_DEBUG ("Transmitting to remote peers");

  return ret != GST_STATE_CHANGE_FAILURE;
}

static GstFlowReturn
push_sample (GstElement *appsink, GstElement *appsrc)
{
  GstFlowReturn ret;
  GstSample *sample;
  GstBuffer *buffer;

  g_print (".");
  g_signal_emit_by_name (appsink, "pull-sample", &sample);
  g_assert (sample != NULL);

  buffer = gst_sample_get_buffer (sample);
  g_signal_emit_by_name (appsrc, "push-buffer", buffer, &ret);
  gst_sample_unref (sample);

  return ret;
}

static void
one_video_local_peer_setup_remote_receive (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  GstCaps *rtp_caps;
  GstElement *asrc, *adepay, *adecode, *afilter, *asink;
  GstElement *vsrc, *vdepay, *vdecode, *vconvert, *vrate, *vfilter, *vsink;

  /* Setup pipeline (remote->receive) to recv & decode from a remote local */
  /* FIXME: Fetch and set udpsrc caps using RTCP or similar */
  asrc = gst_element_factory_make ("udpsrc", "audio-receive-udpsrc-%u");
  rtp_caps = gst_caps_from_string (RTP_AUDIO_CAPS_STR);
  g_object_set (asrc, "address", remote->addr_s, "port", remote->audio_port,
      "caps", rtp_caps, NULL);
  gst_caps_unref (rtp_caps);
  /* audiomixer does not do audio conversion, so we need to do it ourselves */
  adepay = gst_element_factory_make ("rtpopusdepay", NULL);
  adecode = gst_element_factory_make ("opusdec", NULL);
  afilter = gst_element_factory_make ("capsfilter", "audio-receive-caps-%u");
  g_object_set (afilter, "caps", raw_audio_caps, NULL);
  asink = gst_element_factory_make ("appsink", "audio-appsink-%u");
  g_object_set (asink, "emit-signals", TRUE, NULL);
  gst_bin_add_many (GST_BIN (remote->receive), asrc, adepay, adecode,
      afilter, asink, NULL);
  g_assert (gst_element_link_many (asrc, adepay, adecode, afilter,
        asink, NULL));

  vsrc = gst_element_factory_make ("udpsrc", "video-receive-udpsrc-%u");
  rtp_caps = gst_caps_from_string (RTP_VIDEO_CAPS_STR);
  g_object_set (vsrc, "address", remote->addr_s, "port", remote->video_port,
      "caps", rtp_caps, NULL);
  gst_caps_unref (rtp_caps);
  vdepay = gst_element_factory_make ("rtpjpegdepay", NULL);
  vdecode = gst_element_factory_make ("jpegdec", NULL);
  /* We need this despite setting caps everywhere because the jpeg might have
   * been encoded by the webcam, in which case it could be in any raw format */
  vconvert = gst_element_factory_make ("videoconvert", NULL);
  /* XXX: REMOVE ME WHEN WE MOVE TO INTERVIDEO. THIS IS ONLY NEEDED FOR CAPS
   * NEGOTIATION. IS NOT NEEDED WITH INTERVIDEO. */
  vrate = gst_element_factory_make ("videorate", NULL);
  vfilter = gst_element_factory_make ("capsfilter", "video-receive-caps-%u");
  g_object_set (vfilter, "caps", raw_video_caps, NULL);
  /* FIXME: When implementing format negotiation between peers, we have to
   * either use intervideosink or set the caps on the corresponding appsrc.
   * This works right now because we have fixed video caps everywhere. */
  vsink = gst_element_factory_make ("appsink", "video-appsink-%u");
  g_object_set (vsink, "emit-signals", TRUE, NULL);
  gst_bin_add_many (GST_BIN (remote->receive), vsrc, vdepay, vdecode, vconvert,
      vrate, vfilter, vsink, NULL);
  g_assert (gst_element_link_many (vsrc, vdepay, vdecode, vconvert, vrate,
        vfilter, vsink, NULL));

  /* This is what exposes video/audio data from this remote local */
  remote->priv->audio_appsink = asink;
  remote->priv->video_appsink = vsink;

  /* Add to our list of remote peers */
  g_mutex_lock (&local->priv->lock);
  g_ptr_array_add (local->priv->remote_peers, remote);
  g_mutex_unlock (&local->priv->lock);

  /* XXX: We need to go to PLAYING very soon after this */
  gst_element_set_state (remote->receive, GST_STATE_READY);
  GST_DEBUG ("Setup pipeline to receive from remote local");
}

static void
one_video_local_peer_setup_remote_playback (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  GstElement *filter;
  GstPad *ghostpad, *srcpad, *sinkpad;
  GstPadLinkReturn ret;

  /* Setup pipeline (local->playback) to aggregate audio from all remote peers to
   * audiomixer and then render using the provided audio sink
   *  [ appsrc ! capsfilter ] ! audiomixer */
  if (remote->priv->audio_appsink) {
    remote->priv->audio_appsrc = gst_element_factory_make ("appsrc",
        "audio-appsrc-%u");
    g_object_set (remote->priv->audio_appsrc, "format", GST_FORMAT_TIME,
        "is-live", TRUE, "emit-signals", FALSE, NULL);
    filter = gst_element_factory_make ("capsfilter", "audio-sink-caps-%u");
    g_object_set (filter, "caps", raw_audio_caps, NULL);

    sinkpad = gst_element_get_request_pad (local->priv->audiomixer, "sink_%u");

    gst_bin_add_many (GST_BIN (remote->priv->aplayback),
        remote->priv->audio_appsrc, filter, NULL);
    g_assert (gst_bin_add (GST_BIN (local->playback), remote->priv->aplayback));

    srcpad = gst_element_get_static_pad (filter, "src");
    ghostpad = gst_ghost_pad_new ("audiopad", srcpad);
    g_assert (gst_pad_set_active (ghostpad, TRUE));
    g_assert (gst_element_add_pad (remote->priv->aplayback, ghostpad));
    gst_object_unref (srcpad);

    g_assert (gst_element_link (remote->priv->audio_appsrc, filter));
    ret = gst_pad_link (ghostpad, sinkpad);
    g_assert (ret == GST_PAD_LINK_OK);
    gst_object_unref (sinkpad);

    g_signal_connect (remote->priv->audio_appsink, "new-sample",
        G_CALLBACK (push_sample), remote->priv->audio_appsrc);
  }

  /* Setup pipeline (local->playback) to render video from each local to the
   * provided video sink */
  if (remote->priv->video_appsink) {
    GstElement *sink;

    remote->priv->video_appsrc = gst_element_factory_make ("appsrc",
        "video-appsrc-%u");
    g_object_set (remote->priv->video_appsrc, "format", GST_FORMAT_TIME,
        "is-live", TRUE, "emit-signals", FALSE, NULL);
    filter = gst_element_factory_make ("capsfilter", "video-sink-caps-%u");
    g_object_set (filter, "caps", raw_video_caps, NULL);
    sink = gst_element_factory_make ("xvimagesink", NULL);
    gst_bin_add_many (GST_BIN (remote->priv->vplayback),
        remote->priv->video_appsrc, filter, sink, NULL);
    g_assert (gst_bin_add (GST_BIN (local->playback), remote->priv->vplayback));
    g_assert (gst_element_link_many (remote->priv->video_appsrc, filter, sink,
          NULL));

    g_signal_connect (remote->priv->video_appsink, "new-sample",
        G_CALLBACK (push_sample), remote->priv->video_appsrc);
  }

  GST_DEBUG ("Setup pipeline to playback remote local");
}

gboolean
one_video_local_peer_setup_remote (OneVideoLocalPeer * local,
    OneVideoRemotePeer * remote)
{
  one_video_local_peer_setup_remote_receive (local, remote);
  one_video_local_peer_setup_remote_playback (local, remote);
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
  }
  g_mutex_unlock (&local->priv->lock);

  ret = gst_element_set_state (local->playback, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto play_fail;

  local->state = ONE_VIDEO_STATE_PLAYING;
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
