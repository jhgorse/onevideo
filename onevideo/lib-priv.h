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

G_BEGIN_DECLS

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

GST_DEBUG_CATEGORY_EXTERN (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

#define CAPS_SEP ", "

/* We force the same raw audio format everywhere */
#define AUDIO_CAPS_STR "format=S16LE, channels=2, rate=48000, layout=interleaved"
/* This is only used for the test video source */
#define VIDEO_CAPS_STR "width=1280, height=720, framerate=30/1"

#define AUDIO_FORMAT_OPUS "audio/x-opus"
#define VIDEO_FORMAT_JPEG "image/jpeg"
#define VIDEO_FORMAT_H264 "video/x-h264"

#define RTP_ALL_AUDIO_CAPS_STR "application/x-rtp, payload=96, media=audio, clock-rate=48000, encoding-name=OPUS"
#define RTP_JPEG_VIDEO_CAPS_STR "application/x-rtp, payload=26, media=video, clock-rate=90000, encoding-name=JPEG"
#define RTP_H264_VIDEO_CAPS_STR "application/x-rtp, payload=96, media=video, clock-rate=90000, encoding-name=H264"

typedef enum _OvMediaType OvMediaType;

enum _OvMediaType {
  OV_MEDIA_TYPE_UNKNOWN    = 0,
  OV_MEDIA_TYPE_JPEG,
  OV_MEDIA_TYPE_H264, /* Not supported yet */
  OV_MEDIA_TYPE_YUY2, /* Fallback if JPEG/H264 are not supported */
};

struct _OvRemotePeerPrivate {
  /* The remote ports we transmit data to using udpsink, in order:
   * {audio_rtp, audio_send_rtcp SRs, audio_send_rtcp RRs,
   *  video_rtp, video_send_rtcp SRs, video_send_rtcp RRs} */
  guint send_ports[6];
  /* The local ports we receive data on using udpsrc, in order:
   * {audio_rtp, audio_recv_rtcp SRs,
   *  video_rtp, video_recv_rtcp SRs} */
  guint recv_ports[4];

  /*-- Receive pipeline --*/
  /* The caps that we will receive */
  GstCaps *recv_acaps;
  GstCaps *recv_vcaps;
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

G_END_DECLS

#endif /* __OV_LIB_PRIV_H__ */
