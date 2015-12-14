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

#ifndef __OV_LIB_H__
#define __OV_LIB_H__

#include <gst/gst.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _OvLocalPeer       OvLocalPeer;
typedef struct _OvLocalPeerPriv   OvLocalPeerPriv;

typedef struct _OvRemotePeer      OvRemotePeer;
typedef struct _OvRemotePeerPriv  OvRemotePeerPriv;

typedef enum _OvLocalPeerState    OvLocalPeerState;
typedef enum _OvRemotePeerState   OvRemotePeerState;

typedef struct _OvDiscoveredPeer OvDiscoveredPeer;

#define OV_DEFAULT_COMM_PORT 5000

#define RTP_DEFAULT_LATENCY_MS 10

/**
 * OvRemoteFoundCallback:
 * @remote: (transfer full): a #OvDiscoveredPeer representing the found
 * remote peer
 * @user_data: the user data passed
 *
 * See the documentation for ov_local_peer_find_remotes_create_source()
 */
typedef gboolean (*OvRemoteFoundCallback) (OvDiscoveredPeer *peer,
                                           gpointer user_data);

enum _OvLocalPeerState {
  OV_LOCAL_STATE_NULL          = 0,

  /**~ Special states ~**/
  /* These are ORed with other states to signal a special state */
  /* This is ORed with the current state to signal failure */
  OV_LOCAL_STATE_FAILED        = 1 << 0,
  OV_LOCAL_STATE_TIMEOUT       = 1 << 1,
  /* One of these is ORed with the current state when we're taking on the role
   * of either negotiator or negotiatee */
  OV_LOCAL_STATE_NEGOTIATOR    = 1 << 7,
  OV_LOCAL_STATE_NEGOTIATEE    = 1 << 8,

  /* Ordinary states. These are not ORed with each other. */
  OV_LOCAL_STATE_INITIALISED   = 1 << 9,

  OV_LOCAL_STATE_NEGOTIATING   = 1 << 10,
  OV_LOCAL_STATE_NEGOTIATED    = 1 << 11,

  OV_LOCAL_STATE_READY         = 1 << 12,
  OV_LOCAL_STATE_PLAYING       = 1 << 13,
  OV_LOCAL_STATE_PAUSED        = 1 << 14,
  OV_LOCAL_STATE_STOPPED       = 1 << 15,
};

enum _OvRemotePeerState {
  OV_REMOTE_STATE_NULL,
  OV_REMOTE_STATE_FAILED,
  OV_REMOTE_STATE_ALLOCATED,

  OV_REMOTE_STATE_READY,
  OV_REMOTE_STATE_PLAYING,
  OV_REMOTE_STATE_PAUSED,
};

/* Represents us; the library and the client implementing this local */
struct _OvLocalPeer {
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

  OvLocalPeerState state;

  /* < private > */
  OvLocalPeerPriv *priv;
  gpointer _gst_reserved[GST_PADDING];
};

/* Represents a remote local */
struct _OvRemotePeer {
  OvLocalPeer *local;

  /* Receive pipeline */
  GstElement *receive;
  /* Address of remote peer */
  GInetSocketAddress *addr;
  /* String representation (for logging, etc) */
  gchar *addr_s;
  /* Unique id string representing this host
   * Retrieved from the peer during negotiation */
  gchar *id;

  OvRemotePeerState state;

  /* < private > */
  OvRemotePeerPriv *priv;
};

/* Represents a discovered peer */
struct _OvDiscoveredPeer {
  /* Address of the discovered peer */
  GInetSocketAddress *addr;
  /* String representation; contains port if non-default port */
  gchar *addr_s;

  /* Monotonic time when this peer was discovered */
  gint64 discover_time;
};

/* Local peer (us) */
OvLocalPeer*  ov_local_peer_new               (const gchar *iface,
                                               guint16 port);
void                ov_local_peer_free              (OvLocalPeer *local);
void                ov_local_peer_add_remote        (OvLocalPeer *local,
                                                     OvRemotePeer *remote);
/* Asynchronously negotiate with all setup remote peers */
gboolean            ov_local_peer_negotiate_async   (OvLocalPeer *local,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer callback_data);
gboolean            ov_local_peer_negotiate_finish  (OvLocalPeer *local,
                                                     GAsyncResult *result,
                                                     GError **error);
gboolean            ov_local_peer_negotiate_stop    (OvLocalPeer *local);
gboolean            ov_local_peer_start             (OvLocalPeer *local);
void                ov_local_peer_stop              (OvLocalPeer *local);

/* Device discovery */
GList*              ov_local_peer_get_video_devices (OvLocalPeer *local);
gboolean            ov_local_peer_set_video_device  (OvLocalPeer *local,
                                                     GstDevice *device);

/* Peer discovery */
GSource*            ov_local_peer_find_remotes_create_source  (OvLocalPeer *local,
                                                               GCancellable *cancellable,
                                                               OvRemoteFoundCallback callback,
                                                               gpointer callback_data,
                                                               GError **error);

/* Remote peers */
OvRemotePeer*       ov_remote_peer_new                (OvLocalPeer *local,
                                                       GInetSocketAddress *addr);
OvRemotePeer*       ov_remote_peer_new_from_string    (OvLocalPeer *local,
                                                       const gchar *addr_s);
void                ov_remote_peer_free               (OvRemotePeer *remote);
gpointer            ov_remote_peer_add_gtkglsink      (OvRemotePeer *remote);
void                ov_remote_peer_pause              (OvRemotePeer *remote);
void                ov_remote_peer_resume             (OvRemotePeer *remote);
void                ov_remote_peer_remove             (OvRemotePeer *remote);

OvRemotePeer*       ov_local_peer_get_remote_by_id    (OvLocalPeer *local,
                                                       const gchar *peer_id);

/* Discovered peers */
OvDiscoveredPeer*   ov_discovered_peer_new    (GInetSocketAddress *addr);
void                ov_discovered_peer_free   (OvDiscoveredPeer *peer);

G_END_DECLS

#endif /* __OV_LIB_H__ */
