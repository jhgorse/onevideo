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

#include <gst/gst.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define ONE_VIDEO_DEFAULT_COMM_PORT 5000

#define RTP_DEFAULT_LATENCY_MS 10

typedef struct _OneVideoLocalPeer OneVideoLocalPeer;
typedef struct _OneVideoLocalPeerPriv OneVideoLocalPeerPriv;
typedef enum _OneVideoLocalPeerState OneVideoLocalPeerState;

typedef struct _OneVideoRemotePeer OneVideoRemotePeer;
typedef struct _OneVideoRemotePeerPriv OneVideoRemotePeerPriv;
typedef enum _OneVideoRemotePeerState OneVideoRemotePeerState;

typedef struct _OneVideoDiscoveredPeer OneVideoDiscoveredPeer;

/**
 * OneVideoRemoteFoundCallback:
 * @remote: (transfer full): a #OneVideoDiscoveredPeer representing the found
 * remote peer
 * @user_data: the user data passed
 *
 * See the documentation for one_video_local_peer_find_remotes_create_source()
 */
typedef gboolean (*OneVideoRemoteFoundCallback) (OneVideoDiscoveredPeer *peer,
                                                 gpointer user_data);

enum _OneVideoLocalPeerState {
  ONE_VIDEO_LOCAL_STATE_NULL          = 0,

  /**~ Special states ~**/
  /* These are ORed with other states to signal a special state */
  /* This is ORed with the current state to signal failure */
  ONE_VIDEO_LOCAL_STATE_FAILED        = 1 << 0,
  ONE_VIDEO_LOCAL_STATE_TIMEOUT       = 1 << 1,
  /* One of these is ORed with the current state when we're taking on the role
   * of either negotiator or negotiatee */
  ONE_VIDEO_LOCAL_STATE_NEGOTIATOR    = 1 << 7,
  ONE_VIDEO_LOCAL_STATE_NEGOTIATEE    = 1 << 8,

  /* Ordinary states. These are not ORed with each other. */
  ONE_VIDEO_LOCAL_STATE_INITIALISED   = 1 << 9,

  ONE_VIDEO_LOCAL_STATE_NEGOTIATING   = 1 << 10,
  ONE_VIDEO_LOCAL_STATE_NEGOTIATED    = 1 << 11,

  ONE_VIDEO_LOCAL_STATE_READY         = 1 << 12,
  ONE_VIDEO_LOCAL_STATE_PLAYING       = 1 << 13,
  ONE_VIDEO_LOCAL_STATE_PAUSED        = 1 << 14,
  ONE_VIDEO_LOCAL_STATE_STOPPED       = 1 << 15,
};

enum _OneVideoRemotePeerState {
  ONE_VIDEO_REMOTE_STATE_NULL,
  ONE_VIDEO_REMOTE_STATE_FAILED,
  ONE_VIDEO_REMOTE_STATE_ALLOCATED,

  ONE_VIDEO_REMOTE_STATE_READY,
  ONE_VIDEO_REMOTE_STATE_PLAYING,
  ONE_VIDEO_REMOTE_STATE_PAUSED,
};

/* Represents us; the library and the client implementing this local */
struct _OneVideoLocalPeer {
  /* Transmit pipeline */
  GstElement *transmit;
  /* Playback pipeline */
  GstElement *playback;
  /* Address we're listening on */
  GInetSocketAddress *addr;
  /* String representation of the above address (for logging, etc) */
  gchar *addr_s;
  /* Unique id string representing this host */
  gchar *id;

  OneVideoLocalPeerState state;

  OneVideoLocalPeerPriv *priv;
};

/* Represents a remote local */
struct _OneVideoRemotePeer {
  OneVideoLocalPeer *local;

  /* Receive pipeline */
  GstElement *receive;
  /* Address of remote peer */
  GInetSocketAddress *addr;
  /* String representation (for logging, etc) */
  gchar *addr_s;
  /* Unique id string representing this host
   * Retrieved from the peer during negotiation */
  gchar *id;

  OneVideoRemotePeerState state;

  OneVideoRemotePeerPriv *priv;
};

/* Represents a discovered peer */
struct _OneVideoDiscoveredPeer {
  /* Address of the discovered peer */
  GInetSocketAddress *addr;
  /* String representation; contains port if non-default port */
  gchar *addr_s;

  /* Monotonic time when this peer was discovered */
  gint64 discover_time;
};

/* Local peer (us) */
OneVideoLocalPeer*  one_video_local_peer_new              (GSocketAddress *addr);
void                one_video_local_peer_free             (OneVideoLocalPeer *local);
void                one_video_local_peer_add_remote       (OneVideoLocalPeer *local,
                                                           OneVideoRemotePeer *remote);
/* Asynchronously negotiate with all setup remote peers */
gboolean            one_video_local_peer_negotiate_async  (OneVideoLocalPeer *local,
                                                           GCancellable *cancellable,
                                                           GAsyncReadyCallback callback,
                                                           gpointer callback_data);
gboolean            one_video_local_peer_negotiate_finish (OneVideoLocalPeer *local,
                                                           GAsyncResult *result,
                                                           GError **error);
gboolean            one_video_local_peer_negotiate_stop   (OneVideoLocalPeer *local);
gboolean            one_video_local_peer_start            (OneVideoLocalPeer *local);
void                one_video_local_peer_stop             (OneVideoLocalPeer *local);

/* Device discovery */
GList*              one_video_local_peer_get_video_devices  (OneVideoLocalPeer *local);
gboolean            one_video_local_peer_set_video_device   (OneVideoLocalPeer *local,
                                                             GstDevice *device);

/* Peer discovery */
GSource*            one_video_local_peer_find_remotes_create_source (OneVideoLocalPeer *local,
                                                                     GCancellable *cancellable,
                                                                     OneVideoRemoteFoundCallback callback,
                                                                     gpointer callback_data,
                                                                     GError **error);

/* Remote peers */
OneVideoRemotePeer* one_video_remote_peer_new               (OneVideoLocalPeer *local,
                                                             GInetSocketAddress *addr);
OneVideoRemotePeer* one_video_remote_peer_new_from_string   (OneVideoLocalPeer *local,
                                                             const gchar *addr_s);
void                one_video_remote_peer_free              (OneVideoRemotePeer *remote);
void                one_video_remote_peer_pause             (OneVideoRemotePeer *remote);
void                one_video_remote_peer_resume            (OneVideoRemotePeer *remote);
void                one_video_remote_peer_remove            (OneVideoRemotePeer *remote);

OneVideoRemotePeer* one_video_local_peer_get_remote_by_id   (OneVideoLocalPeer *local,
                                                             const gchar *peer_id);

/* Discovered peers */
OneVideoDiscoveredPeer*   one_video_discovered_peer_new   (GInetSocketAddress *addr);
void                      one_video_discovered_peer_free  (OneVideoDiscoveredPeer *peer);

G_END_DECLS

#endif /* __ONE_VIDEO_LIB_H__ */
