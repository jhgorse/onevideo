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

#ifndef __OV_DISCOVERY_H__
#define __OV_DISCOVERY_H__

#include <glib.h>
#include <gio/gio.h>

#include "comms.h"
#include "ov-local-peer.h"

G_BEGIN_DECLS

typedef enum _OvUdpMsgType OvUdpMsgType;

enum _OvUdpMsgType {
  OV_UDP_MSG_TYPE_UNKNOWN,

  /* Queries */
  OV_UDP_MSG_TYPE_MULTICAST_DISCOVER   = 100,

  /* Replies */
  OV_UDP_MSG_TYPE_UNICAST_HI_THERE     = 200,
};

typedef struct _OvUdpMsg OvUdpMsg;

struct _OvUdpMsg {
  guint32 version;
  guint64 id;
  guint32 type; /* OvUdpMsgType */
  guint32 size; /* size of *data */
  gchar *data;
};

/* Max size of our outgoing UDP messages */
#define OV_UDP_MAX_SIZE 4096

/* Size of the metadata sent with a OvUdpMsg */
#define OV_UDP_MSG_HEADER_SIZE 20

/* Ordered from oldest to newest */
#define OV_UDP_MIN_VERSION ov_versions[0]
#define OV_UDP_MAX_VERSION ov_versions[0]

gboolean        on_incoming_udp_message         (GSocket *socket,
                                                 GIOCondition condition,
                                                 OvLocalPeer *local);

void            ov_udp_msg_free                 (OvUdpMsg *msg);
OvUdpMsg*       ov_udp_msg_new                  (OvUdpMsgType type,
                                                 gchar *data,
                                                 gsize size);

gboolean        ov_udp_msg_read_message_from   (OvUdpMsg *msg,
                                                GSocketAddress **addr,
                                                GSocket *socket,
                                                GCancellable *cancellable,
                                                GError **error);
gboolean        ov_udp_msg_send_to_from        (OvUdpMsg *msg,
                                                GSocketAddress *to,
                                                GSocketAddress *from,
                                                GCancellable *cancellable,
                                                GError **error);

gboolean        ov_discovery_send_multicast_discover  (OvLocalPeer *local,
                                                       GCancellable *cancellable,
                                                       GError **error);

G_END_DECLS

#endif /* __OV_DISCOVERY_H__ */
