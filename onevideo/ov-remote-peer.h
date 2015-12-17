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

#ifndef __OV_REMOTE_PEER_H__
#define __OV_REMOTE_PEER_H__

#include <gst/gst.h>
#include <gio/gio.h>
#include "ov-local-peer.h"
#include "ov-discovered-peer.h"

G_BEGIN_DECLS

typedef enum _OvRemotePeerState   OvRemotePeerState;

enum _OvRemotePeerState {
  OV_REMOTE_STATE_NULL,
  OV_REMOTE_STATE_FAILED,
  OV_REMOTE_STATE_ALLOCATED,

  OV_REMOTE_STATE_READY,
  OV_REMOTE_STATE_PLAYING,
  OV_REMOTE_STATE_PAUSED,
};

typedef struct _OvRemotePeer        OvRemotePeer;
typedef struct _OvRemotePeerPrivate OvRemotePeerPrivate;

/* Represents a remote peer */
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
  OvRemotePeerPrivate *priv;
};

OvRemotePeer*       ov_remote_peer_new                (OvLocalPeer *local,
                                                       GInetSocketAddress *addr);
OvRemotePeer*       ov_remote_peer_new_from_string    (OvLocalPeer *local,
                                                       const gchar *addr_s);
void                ov_remote_peer_free               (OvRemotePeer *remote);

G_END_DECLS

#endif /* __OV_REMOTE_PEER_H__ */
