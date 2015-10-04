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
#include "utils.h"
#include "incoming.h"

static gboolean
one_video_local_peer_start_negotiate (OneVideoLocalPeer * local,
    GOutputStream * output, OneVideoTcpMsg * msg)
{
  guint64 call_id;
  OneVideoTcpMsg *reply;
  gchar *negotiator_addr_s;
  const gchar *variant_type;
  gboolean ret = FALSE;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_START_NEGOTIATE, ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &call_id, &negotiator_addr_s);

  g_rec_mutex_lock (&local->priv->lock);

  if (local->state != ONE_VIDEO_LOCAL_STATE_INITIALISED) {
    reply = one_video_tcp_msg_new_error (call_id, "Busy");
    goto send_reply;
  }

  /* Technically, this is covered by the local->state check, but can't hurt */
  if (local->priv->negotiate != NULL) {
    reply = one_video_tcp_msg_new_error (call_id, "Already negotiating");
    goto send_reply;
  }

  local->priv->negotiate = g_new0 (OneVideoNegotiate, 1);
  local->priv->negotiate->call_id = call_id;
  local->priv->negotiate->negotiator =
    one_video_remote_peer_new (local, negotiator_addr_s);

  local->state = ONE_VIDEO_LOCAL_STATE_NEGOTIATING;

  reply = one_video_tcp_msg_new_ack (msg->id);

  ret = TRUE;

send_reply:
  g_rec_mutex_unlock (&local->priv->lock);
  one_video_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  one_video_tcp_msg_free (reply);
  g_free (negotiator_addr_s);
  return ret;
}

/* Called with the lock TAKEN */
static gboolean
setup_negotiate_remote_peers (OneVideoLocalPeer * local, OneVideoTcpMsg * msg)
{
  GVariantIter *iter;
  GHashTable *remotes;
  const gchar *variant_type;
  gchar *addr_s, *negotiator_addr_s;

  remotes = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
      (GDestroyNotify) one_video_remote_peer_free);

  negotiator_addr_s = local->priv->negotiate->negotiator->addr_s;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_QUERY_CAPS, ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, NULL, &iter);
  while (g_variant_iter_loop (iter, "s", &addr_s)) {
    OneVideoRemotePeer *remote;

    if (g_hash_table_contains (remotes, addr_s)) {
      GST_ERROR ("Query caps contains duplicate remote: %s", addr_s);
      g_free (addr_s);
      goto err;
    }

    if (g_strcmp0 (addr_s, negotiator_addr_s) == 0)
      remote = local->priv->negotiate->negotiator;
    else
      remote = one_video_remote_peer_new (local, addr_s);

    g_assert (remote != NULL);

    g_hash_table_insert (remotes, remote->addr_s, remote);
  }
  g_variant_iter_free (iter);

  g_assert (local->priv->negotiate->remotes == NULL);
  local->priv->negotiate->remotes = remotes;
  return TRUE;
err:
  g_hash_table_unref (remotes);
  return FALSE;
}

static gboolean
one_video_local_peer_query_reply_caps (OneVideoLocalPeer * local,
    GSocketConnection * connection, OneVideoTcpMsg * msg)
{
  gchar *tmp;
  gboolean ret;
  guint64 call_id;
  GOutputStream *output;
  GHashTableIter iter;
  GVariantBuilder *peers;
  const gchar *variant_type;
  gchar *recv_acaps, *recv_vcaps;
  gchar *send_acaps, *send_vcaps;
  OneVideoRemotePeer *remote;
  OneVideoTcpMsg *reply;

  /* Get the call id */
  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_QUERY_CAPS, ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &call_id, NULL);

  g_rec_mutex_lock (&local->priv->lock);

  if (local->state != ONE_VIDEO_LOCAL_STATE_NEGOTIATING) {
    reply = one_video_tcp_msg_new_error (call_id, "Busy");
    goto send_reply;
  }

  if (local->priv->negotiate == NULL ||
      local->priv->negotiate->call_id != call_id) {
    reply = one_video_tcp_msg_new_error (call_id, "Invalid call id");
    g_rec_mutex_unlock (&local->priv->lock);
    goto send_reply;
  }

  /* Allocate ports for all peers listed (pre-setup) */
  setup_negotiate_remote_peers (local, msg);

  /* Build the 'reply-caps' msg */
  peers = g_variant_builder_new (G_VARIANT_TYPE ("a(suuuu)"));
  g_hash_table_iter_init (&iter, local->priv->negotiate->remotes);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer) &remote))
    g_variant_builder_add (peers, "(suuuu)", remote->addr_s,
        remote->priv->recv_ports[0], remote->priv->recv_ports[1],
        remote->priv->recv_ports[2], remote->priv->recv_ports[3]);

  g_rec_mutex_unlock (&local->priv->lock);

  send_acaps = gst_caps_to_string (local->priv->supported_send_acaps);
  send_vcaps = gst_caps_to_string (local->priv->supported_send_vcaps);
  recv_acaps = gst_caps_to_string (local->priv->supported_recv_acaps);
  recv_vcaps = gst_caps_to_string (local->priv->supported_recv_vcaps);

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS, ONE_VIDEO_TCP_MAX_VERSION);
  reply = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS,
      g_variant_new (variant_type, call_id, send_acaps, send_vcaps, recv_acaps,
        recv_vcaps, peers));
  g_variant_builder_unref (peers);

  g_free (send_acaps); g_free (send_vcaps);
  g_free (recv_acaps); g_free (recv_vcaps);

  tmp = g_variant_print (reply->variant, FALSE);
  GST_DEBUG ("Replying to 'query caps' with %s", tmp);
  g_free (tmp);

send_reply:
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  ret = one_video_tcp_msg_write_to_stream (output, reply, NULL, NULL);
  one_video_tcp_msg_free (reply);

  return ret;
}

/* Called with the lock TAKEN */
static gboolean
set_call_details (OneVideoLocalPeer * local, OneVideoTcpMsg * msg)
{
  GVariantIter *iter;
  const gchar *vtype;
  gchar *addr_s, *acaps, *vcaps;
  GHashTable *remotes = local->priv->negotiate->remotes;
  guint32 ports[4] = {};

  vtype = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS, ONE_VIDEO_TCP_MAX_VERSION);

  g_variant_get (msg->variant, vtype, NULL, &acaps, &vcaps, &iter);

    /* Set our send_caps; unreffing any existing ones if necessary */
  if (local->priv->send_acaps != NULL)
    gst_caps_unref (local->priv->send_acaps);
  local->priv->send_acaps = gst_caps_from_string (acaps);
  if (local->priv->send_vcaps != NULL)
    gst_caps_unref (local->priv->send_vcaps);
  local->priv->send_vcaps = gst_caps_from_string (vcaps);
  g_free (acaps); g_free (vcaps);

  while (g_variant_iter_loop (iter, "(sssuuuu)", &addr_s, &acaps, &vcaps,
        &ports[0], &ports[1], &ports[2], &ports[3])) {
    OneVideoRemotePeer *remote;

    remote = g_hash_table_lookup (remotes, addr_s);
    if (!remote) {
      GST_ERROR ("Call details contain invalid remote: %s", addr_s);
      g_assert_not_reached ();
      goto err;
    }

    remote->priv->send_ports[0] = ports[0];
    remote->priv->send_ports[1] = ports[1];
    remote->priv->send_ports[2] = ports[2];
    remote->priv->send_ports[3] = ports[3];

    /* Set the expected recv_caps; unreffing any existing ones if necessary */
    if (remote->priv->recv_acaps != NULL)
      gst_caps_unref (remote->priv->recv_acaps);
    remote->priv->recv_acaps = gst_caps_from_string (acaps);
    if (remote->priv->recv_vcaps != NULL)
      gst_caps_unref (remote->priv->recv_vcaps);
    remote->priv->recv_vcaps = gst_caps_from_string (vcaps);
  }

  local->state = ONE_VIDEO_LOCAL_STATE_NEGOTIATED;

  g_variant_iter_free (iter);
  return TRUE;
err:
  g_free (addr_s); g_free (acaps); g_free (vcaps);
  g_variant_iter_free (iter);
  return FALSE;
}

static gboolean
one_video_local_peer_call_details (OneVideoLocalPeer * local,
    GOutputStream * output, OneVideoTcpMsg * msg)
{
  guint64 call_id;
  OneVideoTcpMsg *reply;
  const gchar *variant_type;
  gboolean ret = FALSE;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS, ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &call_id, NULL, NULL, NULL);

  g_rec_mutex_lock (&local->priv->lock);

  if (local->state != ONE_VIDEO_LOCAL_STATE_NEGOTIATING) {
    reply = one_video_tcp_msg_new_error (call_id, "Busy");
    goto send_reply;
  }

  if (local->priv->negotiate == NULL ||
      local->priv->negotiate->call_id != call_id) {
    reply = one_video_tcp_msg_new_error (call_id, "Invalid call id");
    goto send_reply;
  }

  /* Set call details */
  if (!set_call_details (local, msg)) {
    reply = one_video_tcp_msg_new_error (call_id, "Invalid call details");
    goto send_reply;
  }

  reply = one_video_tcp_msg_new_ack (msg->id);
  ret = TRUE;

send_reply:
  g_rec_mutex_unlock (&local->priv->lock);
  one_video_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  one_video_tcp_msg_free (reply);
  return ret;
}

/* Called with the lock TAKEN */
static gboolean
start_call (OneVideoLocalPeer * local, OneVideoTcpMsg * msg)
{
  gchar *addr_s;
  const gchar *vtype;
  GVariantIter *iter;
  GHashTable *remotes = local->priv->negotiate->remotes;

  g_assert (local->priv->remote_peers->len == 0);

  vtype = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_START_CALL, ONE_VIDEO_TCP_MAX_VERSION);

  g_variant_get (msg->variant, vtype, NULL, &iter);
  /* Move remote peers from the "negotiating" list to "negotiated" list */
  while (g_variant_iter_loop (iter, "s", &addr_s)) {
    OneVideoRemotePeer *remote;

    remote = g_hash_table_lookup (remotes, addr_s);
    if (!remote) {
      GST_ERROR ("Start call contains invalid remote: %s", addr_s);
      goto err;
    }

    /* Move from the negotiating hash table to the local peer */
    one_video_local_peer_add_remote (local, remote);
    g_hash_table_steal (remotes, addr_s);
  }
  g_variant_iter_free (iter);

  /* Move the call id */
  local->priv->active_call_id = local->priv->negotiate->call_id;

  /* Clear out negotiate struct. Freeing the unused remotes removes unused
   * udp ports as well */
  g_clear_pointer (&local->priv->negotiate->remotes,
      (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&local->priv->negotiate, g_free);

  return one_video_local_peer_start (local);
err:
  g_free (addr_s);
  g_variant_iter_free (iter);
  return FALSE;
}

static gboolean
one_video_local_peer_start_call (OneVideoLocalPeer * local,
    GOutputStream * output, OneVideoTcpMsg * msg)
{
  guint64 call_id;
  OneVideoTcpMsg *reply;
  const gchar *variant_type;
  gboolean ret = FALSE;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_START_CALL, ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &call_id, NULL);

  g_rec_mutex_lock (&local->priv->lock);

  if (local->state != ONE_VIDEO_LOCAL_STATE_NEGOTIATED) {
    reply = one_video_tcp_msg_new_error (call_id, "Busy");
    goto send_reply;
  }

  if (local->priv->negotiate == NULL ||
      local->priv->negotiate->call_id != call_id) {
    reply = one_video_tcp_msg_new_error (call_id, "Invalid call id");
    goto send_reply;
  }

  /* Start calling the specified list of peers */
  if (!start_call (local, msg)) {
    reply = one_video_tcp_msg_new_error (call_id, "Invalid list of peers");
    goto send_reply;
  }

  reply = one_video_tcp_msg_new_ack (msg->id);
  ret = TRUE;

send_reply:
  g_rec_mutex_unlock (&local->priv->lock);
  one_video_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  one_video_tcp_msg_free (reply);
  return ret;
}

static gboolean
one_video_local_peer_remove_peer_from_call (OneVideoLocalPeer * local,
    GOutputStream * output, OneVideoTcpMsg * msg)
{
  gchar *peer_id;
  guint64 call_id;
  OneVideoTcpMsg *reply;
  OneVideoRemotePeer *remote;
  const gchar *variant_type;
  gboolean ret = FALSE;

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_END_CALL, ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &call_id, &peer_id);

  g_rec_mutex_lock (&local->priv->lock);

  if (local->state != ONE_VIDEO_LOCAL_STATE_PAUSED &&
      local->state != ONE_VIDEO_LOCAL_STATE_PLAYING) {
    reply = one_video_tcp_msg_new_error (call_id, "Busy");
    goto send_reply;
  }

  remote = one_video_local_peer_get_remote_by_addr_s (local, peer_id);
  if (!remote) {
    reply = one_video_tcp_msg_new_error (call_id, "Invalid peer id");
    goto send_reply;
  }

  GST_DEBUG ("Removing remote peer %s from the call", remote->addr_s);

  /* Remove the specified peer from the call */
  one_video_remote_peer_remove (remote);

  if (local->priv->remote_peers->len == 0) {
    GST_DEBUG ("No peers left in call, ending call...");
    one_video_local_peer_stop (local);
  }

  reply = one_video_tcp_msg_new_ack (msg->id);

  ret = TRUE;

send_reply:
  g_rec_mutex_unlock (&local->priv->lock);
  one_video_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  one_video_tcp_msg_free (reply);
  g_free (peer_id);
  return ret;
}

/* TODO: This does blocking reads over the network, which is ok for now because
 * we're using a threaded listener with 10 threads. However, this makes us
 * susceptible to DoS attacks. Needs fixing. */
gboolean
on_incoming_peer_tcp_connection (GSocketService * service,
    GSocketConnection * connection, GObject * source_object G_GNUC_UNUSED,
    OneVideoLocalPeer * local)
{
  gchar *tmp;
  gboolean ret;
  GInputStream *input;
  GOutputStream *output;
  OneVideoTcpMsg *msg;
  GError *error = NULL;

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  msg = g_new0 (OneVideoTcpMsg, 1);

  ret = one_video_tcp_msg_read_header_from_stream (input, msg, NULL,
      &error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message length prefix: %s", error->message);
    if (msg->id)
      /* TODO: Make this more specific; add GError types and send back and
       * forth. In general, error handling is quite crap everywhere right
       * now. */
      one_video_tcp_msg_write_new_error_to_stream (output, msg->id,
          "Couldn't finish reading header", NULL, NULL);
    goto out;
  }

  GST_DEBUG ("Incoming message type '%s' and version %u of length %u bytes",
      one_video_tcp_msg_type_to_string (msg->type, msg->version),
      msg->version, msg->size);

  /* TODO: Handle incoming messages when we're busy negotiating a call, or
   * are in a call, etc. */

  if (msg->size == 0)
    goto body_done;

  /* Read the rest of the message */
  ret = one_video_tcp_msg_read_body_from_stream (input, msg, NULL, &error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message body: %s", error->message);
    one_video_tcp_msg_write_new_error_to_stream (output, msg->id,
        "Couldn't read body", NULL, NULL);
    goto out;
  }

  tmp = g_variant_print (msg->variant, FALSE);
  GST_DEBUG ("Received message body: %s", tmp);
  g_free (tmp);

body_done:
  switch (msg->type) {
    case ONE_VIDEO_TCP_MSG_TYPE_START_NEGOTIATE:
      one_video_local_peer_start_negotiate (local, output, msg);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_QUERY_CAPS:
      one_video_local_peer_query_reply_caps (local, connection, msg);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS:
      one_video_local_peer_call_details (local, output, msg);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_START_CALL:
      one_video_local_peer_start_call (local, output, msg);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_END_CALL:
      one_video_local_peer_remove_peer_from_call (local, output, msg);
      break;
    default:
      one_video_tcp_msg_write_new_error_to_stream (output, msg->id,
          "Unknown message type", NULL, NULL);
  }

out:
  /* FIXME: Check error */
  g_io_stream_close (G_IO_STREAM (connection), NULL, &error);
  g_clear_error (&error);
  one_video_tcp_msg_free (msg);
  /* Call again for new connections */
  return FALSE;
}
