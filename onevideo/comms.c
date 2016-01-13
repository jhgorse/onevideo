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
  OvTcpMsgType type;
  const char *type_string;
  const char *type_variant_string;
} type_strings[] = {
  /* Format: msg_id */
  {OV_TCP_MSG_TYPE_ACK,              "acknowledged",       "x"},

  /* Format: (msg_id, error string) */
  {OV_TCP_MSG_TYPE_ERROR,            "error",              "(xs)"},

  /* Format: (call_id, error string) */
  {OV_TCP_MSG_TYPE_ERROR_CALL,       "error during call",  "(xs)"},

  /* Each peer replies with this structure which contains the ports that it will
   * receive RTP/RTCP on (RR ports are common for all other peers, but SR and
   * data ports are peer-specific), the a/v caps that it supports sending, and
   * the a/v caps that it supports receiving
   *
   * Format:
   * (call_id, arecv_rtcprr_port, vrecv_rtcprr_port,
   *  supported_send_acaps, supported_send_vcaps,
   *  supported_recv_acaps, supported_recv_vcaps,
   *   # This peer has allocated destination ports *_port1 to receive RTP data
   *   # and RTCP SRs from peer1
   *  [(peer1_id, arecv_port1, arecv_rtcpsr_port1, vrecv_port1, vrecv_rtcpsr_port1),
   *   # This peer has allocated destination ports *_port2 to receive RTP data
   *   # and RTCP SRs from peer2
   *   (peer2_id, arecv_port2, arecv_rtcpsr_port2, vrecv_port2, vrecv_rtcpsr_port2),
   *   ...])
   *
   *   Note that the rtcprr ports are shared between all peers */
  {OV_TCP_MSG_TYPE_REPLY_CAPS,       "reply media caps",   "(xqqssssa(sqqqq))"},

  /* Format: (call_id, peer_id_str, peer_port)
   * peer_id_str is of the form "$hostname:$port-$guid" */
  {OV_TCP_MSG_TYPE_START_NEGOTIATE,  "start negotiating",  "(xsq)"},

  /* Format: (call_id, peer_id_str)
   * This is sent in reply to a START_NEGOTIATE */
  {OV_TCP_MSG_TYPE_OK_NEGOTIATE,     "ok, can negotiate",  "(xs)"},

  /* Format: (call_id, peer_id_str)
   * Can be sent by any peer during the negotiation process to cancel it.
   * The call initiator sends it to all peers and the other peers send it
   * only to the call initiator. */
  {OV_TCP_MSG_TYPE_CANCEL_NEGOTIATE, "cancel negotiating", "(xs)"},

  /* Format: (call_id, [(peer1_id, peer1_addr), (peer2_d, peer2_addr), ...])
   *
   * The peer_addresses here are as resolved by the negotiator */
  {OV_TCP_MSG_TYPE_QUERY_CAPS,       "query media caps",   "(xa(ss))"},

  /* Distribute to each peer the details of which format to send audio/video
   * data in, what format each remote will send data in to this peer, and what
   * destination ports to use for each remote while sending RTP and RTCP
   * (these are the ports that each remote will receive RTP/RTCP on)
   *
   * Format:
   * (call_id, send_acaps, send_vcaps,
   *   # peer1_id will send (a|v)caps1, and will recv on *_port1 ports
   *  [(peer1_id, send_acaps1, send_vcaps1,
   *    arecv_port1, arecv_rtcpsr_port1, arecv_rtcprr_port1,
   *    vrecv_port1, vrecv_rtcpsr_port1, vrecv_rtcprr_port1),
   *   # peer2_id will send (a|v)caps2, and will recv on *_port2 ports
   *   (peer2_id, send_acaps2, send_vcaps2,
   *    arecv_port2, arecv_rtcpsr_port2, arecv_rtcprr_port2,
   *    vrecv_port2, vrecv_rtcpsr_port2, vrecv_rtcprr_port2),
   *   ...]) */
  {OV_TCP_MSG_TYPE_CALL_DETAILS,     "call details",       "(xssa(sssqqqqqq))"},

  /* Format: (call_id, [peer1_id, peer2_id, ...]) */
  {OV_TCP_MSG_TYPE_START_CALL,       "start call",         "(xas)"},

  /* Format: call_id, peer_id_str */
  {OV_TCP_MSG_TYPE_PAUSE_CALL,       "pause call",         "(xs)"},

  /* Format: call_id, peer_id_str */
  {OV_TCP_MSG_TYPE_RESUME_CALL,      "resume call",        "(xs)"},

  /* Format: call_id, peer_id_str */
  {OV_TCP_MSG_TYPE_END_CALL,         "end call",           "(xs)"},

  {0}
};

const char *
ov_tcp_msg_type_to_string (OvTcpMsgType type, guint version)
{
  int ii, len;

  len = sizeof (ov_versions) / sizeof (ov_versions[0]);

  for (ii = 0; ii < len; ii++)
    if (version == ov_versions[ii])
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
ov_tcp_msg_type_to_variant_type (OvTcpMsgType type, guint version)
{
  int ii, len;

  len = sizeof (ov_versions) / sizeof (ov_versions[0]);

  for (ii = 0; ii < len; ii++)
    if (version == ov_versions[ii])
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

OvTcpMsg *
ov_tcp_msg_new (OvTcpMsgType type, GVariant * data)
{
  OvTcpMsg *msg;

  msg = g_new0 (OvTcpMsg, 1);
  msg->version = OV_TCP_MAX_VERSION;
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
ov_tcp_msg_free (OvTcpMsg * msg)
{
  if (!msg)
    return;
  if (msg->variant)
    g_variant_unref (msg->variant);
  g_free (msg);
}

OvTcpMsg *
ov_tcp_msg_new_error (guint64 id, const gchar * error_msg)
{
  OvTcpMsg *msg;
  const gchar *variant_type;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_ERROR, OV_TCP_MIN_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_ERROR,
      g_variant_new (variant_type, id, error_msg));

  return msg;
}

OvTcpMsg *
ov_tcp_msg_new_error_call (guint64 id, const gchar * error_msg)
{
  OvTcpMsg *msg;
  const gchar *variant_type;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_ERROR_CALL, OV_TCP_MIN_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_ERROR_CALL,
      g_variant_new (variant_type, id, error_msg));

  return msg;
}

OvTcpMsg *
ov_tcp_msg_new_ack (guint64 id)
{
  OvTcpMsg *msg;
  const gchar *variant_type;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_ACK, OV_TCP_MIN_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_ACK,
      g_variant_new (variant_type, id));

  return msg;
}

OvTcpMsg *
ov_tcp_msg_new_start_negotiate (guint64 call_id, gchar * local_id,
    guint16 local_port)
{
  OvTcpMsg *msg;
  const gchar *variant_type;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_START_NEGOTIATE, OV_TCP_MIN_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_START_NEGOTIATE,
      g_variant_new (variant_type, call_id, local_id, local_port));

  return msg;
}

OvTcpMsg *
ov_tcp_msg_new_ok_negotiate (guint64 call_id, gchar * local_id)
{
  OvTcpMsg *msg;
  const gchar *variant_type;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_OK_NEGOTIATE, OV_TCP_MIN_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_OK_NEGOTIATE,
      g_variant_new (variant_type, call_id, local_id));

  return msg;
}

OvTcpMsg *
ov_tcp_msg_new_cancel_negotiate (guint64 call_id, gchar * local_id)
{
  OvTcpMsg *msg;
  const gchar *variant_type;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_CANCEL_NEGOTIATE, OV_TCP_MIN_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_CANCEL_NEGOTIATE,
      g_variant_new (variant_type, call_id, local_id));

  return msg;
}

gchar *
ov_tcp_msg_print (OvTcpMsg * msg)
{
  gchar *tmp;

  tmp = g_variant_print (msg->variant, FALSE);

  return tmp;
}

gboolean
ov_tcp_msg_write_to_stream (GOutputStream * output, OvTcpMsg * msg,
    GCancellable * cancellable, GError ** error)
{
  gchar *tmp;
  guint32 tmp1;
  guint64 tmp2;
  GVariant *variant;
  gboolean ret = FALSE;

  tmp = ov_tcp_msg_print (msg);
  GST_DEBUG ("Writing msg type %s to the network; contents: %s",
      ov_tcp_msg_type_to_string (msg->type, OV_TCP_MAX_VERSION),
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
  } else {
    ret = TRUE;
  }

out:
  return ret;
err:
  tmp = ov_tcp_msg_print (msg);
  GST_ERROR ("Unable to write header for msg: %s", tmp);
  g_free (tmp);
  goto out;
}

gboolean
ov_tcp_msg_write_new_error_to_stream (GOutputStream * output,
    guint64 id, const gchar * error_msg, GCancellable * cancellable,
    GError ** error)
{
  gboolean ret;
  OvTcpMsg *msg;

  msg = ov_tcp_msg_new_error (id, error_msg);
  ret = ov_tcp_msg_write_to_stream (output, msg, cancellable, error);
  ov_tcp_msg_free (msg);

  return ret;
}

gboolean
ov_tcp_msg_write_new_ack_to_stream (GOutputStream * output, guint64 id,
    GCancellable * cancellable, GError ** error)
{
  gboolean ret;
  OvTcpMsg *msg;

  msg = ov_tcp_msg_new_ack (id);
  ret = ov_tcp_msg_write_to_stream (output, msg, cancellable, error);
  ov_tcp_msg_free (msg);

  return ret;
}

/* Does a blocking read for the header */
gboolean
ov_tcp_msg_read_header_from_stream (GInputStream * input, OvTcpMsg * msg,
    GCancellable * cancellable, GError ** error)
{
  gboolean ret;
  char tmp[OV_TCP_MSG_HEADER_SIZE];
  gsize bytes_read;

  g_return_val_if_fail (msg != NULL, FALSE);

  /* Read message length prefix */
  ret = g_input_stream_read_all (input, tmp, OV_TCP_MSG_HEADER_SIZE,
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
ov_tcp_msg_read_body_from_stream (GInputStream * input, OvTcpMsg * msg,
    GCancellable * cancellable, GError ** error)
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

  variant_type = ov_tcp_msg_type_to_variant_type (msg->type,
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

OvTcpMsg *
ov_tcp_msg_read_from_stream (GInputStream * input, GCancellable * cancellable,
    GError ** error)
{
  gboolean ret;
  OvTcpMsg *msg;

  msg = g_new0 (OvTcpMsg, 1);

  ret = ov_tcp_msg_read_header_from_stream (input, msg, cancellable,
      error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message length prefix: %s",
        error && *error ? (*error)->message : "Unknown error");
    goto err_no_body;
  }

  GST_DEBUG ("Reading message type %s and version %u of length %u bytes",
      ov_tcp_msg_type_to_string (msg->type, msg->version),
      msg->version, msg->size);

  if (msg->size == 0)
    goto out;

  /* Read the rest of the message */
  ret = ov_tcp_msg_read_body_from_stream (input, msg, cancellable,
      error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message body: %s",
        *error ? (*error)->message : "Unknown error");
    goto err_no_body;
  }

out:
  return msg;

err_no_body:
  g_free (msg);
  msg = NULL;
  goto out;
}
