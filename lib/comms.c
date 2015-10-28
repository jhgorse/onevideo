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

#include "lib.h"
#include "lib-priv.h"
#include "comms.h"

#include <string.h>

static const struct {
  OneVideoTcpMsgType type;
  const char *type_string;
  const char *type_variant_string;
} type_strings[] = {
  /* Format: msg_id */
  {ONE_VIDEO_TCP_MSG_TYPE_ACK,              "acknowledged",       "x"},

  /* Format: (msg_id, error string) */
  {ONE_VIDEO_TCP_MSG_TYPE_ERROR,            "error",              "(xs)"},

  /* Format: (call_id, error string) */
  {ONE_VIDEO_TCP_MSG_TYPE_ERROR_CALL,       "error during call",  "(xs)"},

  /* Format:
   * (call_id, arecv_rtcprr_port, vrecv_rtcprr_port,
   *  send_acaps, send_vcaps, recv_acaps, recv_vcaps,
   *  [(remote_peer1, arecv_port1, arecv_rtcpsr_port1, vrecv_port1, vrecv_rtcpsr_port1),
   *   (remote_peer2, arecv_port2, arecv_rtcpsr_port2, vrecv_port2, vrecv_rtcpsr_port2),
   *   ...])
   *
   *   Note that the rtcprr ports are shared between all peers */
  {ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS,       "reply media caps",   "(xuussssa(suuuu))"},

  /* Format: call_id, peer_id_str
   * peer_id_str is of the form "address:tcp_port" */
  {ONE_VIDEO_TCP_MSG_TYPE_START_NEGOTIATE,  "start negotiating",  "(xs)"},

  /* Format: call_id, peer_id_str
   * Can be sent by any peer during the negotiation process to cancel it.
   * The call initiator sends it to all peers and the other peers send it
   * only to the call initiator. */
  {ONE_VIDEO_TCP_MSG_TYPE_CANCEL_NEGOTIATE, "cancel negotiating", "(xs)"},

  /* Format: (call_id, [remote_peer1, remote_peer2, ...]) */
  {ONE_VIDEO_TCP_MSG_TYPE_QUERY_CAPS,       "query media caps",   "(xas)"},

  /* Format:
   * (call_id, send_acaps, send_vcaps,
   *  [(remote_peer1, recv_acaps1, recv_vcaps1,
   *    arecv_port1, arecv_rtcpsr_port1, arecv_rtcprr_port1,
   *    vrecv_port1, vrecv_rtcpsr_port1, vrecv_rtcprr_port1),
   *   (remote_peer2, recv_acaps2, recv_vcaps2,
   *    arecv_port2, arecv_rtcpsr_port2, arecv_rtcprr_port1,
   *    vrecv_port2, vrecv_rtcpsr_port2),
   *   ...]) */
  {ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS,     "call details",       "(xssa(sssuuuuuu))"},

  /* Format: (call_id, [remote_peer1, remote_peer2, ...]) */
  {ONE_VIDEO_TCP_MSG_TYPE_START_CALL,       "start call",         "(xas)"},

  /* Format: call_id, peer_id_str */
  {ONE_VIDEO_TCP_MSG_TYPE_PAUSE_CALL,       "pause call",         "(xs)"},

  /* Format: call_id, peer_id_str */
  {ONE_VIDEO_TCP_MSG_TYPE_RESUME_CALL,      "resume call",        "(xs)"},

  /* Format: call_id, peer_id_str */
  {ONE_VIDEO_TCP_MSG_TYPE_END_CALL,         "end call",           "(xs)"},

  {0}
};

const char *
one_video_tcp_msg_type_to_string (OneVideoTcpMsgType type, guint version)
{
  int ii, len;

  len = sizeof (one_video_versions) / sizeof (one_video_versions[0]);

  for (ii = 0; ii < len; ii++)
    if (version == one_video_versions[ii])
      goto supported;

  GST_ERROR ("Unsupported version: %u", version);
  return NULL;

supported:
  for (ii = 0; type_strings[ii].type; ii++)
    if (type_strings[ii].type == type)
      return type_strings[ii].type_string;

  return "unknown message type";
}

const char *
one_video_tcp_msg_type_to_variant_type (OneVideoTcpMsgType type, guint version)
{
  int ii, len;

  len = sizeof (one_video_versions) / sizeof (one_video_versions[0]);

  for (ii = 0; ii < len; ii++)
    if (version == one_video_versions[ii])
      goto supported;

  GST_ERROR ("Unsupported version: %u", version);
  return NULL;

supported:
  for (ii = 0; type_strings[ii].type; ii++)
    if (type_strings[ii].type == type)
      return type_strings[ii].type_variant_string;

  GST_ERROR ("Unknown message type: %u", type);
  return NULL;
}

OneVideoTcpMsg *
one_video_tcp_msg_new (OneVideoTcpMsgType type, GVariant * data)
{
  OneVideoTcpMsg *msg;

  msg = g_new0 (OneVideoTcpMsg, 1);
  msg->version = ONE_VIDEO_TCP_MAX_VERSION;
  /* FIXME: Collisions? */
  msg->id = g_get_monotonic_time ();
  msg->type = type;

  if (data != NULL) {
    msg->variant = g_variant_ref_sink (data);
    msg->data = g_variant_get_data (msg->variant);
    msg->size = g_variant_get_size (msg->variant);
  }

  return msg;
}

void
one_video_tcp_msg_free (OneVideoTcpMsg * msg)
{
  if (!msg)
    return;
  g_variant_unref (msg->variant);
  g_free (msg);
}

OneVideoTcpMsg *
one_video_tcp_msg_new_error (guint64 id, const gchar * error_msg)
{
  OneVideoTcpMsg *msg;
  const gchar *variant_type;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_ERROR, ONE_VIDEO_TCP_MIN_VERSION);
  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_ERROR,
      g_variant_new (variant_type, id, error_msg));

  return msg;
}

OneVideoTcpMsg *
one_video_tcp_msg_new_error_call (guint64 id, const gchar * error_msg)
{
  OneVideoTcpMsg *msg;
  const gchar *variant_type;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_ERROR_CALL, ONE_VIDEO_TCP_MIN_VERSION);
  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_ERROR_CALL,
      g_variant_new (variant_type, id, error_msg));

  return msg;
}

OneVideoTcpMsg *
one_video_tcp_msg_new_ack (guint64 id)
{
  OneVideoTcpMsg *msg;
  const gchar *variant_type;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_ACK, ONE_VIDEO_TCP_MIN_VERSION);
  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_ACK,
      g_variant_new (variant_type, id));

  return msg;
}

OneVideoTcpMsg *
one_video_tcp_msg_new_start_negotiate (guint64 id, gchar * local_addr_s)
{
  OneVideoTcpMsg *msg;
  const gchar *variant_type;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_START_NEGOTIATE, ONE_VIDEO_TCP_MIN_VERSION);
  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_START_NEGOTIATE,
      g_variant_new (variant_type, id, local_addr_s));

  return msg;
}

OneVideoTcpMsg *
one_video_tcp_msg_new_cancel_negotiate (guint64 id, gchar * local_addr_s)
{
  OneVideoTcpMsg *msg;
  const gchar *variant_type;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_CANCEL_NEGOTIATE, ONE_VIDEO_TCP_MIN_VERSION);
  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_CANCEL_NEGOTIATE,
      g_variant_new (variant_type, id, local_addr_s));

  return msg;
}

gchar *
one_video_tcp_msg_print (OneVideoTcpMsg * msg)
{
  gchar *tmp;

  tmp = g_variant_print (msg->variant, FALSE);

  return tmp;
}

gboolean
one_video_tcp_msg_write_to_stream (GOutputStream * output,
    OneVideoTcpMsg * msg, GCancellable * cancellable, GError ** error)
{
  gchar *tmp;
  gboolean ret;
  guint32 tmp1;
  guint64 tmp2;
  GVariant *variant;

  tmp = one_video_tcp_msg_print (msg);
  GST_DEBUG ("Writing msg type %s to the network; contents: %s",
      one_video_tcp_msg_type_to_string (msg->type, ONE_VIDEO_TCP_MAX_VERSION),
      tmp);
  g_free (tmp);

  /* TODO: Error checking */
  /* Write header */
  tmp1 = GUINT32_TO_BE (msg->version);
  if (!g_output_stream_write_all (output, &tmp1, sizeof (tmp1), NULL,
        cancellable, error))
    goto err;
  tmp2 = GUINT64_TO_BE (msg->id);
  if (!g_output_stream_write_all (output, &tmp2, sizeof (tmp2), NULL,
        cancellable, error))
    goto err;
  tmp1 = GUINT32_TO_BE (msg->type);
  if (!g_output_stream_write_all (output, &tmp1, sizeof (tmp1), NULL,
        cancellable, error))
    goto err;
  tmp1 = GUINT32_TO_BE (msg->size);
  if (!g_output_stream_write_all (output, &tmp1, sizeof (tmp1), NULL,
        cancellable, error))
    goto err;

  if (msg->size > 0) {
    guint32 size;

    /* Network data is always big endian */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    variant = g_variant_byteswap (msg->variant);
#elif G_BYTE_ORDER == G_BIG_ENDIAN
    variant = g_variant_get_normal_form (msg->variant);
#else
#error "Unsupported byte order: " STR(G_BYTE_ORDER)
#endif
    size = g_variant_get_size (variant);
    g_assert (msg->size == size);

    /* Write data */
    ret = g_output_stream_write_all (output, g_variant_get_data (variant),
        size, NULL, cancellable, error);
    g_variant_unref (variant);
  }

out:
  return ret;
err:
  tmp = one_video_tcp_msg_print (msg);
  GST_ERROR ("Unable to write header for msg: %s", tmp);
  g_free (tmp);
  goto out;
}

gboolean
one_video_tcp_msg_write_new_error_to_stream (GOutputStream * output,
    guint64 id, const gchar * error_msg, GCancellable * cancellable,
    GError ** error)
{
  gboolean ret;
  OneVideoTcpMsg *msg;

  msg = one_video_tcp_msg_new_error (id, error_msg);
  ret = one_video_tcp_msg_write_to_stream (output, msg, cancellable, error);
  one_video_tcp_msg_free (msg);

  return ret;
}

gboolean
one_video_tcp_msg_write_new_ack_to_stream (GOutputStream * output,
    guint64 id, GCancellable * cancellable, GError ** error)
{
  gboolean ret;
  OneVideoTcpMsg *msg;

  msg = one_video_tcp_msg_new_ack (id);
  ret = one_video_tcp_msg_write_to_stream (output, msg, cancellable, error);
  one_video_tcp_msg_free (msg);

  return ret;
}

/* Does a blocking read for the header */
gboolean
one_video_tcp_msg_read_header_from_stream (GInputStream * input,
    OneVideoTcpMsg * msg, GCancellable * cancellable, GError ** error)
{
  gboolean ret;
  char tmp[ONE_VIDEO_TCP_MSG_HEADER_SIZE];
  gsize bytes_read;

  g_return_val_if_fail (msg != NULL, FALSE);

  /* Read message length prefix */
  ret = g_input_stream_read_all (input, tmp, ONE_VIDEO_TCP_MSG_HEADER_SIZE,
      &bytes_read, cancellable, error);
  if (!ret)
    return FALSE;

  if (bytes_read < sizeof (tmp)) {
    GST_ERROR ("Unable to read message length prefix, got EOS");
    return FALSE;
  }

  msg->version = GST_READ_UINT32_BE (tmp);

  if (msg->version != 1) {
    GST_ERROR ("Message version %u is not supported", msg->version);
    return FALSE;
  }

  msg->id = GST_READ_UINT64_BE (tmp + 4);
  msg->type = GST_READ_UINT32_BE (tmp + 12);
  msg->size = GST_READ_UINT32_BE (tmp + 16);

  return TRUE;
}

/* Does a blocking read and returns a (transfer-full) buffer with the contents
 * of the body */
gboolean
one_video_tcp_msg_read_body_from_stream (GInputStream * input,
    OneVideoTcpMsg * msg, GCancellable * cancellable, GError ** error)
{
  GBytes *bytes;
  GVariant *variant;
  const gchar *variant_type;

  g_return_val_if_fail (msg != NULL, FALSE);

  /* FIXME: Add a timeout that cancels if we don't get the data for a while */
  bytes = g_input_stream_read_bytes (input, msg->size, cancellable, error);

  if (g_bytes_get_size (bytes) < msg->size) {
    g_bytes_unref (bytes);
    GST_ERROR ("Unable to finish reading incoming data due to EOS");
    return FALSE;
  }

  variant_type = one_video_tcp_msg_type_to_variant_type (msg->type,
      msg->version);
  variant = g_variant_new_from_bytes (G_VARIANT_TYPE (variant_type), bytes,
      FALSE);
  g_variant_ref_sink (variant);
  g_bytes_unref (bytes);

  /* Network data is always big endian */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  msg->variant = g_variant_byteswap (variant);
#elif G_BYTE_ORDER == G_BIG_ENDIAN
  msg->variant = g_variant_get_normal_form (variant);
#else
#error "Unsupported byte order: " STR(G_BYTE_ORDER)
#endif
  g_variant_unref (variant);

  msg->data = g_variant_get_data (msg->variant);
  msg->size = g_variant_get_size (msg->variant);

  return TRUE;
}

OneVideoTcpMsg *
one_video_tcp_msg_read_from_stream (GInputStream * input,
    GCancellable * cancellable, GError ** error)
{
  gboolean ret;
  OneVideoTcpMsg *msg;

  msg = g_new0 (OneVideoTcpMsg, 1);

  ret = one_video_tcp_msg_read_header_from_stream (input, msg, cancellable,
      error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message length prefix: %s",
        error ? (*error)->message : "Unknown error");
    goto err_no_body;
  }

  GST_DEBUG ("Reading message type %s and version %u of length %u bytes",
      one_video_tcp_msg_type_to_string (msg->type, msg->version),
      msg->version, msg->size);

  if (msg->size == 0)
    goto out;

  /* Read the rest of the message */
  ret = one_video_tcp_msg_read_body_from_stream (input, msg, cancellable,
      error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message body: %s", (*error)->message);
    goto err_no_body;
  }

out:
  return msg;

err_no_body:
  g_free (msg);
  msg = NULL;
  goto out;
}
