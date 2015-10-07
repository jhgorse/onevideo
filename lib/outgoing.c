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

#include "comms.h"
#include "lib-priv.h"
#include "outgoing.h"

static void
handle_tcp_msg_ack (OneVideoTcpMsg * msg)
{
  guint64 ack_id;
  const gchar *variant_type;

  variant_type =
    one_video_tcp_msg_type_to_variant_type (ONE_VIDEO_TCP_MSG_TYPE_ACK,
        ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &ack_id);
}

static gchar *
handle_tcp_msg_error (OneVideoTcpMsg * msg)
{
  guint64 ack_id;
  gchar *error_msg;
  const gchar *variant_type;

  variant_type =
    one_video_tcp_msg_type_to_variant_type (ONE_VIDEO_TCP_MSG_TYPE_ERROR,
        ONE_VIDEO_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &ack_id, &error_msg);

  return error_msg;
}

OneVideoTcpMsg *
one_video_remote_peer_send_tcp_msg (OneVideoRemotePeer * remote,
    OneVideoTcpMsg * msg, GCancellable * cancellable, GError ** error)
{
  gchar *tmp;
  gboolean ret;
  GSocketClient *client;
  GSocketConnection *conn;
  GInputStream *input;
  GOutputStream *output;
  OneVideoTcpMsg *reply = NULL;

  client = g_socket_client_new ();
  g_socket_client_set_timeout (client, ONE_VIDEO_TCP_TIMEOUT);
  conn = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (remote->addr),
      cancellable, error);
  if (!conn)
    goto no_conn;

  tmp = one_video_tcp_msg_print (msg);
  GST_TRACE ("Sending to '%s' a '%s' msg of size %u: %s", remote->addr_s,
      one_video_tcp_msg_type_to_string (msg->type, msg->version),
      msg->size, tmp);
  g_free (tmp);

  output = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  ret = one_video_tcp_msg_write_to_stream (output, msg, cancellable, error);
  if (!ret)
    goto out;

  input = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  reply = one_video_tcp_msg_read_from_stream (input, cancellable, error);
  if (!reply)
    goto out;

out:
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
no_conn:
  g_object_unref (client);
  return reply;
}

void
one_video_remote_peer_send_tcp_msg_quick_noreply (OneVideoRemotePeer * remote,
    OneVideoTcpMsg * msg)
{
  gchar *tmp;
  GSocketClient *client;
  GSocketConnection *conn;
  GOutputStream *output;

  client = g_socket_client_new ();
  /* Wait at most 1 second per client */
  g_socket_client_set_timeout (client, 1);
  conn = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (remote->addr),
      NULL, NULL);
  if (!conn)
    goto no_conn;

  tmp = one_video_tcp_msg_print (msg);
  GST_TRACE ("Quick-sending to '%s' a '%s' msg of size %u: %s", remote->addr_s,
      one_video_tcp_msg_type_to_string (msg->type, msg->version),
      msg->size, tmp);
  g_free (tmp);

  output = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  if (!one_video_tcp_msg_write_to_stream (output, msg, NULL, NULL))
    goto out;

out:
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
no_conn:
  g_object_unref (client);
  return;
}

static gboolean
one_video_remote_peer_tcp_client_start_negotiate (OneVideoRemotePeer * remote,
    guint64 call_id, GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OneVideoTcpMsg *msg, *reply = NULL;
  gboolean ret = FALSE;

  msg = one_video_tcp_msg_new_start_negotiate (call_id, remote->local->addr_s);

  reply = one_video_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case ONE_VIDEO_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->addr_s);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while starting negotiation: %s",
          remote->addr_s, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          one_video_tcp_msg_type_to_string (
            ONE_VIDEO_TCP_MSG_TYPE_ACK, ONE_VIDEO_TCP_MAX_VERSION),
          remote->addr_s, one_video_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  one_video_tcp_msg_free (reply);
no_reply:
  one_video_tcp_msg_free (msg);
  return ret;
}

static void
one_video_remote_peer_tcp_client_cancel_negotiate (OneVideoRemotePeer * remote,
    guint64 call_id)
{
  OneVideoTcpMsg *msg;

  msg = one_video_tcp_msg_new_cancel_negotiate (call_id, remote->local->addr_s);

  one_video_remote_peer_send_tcp_msg_quick_noreply (remote, msg);
    
  one_video_tcp_msg_free (msg);
}

static GVariant *
get_all_peers_except_this (OneVideoRemotePeer * remote, guint64 call_id)
{
  int ii;
  gchar *tmp;
  GVariant *peers;
  GVariantBuilder *builder;
  OneVideoLocalPeer *local = remote->local;

  builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  g_variant_builder_add (builder, "s", local->addr_s);

  for (ii = 0; ii < local->priv->remote_peers->len; ii++) {
    OneVideoRemotePeer *peer = g_ptr_array_index (local->priv->remote_peers, ii);
    if (peer != remote)
      g_variant_builder_add (builder, "s", peer->addr_s);
  }

  peers = g_variant_new ("(xas)", call_id, builder);
  g_variant_builder_unref (builder);
  
  tmp = g_variant_print (peers, FALSE);
  GST_DEBUG ("Peers remote to peer %s: %s", remote->addr_s, tmp);
  g_free (tmp);

  return peers;
}

static OneVideoTcpMsg *
one_video_remote_peer_tcp_client_query_caps (OneVideoRemotePeer * remote,
    guint64 call_id, GCancellable * cancellable, GError ** error)
{
  gchar *tmp;
  OneVideoTcpMsg *msg, *reply = NULL;

  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_QUERY_CAPS,
      get_all_peers_except_this (remote, call_id));

  reply = one_video_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS:
      /* TODO: Check whether the call id matches */
      tmp = one_video_tcp_msg_print (reply);
      GST_DEBUG ("Reply caps from %s: %s", remote->addr_s, tmp);
      g_free (tmp);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_ERROR ("Expected a 'reply-caps' reply, but got ACK instead");
      goto clear_reply;
    case ONE_VIDEO_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      tmp = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while receiving query caps: %s",
          remote->addr_s, tmp);
      g_free (tmp);
      goto clear_reply;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          one_video_tcp_msg_type_to_string (
            ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS, ONE_VIDEO_TCP_MAX_VERSION),
          remote->addr_s, one_video_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto clear_reply;
  }

no_reply:
  one_video_tcp_msg_free (msg);
  return reply;

clear_reply:
  g_clear_pointer (&reply, (GDestroyNotify) one_video_tcp_msg_free);
  goto no_reply;
}

static void
one_video_local_peer_set_call_details (OneVideoLocalPeer * local,
    GHashTable * in)
{
  guint ii;
  const gchar *in_vtype;

  in_vtype = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS, ONE_VIDEO_TCP_MAX_VERSION);

  /* For each remote peer, find the ports *we* need to send to */
  for (ii = 0; ii < local->priv->remote_peers->len; ii++) {
    GVariant *value;
    GVariantIter *iter;
    OneVideoRemotePeer *remote;
    gchar *addr_s, *send_acaps, *send_vcaps;
    guint32 ports[4] = {};

    remote = g_ptr_array_index (local->priv->remote_peers, ii);

    value = g_hash_table_lookup (in, remote);
    g_variant_get (value, in_vtype, NULL, &send_acaps, &send_vcaps, NULL, NULL,
        &iter);

    /* The caps we will receive from this peer are, of course,
     * the caps it will send to us */
    remote->priv->recv_acaps = gst_caps_from_string (send_acaps);
    remote->priv->recv_vcaps = gst_caps_from_string (send_vcaps);
    g_free (send_acaps); g_free (send_vcaps);

    while (g_variant_iter_loop (iter, "(suuuu)", &addr_s, &ports[0],
          &ports[1], &ports[2], &ports[3])) {
      if (g_strcmp0 (local->addr_s, addr_s) != 0)
        continue;
      remote->priv->send_ports[0] = ports[0];
      remote->priv->send_ports[1] = ports[1];
      remote->priv->send_ports[2] = ports[2];
      remote->priv->send_ports[3] = ports[3];
      GST_DEBUG ("Set remote peer call details: %s, [%u, %u, %u, %u]", addr_s,
          ports[0], ports[1], ports[2], ports[3]);
      g_free (addr_s);
      break;
    }
    g_variant_iter_free (iter);
  }

  local->priv->send_acaps = gst_caps_ref (local->priv->supported_send_acaps);
  /* TODO: Decide send_vcaps based on our upload bandwidth limit */
  local->priv->send_vcaps = gst_caps_ref (local->priv->supported_send_vcaps);
  /* TODO: Restrict local->priv->supported_recv_vcaps as per CPU and download
   * bandwidth limits based on the number of peers */
}

static GHashTable *
one_video_aggregate_call_details_for_remotes (OneVideoLocalPeer * local,
    GHashTable * in, guint64 call_id)
{
  guint ii, jj;
  GHashTable *out;
  gchar *send_acaps, *send_vcaps;
  const gchar *in_vtype, *out_vtype;
  GPtrArray *remotes = local->priv->remote_peers;

  in_vtype = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS, ONE_VIDEO_TCP_MAX_VERSION);
  out_vtype = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS, ONE_VIDEO_TCP_MAX_VERSION);

  /* Format: {OneVideoRemotePeer*: GVariant*}
   * GVariant is of type ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS */
  out = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_variant_unref);

  /* FIXME: Decide send caps. For now, we use the same caps everywhere. */
  send_acaps = gst_caps_to_string (local->priv->supported_send_acaps);
  send_vcaps = gst_caps_to_string (local->priv->supported_send_vcaps);

  /* For each remote peer, iterate over the reply-caps messages received from
   * all *other* peers and find the udpsink send_ports and recv caps that each
   * remote peer should be using for all other peers. */
  /* FIXME: This is O(n³), but could be O(n²) if we iterate over all the
   * reply-caps messages once. Difficult to do with GVariants unless we keep
   * one GVariantBuilder for each peer or something. */
  for (ii = 0; ii < remotes->len; ii++) {
    GVariant *negotiated;
    GVariantBuilder *thisb;
    OneVideoRemotePeer *this;
    
    this = g_ptr_array_index (remotes, ii);
    thisb = g_variant_builder_new (G_VARIANT_TYPE ("a(sssuuuu)"));

    for (jj = 0; jj < remotes->len; jj++) {
      gchar *addr_s;
      GVariant *otherv;
      GVariantIter *iter;
      OneVideoRemotePeer *other;
      gchar *recv_acaps, *recv_vcaps;
      guint32 ports[4] = {};

      other = g_ptr_array_index (remotes, jj);
      if (other == this)
        continue;
      otherv = g_hash_table_lookup (in, other);

      /* 'send_caps' of 'this' remote are 'recv_caps' of the 'other' remote */
      g_variant_get (otherv, in_vtype, NULL, &recv_acaps, &recv_vcaps, NULL,
          NULL, &iter);
      g_assert (recv_acaps && recv_vcaps);
      while (g_variant_iter_loop (iter, "(suuuu)", &addr_s, &ports[0],
            &ports[1], &ports[2], &ports[3])) {
        /* Skip this element if it's not about this 'other' remote */
        if (g_strcmp0 (this->addr_s, addr_s) != 0) {
          GST_DEBUG ("Building details for %s, got %s, continuing",
              this->addr_s, addr_s);
          continue;
        }
        GST_DEBUG ("Building details for %s, got %s, building",
            this->addr_s, addr_s);
        /* Now we know what receiver-side ports 'this' should use while sending
         * data to addr_s */
        g_variant_builder_add (thisb, "(sssuuuu)", other->addr_s, recv_acaps,
            recv_vcaps, ports[0], ports[1], ports[2], ports[3]);
        GST_DEBUG ("%s will recv from %s on ports [%u, %u, %u, %u]",
            other->addr_s, addr_s, ports[0], ports[1], ports[2], ports[3]);
      }
      g_free (recv_acaps);
      g_free (recv_vcaps);
    }
    /* Besides all the other (remote) peers, also add the recv ports that we
     * have allocated for this remote peer */
    g_variant_builder_add (thisb, "(sssuuuu)", local->addr_s, send_acaps,
        send_vcaps, this->priv->recv_ports[0], this->priv->recv_ports[1],
        this->priv->recv_ports[2], this->priv->recv_ports[3]);
    negotiated =
      g_variant_new (out_vtype, call_id, send_acaps, send_vcaps, thisb);
    g_hash_table_insert (out, this, g_variant_ref_sink (negotiated));
    g_variant_builder_unref (thisb);
  }

  g_free (send_acaps);
  g_free (send_vcaps);

  return out;
}

static gboolean
one_video_remote_peer_tcp_client_send_call_details (OneVideoRemotePeer * remote,
    GVariant * details, GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OneVideoTcpMsg *msg, *reply = NULL;
  gboolean ret = FALSE;

  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_CALL_DETAILS, details);

  reply = one_video_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case ONE_VIDEO_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->addr_s);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while receiving call details: %s",
          remote->addr_s, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          one_video_tcp_msg_type_to_string (
            ONE_VIDEO_TCP_MSG_TYPE_ACK, ONE_VIDEO_TCP_MAX_VERSION),
          remote->addr_s, one_video_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  one_video_tcp_msg_free (reply);
no_reply:
  one_video_tcp_msg_free (msg);
  return ret;
}

static gboolean
one_video_remote_peer_tcp_client_start_call (OneVideoRemotePeer * remote,
    GVariant * peers, GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OneVideoTcpMsg *msg, *reply = NULL;
  gboolean ret = FALSE;

  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_START_CALL, peers);

  reply = one_video_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case ONE_VIDEO_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->addr_s);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while receiving call details: %s",
          remote->addr_s, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          one_video_tcp_msg_type_to_string (
            ONE_VIDEO_TCP_MSG_TYPE_ACK, ONE_VIDEO_TCP_MAX_VERSION),
          remote->addr_s, one_video_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  one_video_tcp_msg_free (reply);
no_reply:
  one_video_tcp_msg_free (msg);
  return ret;
}

/* Called with the lock TAKEN
 *
 * START_NEGOTIATE → ACK
 * QUERY_CAPS → REPLY_CAPS
 * CALL_DETAILS → ACK
 * START_CALL → ACK */
void
one_video_local_peer_negotiate_thread (GTask * task, gpointer source_object,
    OneVideoLocalPeer * local, GCancellable * cancellable)
{
  guint ii;
  guint64 call_id;
  GHashTable *in, *out;
  GPtrArray *remotes = local->priv->remote_peers;
  GError *error = NULL;

  if (g_cancellable_is_cancelled (cancellable)) {
    local->priv->negotiator_task = NULL;
    return;
  }

  /* Format: {OneVideoRemotePeer*: GVariant*}
   * GVariant is of type ONE_VIDEO_TCP_MSG_TYPE_REPLY_CAPS */
  in = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_variant_unref);

  call_id = g_get_monotonic_time ();

  /* Send START_NEGOTIATE + QUERY_CAPS to remote peers and get REPLY_CAPS */
  g_rec_mutex_lock (&local->priv->lock);
  /* Every time we take the lock, check if our task has been cancelled */
  if (g_cancellable_is_cancelled (cancellable))
    goto cancelled;
  local->state = ONE_VIDEO_LOCAL_STATE_NEGOTIATING |
    ONE_VIDEO_LOCAL_STATE_NEGOTIATOR;
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    gboolean ret;
    OneVideoTcpMsg *reply;
    OneVideoRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    /* START_NEGOTIATE → ACK */
    ret = one_video_remote_peer_tcp_client_start_negotiate (remote, call_id,
        cancellable, &error);
    if (!ret)
      goto err;

    /* QUERY_CAPS → REPLY_CAPS */
    reply = one_video_remote_peer_tcp_client_query_caps (remote, call_id,
        cancellable, &error);
    if (!reply)
      goto err;

    g_hash_table_insert (in, remote, g_variant_ref (reply->variant));
    one_video_tcp_msg_free (reply);
  }
  g_rec_mutex_unlock (&local->priv->lock);

  /* FIXME: Read caps from all the peers and find the best caps
   * For now, we use the same caps everywhere and just negotiate ports */
  
  g_rec_mutex_lock (&local->priv->lock);
  if (g_cancellable_is_cancelled (cancellable))
    goto cancelled;
  /* Transform REPLY_CAPS to CALL_DETAILS */
  out = one_video_aggregate_call_details_for_remotes (local, in, call_id);
  g_rec_mutex_unlock (&local->priv->lock);

  /* Distribute call details to all remotes */
  g_rec_mutex_lock (&local->priv->lock);
  if (g_cancellable_is_cancelled (cancellable)) {
    g_hash_table_unref (out);
    goto cancelled;
  }
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    gboolean ret;
    OneVideoRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    /* CALL_DETAILS → ACK */
    ret = one_video_remote_peer_tcp_client_send_call_details (remote,
        g_hash_table_lookup (out, remote), cancellable, &error);

    if (!ret) {
      g_hash_table_unref (out);
      goto err;
    }
  }
  local->state = ONE_VIDEO_LOCAL_STATE_NEGOTIATED |
    ONE_VIDEO_LOCAL_STATE_NEGOTIATOR;
  g_rec_mutex_unlock (&local->priv->lock);

  /* Start the call
   * FIXME: Do this in one_video_local_start() ? */
  g_rec_mutex_lock (&local->priv->lock);
  if (g_cancellable_is_cancelled (cancellable)) {
    g_hash_table_unref (out);
    goto cancelled;
  }
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    gboolean ret;
    GVariant *peers;
    OneVideoRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    peers = g_variant_ref_sink (get_all_peers_except_this (remote, call_id));
    /* START_CALL → ACK */
    ret = one_video_remote_peer_tcp_client_start_call (remote,
        peers, cancellable, &error);
    g_variant_unref (peers);
    if (!ret) {
      g_hash_table_unref (out);
      goto err;
    }
  }
  /* Set our own call details */
  one_video_local_peer_set_call_details (local, in);
  local->state = ONE_VIDEO_LOCAL_STATE_READY |
    ONE_VIDEO_LOCAL_STATE_NEGOTIATOR;

  g_hash_table_unref (out);

  /* Unset return-on-cancel so we can do our own return */
  if (g_task_set_return_on_cancel (task, FALSE)) {
    local->priv->active_call_id = call_id;
    g_task_return_boolean (task, TRUE);
  } else {
    goto cancelled;
  }

out:
  g_rec_mutex_unlock (&local->priv->lock);
  g_hash_table_unref (in);
  local->priv->negotiator_task = NULL;
  return;

  /* Called with the lock TAKEN */
err:
  g_task_return_error (task, error);
cancelled:
  GST_DEBUG ("Negotiation cancelled, sending CANCEL_NEGOTIATE");
  for (ii = 0; ii < remotes->len; ii++) {
    OneVideoRemotePeer *remote = g_ptr_array_index (remotes, ii);
    one_video_remote_peer_tcp_client_cancel_negotiate (remote, call_id);
  }
  local->state |= ONE_VIDEO_LOCAL_STATE_FAILED;
  g_rec_mutex_unlock (&local->priv->lock);
  goto out;
}

static gboolean
one_video_remote_peer_tcp_client_end_call (OneVideoRemotePeer * remote,
    OneVideoTcpMsg * msg, GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OneVideoTcpMsg *reply = NULL;
  gboolean ret = FALSE;

  /* FIXME: This does a blocking write which is really bad especially if we're
   * ending the call because we're exiting */
  reply = one_video_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case ONE_VIDEO_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->addr_s);
      break;
    case ONE_VIDEO_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error in reply to end call: %s",
          remote->addr_s, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          one_video_tcp_msg_type_to_string (
            ONE_VIDEO_TCP_MSG_TYPE_ACK, ONE_VIDEO_TCP_MAX_VERSION),
          remote->addr_s, one_video_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  one_video_tcp_msg_free (reply);
no_reply:
  return ret;
}

void
one_video_local_peer_end_call (OneVideoLocalPeer * local)
{
  guint ii;
  OneVideoTcpMsg *msg;
  const gchar *variant_type;

  g_assert (local->priv->active_call_id);

  variant_type = one_video_tcp_msg_type_to_variant_type (
      ONE_VIDEO_TCP_MSG_TYPE_END_CALL, ONE_VIDEO_TCP_MAX_VERSION);
  msg = one_video_tcp_msg_new (ONE_VIDEO_TCP_MSG_TYPE_END_CALL,
      g_variant_new (variant_type, local->priv->active_call_id, local->addr_s));

  for (ii = 0; ii < local->priv->remote_peers->len; ii++) {
    OneVideoRemotePeer *remote;

    remote = g_ptr_array_index (local->priv->remote_peers, ii);
    one_video_remote_peer_tcp_client_end_call (remote, msg, NULL, NULL);
  }

  local->priv->active_call_id = 0;

  one_video_tcp_msg_free (msg);
}
