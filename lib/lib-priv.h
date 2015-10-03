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

#ifndef __ONE_VIDEO_LIB_PRIV_H__
#define __ONE_VIDEO_LIB_PRIV_H__

#include "lib.h"

G_BEGIN_DECLS

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

GST_DEBUG_CATEGORY_EXTERN (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

/* We force the same raw audio format everywhere */
#define AUDIO_CAPS_STR "format=S16LE, channels=2, rate=48000, layout=interleaved"
#define VIDEO_CAPS_STR "width=1280, height=720, framerate=30/1"

struct _OneVideoNegotiate {
  /* Call id while negotiating */
  guint64 call_id;
  /* The remote that we're talking to */
  OneVideoRemotePeer *negotiator;
  /* Potential remotes while negotiating */
  GHashTable *remotes;
};

typedef struct _OneVideoNegotiate OneVideoNegotiate;

struct _OneVideoLocalPeerPriv {
  /* Transmit GstRtpBin */
  GstElement *rtpbin;

  /* udpsinks transmitting RTP and RTCP */
  GstElement *audpsink;
  GstElement *artcpudpsink;
  GstElement *vudpsink;
  GstElement *vrtcpudpsink;

  /* primary audio playback elements */
  GstElement *audiomixer;
  GstElement *audiosink;

  /* TCP Server for communication */
  GSocketService *tcp_server;
  /* A unique id representing an active call (0 if no active call) */
  guint64 active_call_id;
  /* Struct used for holding info while negotiating */
  OneVideoNegotiate *negotiate;

  /* The caps that we support sending */
  GstCaps *supported_send_acaps;
  GstCaps *supported_send_vcaps;
  /* The caps that we support receiving */
  GstCaps *supported_recv_acaps;
  GstCaps *supported_recv_vcaps;
  /* The caps that we *will* send */
  GstCaps *send_acaps;
  GstCaps *send_vcaps;

  /* List of UDP ports that are either reserved or in use for receiving */
  GArray *used_ports;
  /* Array of OneVideoRemotePeers: peers we want to connect to */
  GPtrArray *remote_peers;
  /* Lock to access non-thread-safe structures like GPtrArray */
  GRecMutex lock;

  /* Path to the v4l2 device being used */
  gchar *v4l2_path;
};

struct _OneVideoRemotePeerPriv {
  /* Transmit udpsink ports in order:
   * {audio, audio_rtcp, video, video_rtcp} */
  guint send_ports[4];

  /*-- Receive pipeline --*/
  /* Receive udpsrc ports in order:
   * {audio, audio_rtcp, video, video_rtcp} */
  guint recv_ports[4];
  /* The caps that we will receive */
  GstCaps *recv_acaps;
  GstCaps *recv_vcaps;
  /* GstRtpBin inside the receive pipeline */
  GstElement *rtpbin;
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
};

gboolean            one_video_local_peer_setup               (OneVideoLocalPeer *local);
void                one_video_remote_peer_remove_not_array   (OneVideoRemotePeer *remote);

G_END_DECLS

#endif /* __ONE_VIDEO_LIB_PRIV_H__ */
