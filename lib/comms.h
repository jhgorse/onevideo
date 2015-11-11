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

#ifndef __ONE_VIDEO_COMMS_H__
#define __ONE_VIDEO_COMMS_H__

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define ONE_VIDEO_TCP_TIMEOUT 5

typedef enum _OneVideoTcpMsgType OneVideoTcpMsgType;

enum _OneVideoTcpMsgType {
  /* Queries/Events */
  ONE_VIDEO_TCP_MSG_TYPE_START_NEGOTIATE = 100,
  ONE_VIDEO_TCP_MSG_TYPE_CANCEL_NEGOTIATE,
  ONE_VIDEO_TCP_MSG_TYPE_QUERY_CAPS,
  ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS,
  ONE_VIDEO_TCP_MSG_TYPE_START_CALL,
  ONE_VIDEO_TCP_MSG_TYPE_PAUSE_CALL,
  ONE_VIDEO_TCP_MSG_TYPE_RESUME_CALL,
  ONE_VIDEO_TCP_MSG_TYPE_END_CALL,

  /* Replies */
  ONE_VIDEO_TCP_MSG_TYPE_ACK = 200,
  ONE_VIDEO_TCP_MSG_TYPE_ERROR,
  ONE_VIDEO_TCP_MSG_TYPE_ERROR_CALL,
  ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS,

  /* Both queries and replies */
  ONE_VIDEO_TCP_MSG_TYPE_HELLO = 300,     /* Hello with basic client info (NOT IMPLEMENTED YET) */
};

typedef struct _OneVideoTcpMsg OneVideoTcpMsg;

struct _OneVideoTcpMsg {
  guint32 version;
  guint64 id;
  guint32 type; /* OneVideoTcpMsgType */
  guint32 size;
  const gchar *data;

  GVariant *variant; /* *data is the wire-friendly repr of *variant */
};

/* Size of the metadata sent with a OneVideoTcpMsg */
#define ONE_VIDEO_TCP_MSG_HEADER_SIZE 20

/* Ordered from oldest to newest */
static const guint32 one_video_versions[] = {1,};
#define ONE_VIDEO_TCP_MIN_VERSION one_video_versions[0]
#define ONE_VIDEO_TCP_MAX_VERSION one_video_versions[0]

OneVideoTcpMsg* one_video_tcp_msg_new                   (OneVideoTcpMsgType type,
                                                         GVariant *data);
void            one_video_tcp_msg_free                  (OneVideoTcpMsg *msg);
OneVideoTcpMsg* one_video_tcp_msg_new_error             (guint64 id,
                                                         const gchar *error_msg);
OneVideoTcpMsg* one_video_tcp_msg_new_error_call        (guint64 id,
                                                         const gchar *error_msg);
OneVideoTcpMsg* one_video_tcp_msg_new_ack               (guint64 id);
OneVideoTcpMsg* one_video_tcp_msg_new_start_negotiate   (guint64 id,
                                                         gchar *local_addr_s);
OneVideoTcpMsg* one_video_tcp_msg_new_cancel_negotiate  (guint64 id,
                                                         gchar *local_addr_s);

gchar*          one_video_tcp_msg_print                 (OneVideoTcpMsg *msg);

gboolean        one_video_tcp_msg_write_to_stream           (GOutputStream *output,
                                                             OneVideoTcpMsg *msg,
                                                             GCancellable *cancellable,
                                                             GError **error);
gboolean        one_video_tcp_msg_write_new_error_to_stream (GOutputStream *output,
                                                             guint64 id,
                                                             const gchar *error_msg,
                                                             GCancellable *cancellable,
                                                             GError **error);
gboolean        one_video_tcp_msg_write_new_ack_to_stream   (GOutputStream *output,
                                                             guint64 id,
                                                             GCancellable *cancellable,
                                                             GError **error);

gboolean        one_video_tcp_msg_read_header_from_stream (GInputStream *input,
                                                           OneVideoTcpMsg *msg,
                                                           GCancellable *cancellable,
                                                           GError **error);
gboolean        one_video_tcp_msg_read_body_from_stream   (GInputStream *input,
                                                           OneVideoTcpMsg *msg,
                                                           GCancellable *cancellable,
                                                           GError **error);
OneVideoTcpMsg* one_video_tcp_msg_read_from_stream        (GInputStream *input,
                                                           GCancellable *cancellable,
                                                           GError **error);

const gchar*    one_video_tcp_msg_type_to_string       (OneVideoTcpMsgType type,
                                                        guint32 version);
const gchar*    one_video_tcp_msg_type_to_variant_type (OneVideoTcpMsgType type,
                                                        guint32 version);

G_END_DECLS

#endif /* __ONE_VIDEO_COMMS_H__ */
