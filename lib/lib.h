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

#ifndef __ONE_VIDEO_LIB_H__
#define __ONE_VIDEO_LIB_H__

#include <stdlib.h>
#include <gst/gst.h>
#include <gio/gio.h>

G_BEGIN_DECLS

GST_DEBUG_CATEGORY_EXTERN (onevideo_debug);
#define GST_CAT_DEFAULT onevideo_debug

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define UDPCLIENT_ADATA_PORT 5000
#define UDPCLIENT_ARTCP_PORT 5001
#define UDPCLIENT_VDATA_PORT 5002
#define UDPCLIENT_VRTCP_PORT 5003

#define RTP_DEFAULT_LATENCY_MS 10

#define RTP_AUDIO_CAPS_STR "application/x-rtp, payload=96, media=audio, clock-rate=48000, encoding-name=OPUS"
#define RTP_VIDEO_CAPS_STR "application/x-rtp, payload=26, media=video, clock-rate=90000, encoding-name=JPEG, framerate=30/1"

typedef struct _OneVideoLocalPeer OneVideoLocalPeer;
typedef struct _OneVideoLocalPeerPriv OneVideoLocalPeerPriv;
typedef enum _OneVideoLocalPeerState OneVideoLocalPeerState;

typedef struct _OneVideoRemotePeer OneVideoRemotePeer;
typedef struct _OneVideoRemotePeerPriv OneVideoRemotePeerPriv;

enum _OneVideoLocalPeerState {
  ONE_VIDEO_STATE_NULL,
  ONE_VIDEO_STATE_READY,
  ONE_VIDEO_STATE_PLAYING,
};

/* Represents us; the library and the client implementing this local */
struct _OneVideoLocalPeer {
  /* Transmit pipeline */
  GstElement *transmit;
  /* Playback pipeline */
  GstElement *playback;
  /* Address we're listening on */
  GInetAddress *addr;

  OneVideoLocalPeerState state;

  OneVideoLocalPeerPriv *priv;
};

/* Represents a remote local */
struct _OneVideoRemotePeer {
  OneVideoLocalPeer *local;

  /* Receive pipeline */
  GstElement *receive;
  /* Address of remote peer */
  gchar *addr_s;

  OneVideoRemotePeerPriv *priv;
};

OneVideoLocalPeer*  one_video_local_peer_new            (GInetAddress *addr,
                                                         gchar *v4l2_device_path);
void                one_video_local_peer_free           (OneVideoLocalPeer *local);
void                one_video_local_peer_stop           (OneVideoLocalPeer *local);

OneVideoRemotePeer* one_video_remote_peer_new           (OneVideoLocalPeer *local,
                                                         const gchar *addr_s);
void                one_video_remote_peer_pause         (OneVideoRemotePeer *remote);
void                one_video_remote_peer_resume        (OneVideoRemotePeer *remote);
void                one_video_remote_peer_remove        (OneVideoRemotePeer *remote);
void                one_video_remote_peer_free          (OneVideoRemotePeer *remote);

GPtrArray*          one_video_local_peer_find_remotes   (OneVideoLocalPeer *local);
gboolean            one_video_local_peer_setup_remote   (OneVideoLocalPeer *local,
                                                         OneVideoRemotePeer *remote);
gboolean            one_video_local_peer_start          (OneVideoLocalPeer * local);

G_END_DECLS

#endif /* __ONE_VIDEO_LIB_H__ */
