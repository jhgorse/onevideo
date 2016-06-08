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

#ifndef __OV_LIB_PRIV_H__
#define __OV_LIB_PRIV_H__

#include <glib.h>
#include <gst/gst.h>

#ifdef __APPLE__
/* Gives us access to TARGET_OS_* */
#include <TargetConditionals.h>
#endif

G_BEGIN_DECLS

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

GST_DEBUG_CATEGORY_EXTERN (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

#define CAPS_FIELD_SEP ", "
#define CAPS_STRUC_SEP "; "

#define RTP_DEFAULT_LATENCY_MS 10

/* We force the same raw audio format everywhere */
#define AUDIO_CAPS_STR "format=S16LE, channels=2, rate=48000, layout=interleaved"
#define AUDIO_RATE 48000
/* This is only used for the test video source since we need both width and
 * height in the capsfilter for it */
#define TEST_VIDEO_CAPS_720P_STR "width=1280, height=720, framerate={ 30/1, 15/1, 5/1 }"
#define TEST_VIDEO_CAPS_360P_STR "width=640, height=360, framerate={ 30/1, 15/1, 5/1 }"
#define TEST_VIDEO_CAPS_240P_STR "width=320, height=240, framerate={ 10/1, 5/1 }"

#define AUDIO_FORMAT_OPUS "audio/x-opus"
#define VIDEO_FORMAT_JPEG "image/jpeg"
#define VIDEO_FORMAT_H264 "video/x-h264"

#define RTP_ALL_AUDIO_CAPS_STR "application/x-rtp, payload=96, media=audio, clock-rate=48000, encoding-name=OPUS"
#define RTP_JPEG_VIDEO_CAPS_STR "application/x-rtp, payload=26, media=video, clock-rate=90000, encoding-name=JPEG"
#define RTP_H264_VIDEO_CAPS_STR "application/x-rtp, payload=96, media=video, clock-rate=90000, encoding-name=H264"

/* For simplicity, we always use 0 for audio RTP sessions and 1 for video
 * XXX: These are also used as indices for the ssrc[] arrays on OvLocalPeerPriv
 * and OvRemotePeerPriv, so keep them within the range */
#define OV_AUDIO_RTP_SESSION            0
#define OV_VIDEO_RTP_SESSION            1
#define OV_AUDIO_RTP_SESSION_STR        STR(OV_AUDIO_RTP_SESSION)
#define OV_VIDEO_RTP_SESSION_STR        STR(OV_VIDEO_RTP_SESSION)
#define OV_AUDIO_RTP_SESSION_NAME       "audio"
#define OV_VIDEO_RTP_SESSION_NAME       "video"

#define OV_RTP_SESSION_IS_VALID(X) \
  (X == OV_AUDIO_RTP_SESSION ? TRUE : (X == OV_VIDEO_RTP_SESSION ? TRUE : FALSE))

#define OV_RTP_SESSION_TO_NAME(X) \
  (X == OV_AUDIO_RTP_SESSION ? \
    OV_AUDIO_RTP_SESSION_NAME : \
    (X == OV_VIDEO_RTP_SESSION ? OV_VIDEO_RTP_SESSION_NAME : "unknown"))

#define OV_RTP_SESSION_FROM_NAME(X) \
  (g_strcmp0 (X, OV_AUDIO_RTP_SESSION_NAME) == 0 ? \
    OV_AUDIO_RTP_SESSION : \
    (g_strcmp0 (X, OV_VIDEO_RTP_SESSION_NAME) == 0 ? OV_VIDEO_RTP_SESSION : -1))

typedef enum _OvVideoFormat OvVideoFormat;

enum _OvVideoFormat {
  OV_VIDEO_FORMAT_UNKNOWN     = 0,
  OV_VIDEO_FORMAT_TEST        = 1,      /* videotestsrc if no hardware sources are found */

  /* This is where the list of media formats ends */
  OV_VIDEO_FORMAT_SENTINEL    = 1 << 1,

  /* These formats are listed in increasing order of desirability
   * Video sources will usually support combinations of these */
  OV_VIDEO_FORMAT_YUY2        = 1 << 2, /* Fallback if JPEG/H264 are not supported */
  OV_VIDEO_FORMAT_JPEG        = 1 << 3, /* Almost every webcam should support this */
  OV_VIDEO_FORMAT_H264        = 1 << 4, /* Not supported yet */
};

struct _OvRemotePeerPrivate {
  /* The destination ports we transmit data to using udpsink, in order:
   * {audio_rtp, audio_send_rtcp SRs, audio_send_rtcp RRs,
   *  video_rtp, video_send_rtcp SRs, video_send_rtcp RRs} */
  guint16 send_ports[6];
  /* The local ports we receive data on using udpsrc, in order:
   * {audio_rtp, audio_recv_rtcp SRs,
   *  video_rtp, video_recv_rtcp SRs} */
  guint16 recv_ports[4];

  /* When this remote starts sending us data, we store the audio and
   * video SSRCs here so we can keep track of RTP statistics via the
   * RTPSource for this SSRC within the RTPSession inside GstRtpBin
   * GstRtpBin::get-internal-session ->
   *   RTPSession::get-source-by-ssrc ->
   *     RTPSource::stats ->
   *       GstStructure
   *
   * {audio_ssrc, video_ssrc} */
  guint ssrcs[2];

  /*-- Receive pipeline --*/
  /* The format that we will receive data in from this peer */
  GstCaps *recv_acaps;
  GstCaps *recv_vcaps;
  /* Pre-depayloader queues */
  GstElement *aqueue;
  GstElement *vqueue;
  /* Depayloaders */
  GstElement *adepay;
  GstElement *vdepay;
  /* Audio/Video proxysinks */
  GstElement *audio_proxysink;
  GstElement *video_proxysink;

  /*-- Playback pipeline --*/
  /* playback bins */
  GstElement *aplayback;
  GstElement *vplayback;
  /* Audio/Video proxysrcs inside aplayback/vplayback */
  GstElement *audio_proxysrc;
  GstElement *video_proxysrc;
  /* Video sink */
  GstElement *video_sink;
};

/* OvVideoFormat is not a public symbol */
GstCaps*        ov_video_format_to_caps (OvVideoFormat format);
OvVideoFormat   ov_caps_to_video_format (const GstCaps *caps);
gboolean        _ov_opengl_is_mesa      (void);

G_END_DECLS

#endif /* __OV_LIB_PRIV_H__ */
