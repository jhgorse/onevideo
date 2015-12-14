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

#ifndef __OV_COMMS_H__
#define __OV_COMMS_H__

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OV_TCP_TIMEOUT 5

/* Zeroconf is 224.0.0.251 on port 53. We use the same address but the port is
 * OV_DEFAULT_COMM_PORT.
 * See: https://en.wikipedia.org/wiki/Multicast_address#IPv4 */
#define OV_MULTICAST_GROUP "224.0.0.251"

typedef enum _OvTcpMsgType OvTcpMsgType;

enum _OvTcpMsgType {
  /* Queries/Events */
  OV_TCP_MSG_TYPE_START_NEGOTIATE = 100,
  OV_TCP_MSG_TYPE_CANCEL_NEGOTIATE,
  OV_TCP_MSG_TYPE_QUERY_CAPS,
  OV_TCP_MSG_TYPE_CALL_DETAILS,
  OV_TCP_MSG_TYPE_START_CALL,
  OV_TCP_MSG_TYPE_PAUSE_CALL,
  OV_TCP_MSG_TYPE_RESUME_CALL,
  OV_TCP_MSG_TYPE_END_CALL,

  /* Replies */
  OV_TCP_MSG_TYPE_ACK = 200,
  OV_TCP_MSG_TYPE_ERROR,
  OV_TCP_MSG_TYPE_ERROR_CALL,
  OV_TCP_MSG_TYPE_OK_NEGOTIATE,
  OV_TCP_MSG_TYPE_REPLY_CAPS,

  /* Both queries and replies */
};

typedef struct _OvTcpMsg OvTcpMsg;

struct _OvTcpMsg {
  guint32 version;
  guint64 id;
  guint32 type; /* OvTcpMsgType */
  guint32 size;
  const gchar *data;

  GVariant *variant; /* *data is the wire-friendly repr of *variant */
};

/* Size of the metadata sent with a OvTcpMsg */
#define OV_TCP_MSG_HEADER_SIZE 20

/* Ordered from oldest to newest */
static const guint32 ov_versions[] = {1,};
#define OV_TCP_MIN_VERSION ov_versions[0]
#define OV_TCP_MAX_VERSION ov_versions[0]

OvTcpMsg*     ov_tcp_msg_new                    (OvTcpMsgType type,
                                                 GVariant *data);
void          ov_tcp_msg_free                   (OvTcpMsg *msg);
OvTcpMsg*     ov_tcp_msg_new_error              (guint64 id,
                                                 const gchar *error_msg);
OvTcpMsg*     ov_tcp_msg_new_error_call         (guint64 id,
                                                 const gchar *error_msg);
OvTcpMsg*     ov_tcp_msg_new_ack                (guint64 id);
OvTcpMsg*     ov_tcp_msg_new_start_negotiate    (guint64 call_id,
                                                 gchar *local_id,
                                                 guint16 local_port);
OvTcpMsg*     ov_tcp_msg_new_ok_negotiate       (guint64 call_id,
                                                 gchar *local_id);
OvTcpMsg*     ov_tcp_msg_new_cancel_negotiate   (guint64 call_id,
                                                 gchar *local_id);

gchar*        ov_tcp_msg_print                  (OvTcpMsg *msg);

gboolean      ov_tcp_msg_write_to_stream              (GOutputStream *output,
                                                       OvTcpMsg *msg,
                                                       GCancellable *cancellable,
                                                       GError **error);
gboolean      ov_tcp_msg_write_new_error_to_stream    (GOutputStream *output,
                                                       guint64 id,
                                                       const gchar *error_msg,
                                                       GCancellable *cancellable,
                                                       GError **error);
gboolean      ov_tcp_msg_write_new_ack_to_stream      (GOutputStream *output,
                                                       guint64 id,
                                                       GCancellable *cancellable,
                                                       GError **error);

gboolean      ov_tcp_msg_read_header_from_stream      (GInputStream *input,
                                                       OvTcpMsg *msg,
                                                       GCancellable *cancellable,
                                                       GError **error);
gboolean      ov_tcp_msg_read_body_from_stream        (GInputStream *input,
                                                       OvTcpMsg *msg,
                                                       GCancellable *cancellable,
                                                       GError **error);
OvTcpMsg*     ov_tcp_msg_read_from_stream             (GInputStream *input,
                                                       GCancellable *cancellable,
                                                       GError **error);

const gchar*  ov_tcp_msg_type_to_string               (OvTcpMsgType type,
                                                       guint32 version);
const gchar*  ov_tcp_msg_type_to_variant_type         (OvTcpMsgType type,
                                                       guint32 version);

G_END_DECLS

#endif /* __OV_COMMS_H__ */
