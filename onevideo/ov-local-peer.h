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

#ifndef __OV_LOCAL_PEER_H__
#define __OV_LOCAL_PEER_H__

#include "ov-peer.h"
#include "ov-discovered-peer.h"

#include <gst/gst.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OV_DEFAULT_COMM_PORT 5000

#define OV_TYPE_LOCAL_PEER ov_local_peer_get_type ()
G_DECLARE_DERIVABLE_TYPE (OvLocalPeer, ov_local_peer, OV, LOCAL_PEER, OvPeer)

typedef struct _OvLocalPeerPrivate  OvLocalPeerPrivate;

typedef enum _OvLocalPeerState    OvLocalPeerState;

struct _OvLocalPeerClass {
  GObjectClass parent_class;

  /* signals */
  void (*discovery_sent)            (OvLocalPeer *local);
  void (*peer_discovered)           (OvLocalPeer *local,
                                     OvDiscoveredPeer *peer);

  void (*negotiate_incoming)        (OvLocalPeer *local,
                                     OvPeer *peer);

  void (*negotiate_started)         (OvLocalPeer *local);
  void (*negotiate_skipped_remote)  (OvLocalPeer *local,
                                     OvPeer *skipped,
                                     GError *error);
  void (*negotiate_finished)        (OvLocalPeer *local);
  void (*negotiate_aborted)         (OvLocalPeer *local,
                                     GError *error);
  void (*call_remote_gone)          (OvLocalPeer *local,
                                     OvPeer *remote,
                                     gboolean timedout);
  void (*call_all_remotes_gone)     (OvLocalPeer *local);

  /* Padding to allow up to 12 new virtual functions without breaking ABI */
  gpointer padding[12];
};

enum _OvLocalPeerState {
  OV_LOCAL_STATE_NULL           = 0,

  /**~ Special states ~**/
  /* These are ORed with other states to signal a special state */
  /* This is ORed with the current state to signal failure */
  OV_LOCAL_STATE_FAILED         = 1 << 0,
  OV_LOCAL_STATE_TIMEOUT        = 1 << 1,
  /* One of these is ORed with the current state when we're taking on the role
   * of either negotiator or negotiatee */
  OV_LOCAL_STATE_NEGOTIATOR     = 1 << 7,
  OV_LOCAL_STATE_NEGOTIATEE     = 1 << 8,

  /* Ordinary states. These are not ORed with each other. */
  OV_LOCAL_STATE_STARTED        = 1 << 9,

  OV_LOCAL_STATE_NEGOTIATING    = 1 << 10,
  OV_LOCAL_STATE_NEGOTIATED     = 1 << 11,

  OV_LOCAL_STATE_READY          = 1 << 12,
  OV_LOCAL_STATE_PLAYING        = 1 << 13,
  OV_LOCAL_STATE_PAUSED         = 1 << 14,
  OV_LOCAL_STATE_STOPPED        = 1 << 15,
};

OvLocalPeer*          ov_local_peer_new           (const gchar *iface,
                                                   guint16 port);
OvLocalPeerState      ov_local_peer_get_state     (OvLocalPeer *self);
void                  ov_local_peer_set_volume    (OvLocalPeer *self,
                                                   gdouble volume);

G_END_DECLS

#endif /* __OV_LOCAL_PEER_H__ */
