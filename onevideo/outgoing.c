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
#include "ov-local-peer-priv.h"

static void
handle_tcp_msg_ack (OvTcpMsg * msg)
{
  guint64 ack_id;
  const gchar *variant_type;

  variant_type =
    ov_tcp_msg_type_to_variant_type (OV_TCP_MSG_TYPE_ACK,
        OV_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &ack_id);
  /* Does nothing right now */
}

static gchar *
handle_tcp_msg_error (OvTcpMsg * msg)
{
  guint64 ack_id;
  gchar *error_msg;
  const gchar *variant_type;

  variant_type =
    ov_tcp_msg_type_to_variant_type (OV_TCP_MSG_TYPE_ERROR,
        OV_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &ack_id, &error_msg);

  return error_msg;
}

static gchar *
handle_tcp_msg_ok_negotiate (OvTcpMsg * msg)
{
  guint64 ack_id;
  gchar *peer_id;
  const gchar *variant_type;

  variant_type =
    ov_tcp_msg_type_to_variant_type (OV_TCP_MSG_TYPE_OK_NEGOTIATE,
        OV_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, &ack_id, &peer_id);

  return peer_id;
}

OvTcpMsg *
ov_remote_peer_send_tcp_msg (OvRemotePeer * remote, OvTcpMsg * msg,
    GCancellable * cancellable, GError ** error)
{
  gchar *tmp;
  gboolean ret;
  GSocketClient *client;
  GSocketConnection *conn;
  GInputStream *input;
  GOutputStream *output;
  GSocketAddress *addr;
  GInetSocketAddress *local_addr;
  OvTcpMsg *reply = NULL;

  client = g_socket_client_new ();
  
  /* Set local address with random port to ensure that we connect from the same
   * interface that we're listening on */
  g_object_get (OV_PEER (remote->local), "address", &local_addr, NULL);
  addr = g_inet_socket_address_new (
      g_inet_socket_address_get_address (local_addr), 0);
  g_socket_client_set_local_address (client, addr);
  g_object_unref (local_addr);
  g_object_unref (addr);
  
  /* Set timeout */
  g_socket_client_set_timeout (client, OV_TCP_TIMEOUT);

  conn = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (remote->addr),
      cancellable, error);
  if (!conn) {
    GST_ERROR ("Unable to connect to %s (%s): %s", remote->id, remote->addr_s,
        error ? (*error)->message : "Unknown error");
    goto no_conn;
  }

  tmp = ov_tcp_msg_print (msg);
  GST_TRACE ("Sending to '%s' a '%s' msg of size %u: %s", remote->id,
      ov_tcp_msg_type_to_string (msg->type, msg->version),
      msg->size, tmp);
  g_free (tmp);

  output = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  ret = ov_tcp_msg_write_to_stream (output, msg, cancellable, error);
  if (!ret)
    goto out;

  input = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  reply = ov_tcp_msg_read_from_stream (input, cancellable, error);
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
ov_remote_peer_send_tcp_msg_quick_noreply (OvRemotePeer * remote,
    OvTcpMsg * msg)
{
  gchar *tmp;
  GSocketClient *client;
  GSocketConnection *conn;
  GSocketAddress *addr;
  GOutputStream *output;
  GInetSocketAddress *local_addr;

  client = g_socket_client_new ();
  
  /* Set local address with random port to ensure that we connect from the same
   * interface that we're listening on */
  g_object_get (OV_PEER (remote->local), "address", &local_addr, NULL);
  addr = g_inet_socket_address_new (
      g_inet_socket_address_get_address (local_addr), 0);
  g_socket_client_set_local_address (client, addr);
  g_object_unref (local_addr);
  g_object_unref (addr);
  
  /* Wait at most 1 second per client */
  g_socket_client_set_timeout (client, 1);

  conn = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (remote->addr),
      NULL, NULL);
  if (!conn)
    goto no_conn;

  tmp = ov_tcp_msg_print (msg);
  GST_TRACE ("Quick-sending to '%s' a '%s' msg of size %u: %s", remote->id,
      ov_tcp_msg_type_to_string (msg->type, msg->version),
      msg->size, tmp);
  g_free (tmp);

  output = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  if (!ov_tcp_msg_write_to_stream (output, msg, NULL, NULL))
    goto out;

out:
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
no_conn:
  g_object_unref (client);
  return;
}

static gboolean
ov_remote_peer_tcp_client_start_negotiate (OvRemotePeer * remote,
    guint64 call_id, GCancellable * cancellable, GError ** error)
{
  gchar *error_msg, *local_id;
  GInetSocketAddress *local_addr;
  OvTcpMsg *msg, *reply = NULL;
  gboolean ret = FALSE;

  g_object_get (remote->local, "address", &local_addr, "id", &local_id, NULL);
  msg = ov_tcp_msg_new_start_negotiate (call_id, local_id,
      g_inet_socket_address_get_port (local_addr));
  g_object_unref (local_addr);
  g_free (local_id);

  reply = ov_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case OV_TCP_MSG_TYPE_OK_NEGOTIATE:
      g_assert (remote->id == NULL);
      remote->id = handle_tcp_msg_ok_negotiate (reply);
      GST_DEBUG ("Recvd OK from '%s'", remote->id);
      break;
    case OV_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while starting negotiation: %s",
          remote->addr_s, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          ov_tcp_msg_type_to_string (
            OV_TCP_MSG_TYPE_ACK, OV_TCP_MAX_VERSION),
          remote->addr_s, ov_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  ov_tcp_msg_free (reply);
no_reply:
  ov_tcp_msg_free (msg);
  return ret;
}

static void
ov_remote_peer_tcp_client_cancel_negotiate (OvRemotePeer * remote,
    guint64 call_id)
{
  OvTcpMsg *msg;
  gchar *local_id;

  g_object_get (OV_PEER (remote->local), "id", &local_id, NULL);
  msg = ov_tcp_msg_new_cancel_negotiate (call_id, local_id);
  g_free (local_id);

  ov_remote_peer_send_tcp_msg_quick_noreply (remote, msg);
    
  ov_tcp_msg_free (msg);
}

static GVariant *
get_all_peers_list_except_this (OvRemotePeer * remote, guint64 call_id)
{
  int ii;
  gchar *tmp, *local_id;
  GVariant *peers;
  GVariantBuilder *builder;
  OvLocalPeerPrivate *local_priv;

  local_priv = ov_local_peer_get_private (remote->local);
  g_object_get (OV_PEER (remote->local), "id", &local_id, NULL);

  builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  /* We add ourselves here because we might want to potentially have a call that
   * does not include ourselves */
  g_variant_builder_add (builder, "s", local_id);
  g_free (local_id);

  for (ii = 0; ii < local_priv->remote_peers->len; ii++) {
    OvRemotePeer *peer = g_ptr_array_index (local_priv->remote_peers, ii);
    if (peer != remote)
      g_variant_builder_add (builder, "s", peer->id);
  }

  peers = g_variant_new ("(xas)", call_id, builder);
  g_variant_builder_unref (builder);
  
  tmp = g_variant_print (peers, FALSE);
  GST_DEBUG ("Peers remote to peer %s: %s", remote->id, tmp);
  g_free (tmp);

  return peers;
}

static GVariant *
get_all_remotes_addr_list_except_this (OvRemotePeer * remote,
    guint64 call_id)
{
  int ii;
  gchar *tmp;
  GVariant *peers;
  GVariantBuilder *builder;
  OvLocalPeerPrivate *local_priv;

  local_priv = ov_local_peer_get_private (remote->local);

  builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ss)"));

  /* We don't add ourselves here because each peer already knows about us */

  for (ii = 0; ii < local_priv->remote_peers->len; ii++) {
    OvRemotePeer *peer = g_ptr_array_index (local_priv->remote_peers, ii);
    if (peer != remote)
      /* Here we inform this remote about the address to use to connect to all
       * the other remotes */
      g_variant_builder_add (builder, "(ss)", peer->id, peer->addr_s);
  }

  peers = g_variant_new ("(xa(ss))", call_id, builder);
  g_variant_builder_unref (builder);
  
  tmp = g_variant_print (peers, FALSE);
  GST_DEBUG ("Peers (other than us) remote to peer %s: %s", remote->id, tmp);
  g_free (tmp);

  return peers;
}

static OvTcpMsg *
ov_remote_peer_tcp_client_query_caps (OvRemotePeer * remote,
    guint64 call_id, GCancellable * cancellable, GError ** error)
{
  gchar *tmp;
  OvTcpMsg *msg, *reply = NULL;

  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_QUERY_CAPS,
      get_all_remotes_addr_list_except_this (remote, call_id));

  reply = ov_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case OV_TCP_MSG_TYPE_REPLY_CAPS:
      /* TODO: Check whether the call id matches */
      tmp = ov_tcp_msg_print (reply);
      GST_DEBUG ("Reply caps from %s: %s", remote->id, tmp);
      g_free (tmp);
      break;
    case OV_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_ERROR ("Expected a 'reply-caps' reply, but got ACK instead");
      goto clear_reply;
    case OV_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      tmp = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while receiving query caps: %s",
          remote->id, tmp);
      g_free (tmp);
      goto clear_reply;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          ov_tcp_msg_type_to_string (
            OV_TCP_MSG_TYPE_REPLY_CAPS, OV_TCP_MAX_VERSION),
          remote->id, ov_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto clear_reply;
  }

no_reply:
  ov_tcp_msg_free (msg);
  return reply;

clear_reply:
  g_clear_pointer (&reply, (GDestroyNotify) ov_tcp_msg_free);
  goto no_reply;
}

static void
ov_local_peer_set_call_details (OvLocalPeer * local, GHashTable * in)
{
  guint ii;
  gchar *local_id;
  const gchar *in_vtype;
  OvLocalPeerPrivate *local_priv;

  g_object_get (OV_PEER (local), "id", &local_id, NULL);
  local_priv = ov_local_peer_get_private (local);

  in_vtype = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_REPLY_CAPS, OV_TCP_MAX_VERSION);

  /* For each remote peer, find the ports *we* need to send to */
  for (ii = 0; ii < local_priv->remote_peers->len; ii++) {
    GVariant *value;
    GVariantIter *iter;
    OvRemotePeer *remote;
    /* The caps of the data that this remote will send to everyone */
    gchar *send_acaps, *send_vcaps, *peer_id;
    guint32 ports[6] = {};

    remote = g_ptr_array_index (local_priv->remote_peers, ii);

    value = g_hash_table_lookup (in, remote);
    g_variant_get (value, in_vtype, NULL, &ports[2], &ports[5], &send_acaps,
        &send_vcaps, NULL, NULL, &iter);

    remote->priv->send_ports[2] = ports[2];
    remote->priv->send_ports[5] = ports[5];

    /* The caps we will receive from this peer are, of course,
     * the caps it will send to us */
    remote->priv->recv_acaps = gst_caps_from_string (send_acaps);
    remote->priv->recv_vcaps = gst_caps_from_string (send_vcaps);
    g_free (send_acaps); g_free (send_vcaps);

    while (g_variant_iter_loop (iter, "(suuuu)", &peer_id, &ports[0],
          &ports[1], &ports[3], &ports[4])) {
      if (g_strcmp0 (local_id, peer_id) != 0)
        continue;
      remote->priv->send_ports[0] = ports[0];
      remote->priv->send_ports[1] = ports[1];
      remote->priv->send_ports[3] = ports[3];
      remote->priv->send_ports[4] = ports[4];
      GST_DEBUG ("Set remote peer call details: %s, [%u, %u, %u, %u, %u, %u]",
          peer_id, ports[0], ports[1], ports[2], ports[3], ports[4], ports[5]);
      g_free (peer_id);
      break;
    }
    g_variant_iter_free (iter);
  }
  g_free (local_id);
}

/* Format of GHashTable *in is: {OvRemotePeer*: GVariant*}
 * GVariant is of type OV_TCP_MSG_TYPE_REPLY_CAPS */
static GHashTable *
ov_aggregate_call_details_for_remotes (OvLocalPeer * local, GHashTable * in,
    guint64 call_id)
{
  guint ii, jj;
  GHashTable *out;
  GPtrArray *remotes;
  /* The caps of the data that we will send to everyone */
  gchar *send_acaps, *send_vcaps, *local_id;
  const gchar *in_vtype, *out_vtype;
  OvLocalPeerPrivate *local_priv;

  local_priv = ov_local_peer_get_private (local);
  g_object_get (OV_PEER (local), "id", &local_id, NULL);
  remotes = local_priv->remote_peers;

  in_vtype = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_REPLY_CAPS, OV_TCP_MAX_VERSION);
  out_vtype = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_CALL_DETAILS, OV_TCP_MAX_VERSION);

  /* Format: {OvRemotePeer*: GVariant*}
   * GVariant is of type OV_TCP_MSG_TYPE_CALL_DETAILS */
  out = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_variant_unref);

  local_priv->send_acaps = gst_caps_fixate (
      gst_caps_copy (local_priv->supported_send_acaps));
  send_acaps = gst_caps_to_string (local_priv->send_acaps);
  /* TODO: Right now, we just select the best video caps available. Instead, we
   * should decide send_vcaps based on our upload bandwidth limit and based on
   * the recv_caps of other peers. This is ok right now because other peers
   * just accept any JPEG video anyway. */
  local_priv->send_vcaps = gst_caps_fixate (
      gst_caps_copy (local_priv->supported_send_vcaps));
  send_vcaps = gst_caps_to_string (local_priv->send_vcaps);

  /* For each remote peer, iterate over the reply-caps messages received from
   * all *other* peers and find the udpsink send_ports and recv caps that each
   * remote peer should be using for all other peers. */
  /* FIXME: This is O(n³), but could be O(n²) if we iterate over all the
   * reply-caps messages once. Difficult to do with GVariants unless we keep
   * one GVariantBuilder for each peer or something. */
  for (ii = 0; ii < remotes->len; ii++) {
    GVariant *negotiated;
    GVariantBuilder *thisb;
    OvRemotePeer *this;
    
    this = g_ptr_array_index (remotes, ii);
    thisb = g_variant_builder_new (G_VARIANT_TYPE ("a(sssuuuuuu)"));

    for (jj = 0; jj < remotes->len; jj++) {
      GVariant *otherv;
      GVariantIter *iter;
      OvRemotePeer *other;
      /* The caps of the data that the remote 'other'
       * will receive from the remote 'this'*/
      gchar *recv_acaps, *recv_vcaps, *peer_id;
      guint32 ports[6] = {};

      other = g_ptr_array_index (remotes, jj);
      if (other == this)
        continue;
      otherv = g_hash_table_lookup (in, other);

      g_variant_get (otherv, in_vtype, NULL, &ports[2], &ports[5],
          /* 'send_caps' of 'this' remote are 'recv_caps' of the 'other' remote */
          &recv_acaps, &recv_vcaps, NULL, NULL, &iter);
      g_assert (recv_acaps && recv_vcaps);
      while (g_variant_iter_loop (iter, "(suuuu)", &peer_id, &ports[0],
            &ports[1], &ports[3], &ports[4])) {
        /* Skip this element if it's not about this 'other' remote */
        if (g_strcmp0 (this->id, peer_id) != 0) {
          GST_DEBUG ("Building details for %s, got %s, continuing",
              this->id, peer_id);
          continue;
        }
        GST_DEBUG ("Building details for %s, got %s, building",
            this->id, peer_id);
        /* Now we know what receiver-side ports 'this' should use while sending
         * data to `peer_id` */
        g_variant_builder_add (thisb, "(sssuuuuuu)", other->id, recv_acaps,
            recv_vcaps, ports[0], ports[1], ports[2], ports[3], ports[4],
            ports[5]);
        GST_DEBUG ("%s will recv from %s on ports [%u, %u, %u, %u, %u, %u]",
            other->id, peer_id, ports[0], ports[1], ports[2], ports[3],
            ports[4], ports[5]);
      }
      g_free (recv_acaps);
      g_free (recv_vcaps);
    }
    /* Besides all the other (remote) peers, also add the recv ports that we
     * have allocated for this remote peer and the recv rtcp ports that are
     * common between all remote peers */
    g_variant_builder_add (thisb, "(sssuuuuuu)", local_id, send_acaps,
        send_vcaps, this->priv->recv_ports[0], this->priv->recv_ports[1],
        local_priv->recv_rtcp_ports[0], this->priv->recv_ports[2],
        this->priv->recv_ports[3], local_priv->recv_rtcp_ports[1]);
    /* Create the aggregated CALL_DETAILS GVariant for this remote peer */
    negotiated =
      g_variant_new (out_vtype, call_id, send_acaps, send_vcaps, thisb);
    g_hash_table_insert (out, this, g_variant_ref_sink (negotiated));
    g_variant_builder_unref (thisb);
  }

  g_free (send_acaps);
  g_free (send_vcaps);
  g_free (local_id);
  return out;
}

static gboolean
ov_remote_peer_tcp_client_send_call_details (OvRemotePeer * remote,
    GVariant * details, GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OvTcpMsg *msg, *reply = NULL;
  gboolean ret = FALSE;

  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_CALL_DETAILS, details);

  reply = ov_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case OV_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->id);
      break;
    case OV_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while receiving call details: %s",
          remote->id, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          ov_tcp_msg_type_to_string (
            OV_TCP_MSG_TYPE_ACK, OV_TCP_MAX_VERSION),
          remote->id, ov_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  ov_tcp_msg_free (reply);
no_reply:
  ov_tcp_msg_free (msg);
  return ret;
}

static gboolean
ov_remote_peer_tcp_client_start_call (OvRemotePeer * remote, GVariant * peers,
    GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OvTcpMsg *msg, *reply = NULL;
  gboolean ret = FALSE;

  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_START_CALL, peers);

  reply = ov_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case OV_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->id);
      break;
    case OV_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error while receiving call details: %s",
          remote->id, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          ov_tcp_msg_type_to_string (
            OV_TCP_MSG_TYPE_ACK, OV_TCP_MAX_VERSION),
          remote->id, ov_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  ov_tcp_msg_free (reply);
no_reply:
  ov_tcp_msg_free (msg);
  return ret;
}

/* Called with the lock TAKEN
 *
 * START_NEGOTIATE → ACK
 * QUERY_CAPS → REPLY_CAPS
 * CALL_DETAILS → ACK
 * START_CALL → ACK */
void
ov_local_peer_negotiate_thread (GTask * task, OvLocalPeer * local,
    gpointer task_data, GCancellable * cancellable)
{
  gint ii;
  guint64 call_id;
  GPtrArray *remotes;
  GHashTable *in, *out;
  OvLocalPeerPrivate *local_priv;
  GError *error = NULL;

  local_priv = ov_local_peer_get_private (local);
  remotes = local_priv->remote_peers;

  if (g_cancellable_is_cancelled (cancellable)) {
    local_priv->negotiator_task = NULL;
    return;
  }

  /* Format: {OvRemotePeer*: GVariant*}
   * GVariant is of type OV_TCP_MSG_TYPE_REPLY_CAPS */
  in = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_variant_unref);

  call_id = g_get_monotonic_time ();

  /* Send START_NEGOTIATE + QUERY_CAPS to remote peers and get REPLY_CAPS */
  ov_local_peer_lock (local);
  /* Every time we take the lock, check if our task has been cancelled */
  if (g_cancellable_is_cancelled (cancellable))
    goto cancelled;
  ov_local_peer_set_state (local, OV_LOCAL_STATE_NEGOTIATING);
  ov_local_peer_set_state_negotiator (local);
  /* Begin negotiation with all peers first (which returns a peer id) */
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    gboolean ret;
    OvRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    /* START_NEGOTIATE → ACK */
    ret = ov_remote_peer_tcp_client_start_negotiate (remote, call_id,
        cancellable, &error);
    if (!ret) {
      OvPeer *skipped;
      GST_WARNING ("Unable to start negotiation with remote %s: %s. Skipped.",
          /* We don't know remote->id yet */
          remote->addr_s, error ? error->message : "Unknown error");

      skipped = ov_peer_new (remote->addr);
      g_ptr_array_remove_index (remotes, ii);
      
      /* Unlock local and emit signal */
      ov_local_peer_unlock (local);
      g_signal_emit (local, ov_local_peer_signals[NEGOTIATE_SKIPPED_REMOTE], 0,
          skipped, error);
      ov_local_peer_lock (local);

      g_object_unref (skipped);

      if (g_cancellable_is_cancelled (cancellable)) {
        /* We didn't START_NEGOTIATE with the rest, so don't send
         * CANCEL_NEGOTIATE to them while cancelling */
        g_ptr_array_remove_range (remotes, ii, remotes->len - ii);
        goto cancelled;
      }
      /* This will be incremented in the next loop */
      ii--;
    }
  }
  if (remotes->len == 0) {
    GST_ERROR ("No peers left to call, all failed to negotiate");
    goto err;
  }
  ov_local_peer_unlock (local);

  /* Emit signal after unlocking */
  g_signal_emit (local, ov_local_peer_signals[NEGOTIATE_STARTED], 0);

  ov_local_peer_lock (local);
  if (g_cancellable_is_cancelled (cancellable))
    goto cancelled;
  /* Continue negotiation now that we have the peer id for all peers */
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    OvTcpMsg *reply;
    OvRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    /* QUERY_CAPS → REPLY_CAPS */
    reply = ov_remote_peer_tcp_client_query_caps (remote, call_id,
        cancellable, &error);
    if (!reply)
      goto err;

    g_hash_table_insert (in, remote, g_variant_ref (reply->variant));
    ov_tcp_msg_free (reply);
  }
  ov_local_peer_unlock (local);
  
  ov_local_peer_lock (local);
  if (g_cancellable_is_cancelled (cancellable))
    goto cancelled;
  /* Transform REPLY_CAPS to CALL_DETAILS */
  out = ov_aggregate_call_details_for_remotes (local, in, call_id);
  ov_local_peer_unlock (local);

  /* Distribute call details to all remotes */
  ov_local_peer_lock (local);
  if (g_cancellable_is_cancelled (cancellable)) {
    g_hash_table_unref (out);
    goto cancelled;
  }
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    gboolean ret;
    OvRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    /* CALL_DETAILS → ACK */
    ret = ov_remote_peer_tcp_client_send_call_details (remote,
        g_hash_table_lookup (out, remote), cancellable, &error);

    if (!ret) {
      g_hash_table_unref (out);
      goto err;
    }
  }
  ov_local_peer_set_state (local, OV_LOCAL_STATE_NEGOTIATED);
  ov_local_peer_set_state_negotiator (local);
  ov_local_peer_unlock (local);

  /* Start the call
   * FIXME: Do this in ov_local_start() ? */
  ov_local_peer_lock (local);
  if (g_cancellable_is_cancelled (cancellable)) {
    g_hash_table_unref (out);
    goto cancelled;
  }
  /* TODO: Synchronous for now, make this async to speed up negotiation */
  for (ii = 0; ii < remotes->len; ii++) {
    gboolean ret;
    GVariant *peers;
    OvRemotePeer *remote;

    remote = g_ptr_array_index (remotes, ii);

    peers =
      g_variant_ref_sink (get_all_peers_list_except_this (remote, call_id));
    /* START_CALL → ACK */
    ret = ov_remote_peer_tcp_client_start_call (remote,
        peers, cancellable, &error);
    g_variant_unref (peers);
    if (!ret) {
      g_hash_table_unref (out);
      goto err;
    }
  }
  /* Set our own call details */
  ov_local_peer_set_call_details (local, in);
  ov_local_peer_set_state (local, OV_LOCAL_STATE_READY);
  ov_local_peer_set_state_negotiator (local);

  g_hash_table_unref (out);

  /* Unset return-on-cancel so we can do our own return */
  if (!g_task_set_return_on_cancel (task, FALSE))
    /* FIXME: This won't actually cancel the negotiation because we've already
     * started the call above */
    goto cancelled;

  local_priv->active_call_id = call_id;
  g_task_return_boolean (task, TRUE);

  ov_local_peer_unlock (local);
  g_hash_table_unref (in);
  local_priv->negotiator_task = NULL;

  /* Emit signal after unlocking */
  g_signal_emit (local, ov_local_peer_signals[NEGOTIATE_FINISHED], 0);
  return;

  /* Called with the lock TAKEN */
err:
  /* Return a copy of the error which the task takes ownership of
   * We pass the old error to our NEGOTIATE_ABORTED closures (transfer-none) */
  g_task_return_error (task, error ? g_error_copy (error) : NULL);
cancelled:
  GST_DEBUG ("Negotiation cancelled, sending CANCEL_NEGOTIATE");
  for (ii = 0; ii < remotes->len; ii++) {
    OvRemotePeer *remote = g_ptr_array_index (remotes, ii);
    ov_remote_peer_tcp_client_cancel_negotiate (remote, call_id);
  }
  /* Revert state to STARTED */
  ov_local_peer_set_state (local, OV_LOCAL_STATE_STARTED);
  ov_local_peer_set_state_failed (local);

  ov_local_peer_unlock (local);
  g_hash_table_unref (in);
  local_priv->negotiator_task = NULL;

  /* Emit signal after unlocking. FIXME: Set the error. */
  g_signal_emit (local, ov_local_peer_signals[NEGOTIATE_ABORTED], 0, error);
  g_error_free (error);
  return;
}

static gboolean
ov_remote_peer_tcp_client_end_call (OvRemotePeer * remote, OvTcpMsg * msg,
    GCancellable * cancellable, GError ** error)
{
  gchar *error_msg;
  OvTcpMsg *reply = NULL;
  gboolean ret = FALSE;

  /* FIXME: This does a blocking write which is really bad especially if we're
   * ending the call because we're exiting */
  reply = ov_remote_peer_send_tcp_msg (remote, msg, cancellable, error);
  if (!reply)
    goto no_reply;

  switch (reply->type) {
    case OV_TCP_MSG_TYPE_ACK:
      handle_tcp_msg_ack (reply);
      GST_DEBUG ("Recvd from '%s' ACK", remote->id);
      break;
    case OV_TCP_MSG_TYPE_ERROR:
      /* Try again? */
      error_msg = handle_tcp_msg_error (reply);
      GST_ERROR ("Remote %s returned an error in reply to end call: %s",
          remote->id, error_msg);
      g_free (error_msg);
      goto err;
    default:
      GST_ERROR ("Expected message type '%s' from %s, got '%s'",
          ov_tcp_msg_type_to_string (
            OV_TCP_MSG_TYPE_ACK, OV_TCP_MAX_VERSION),
          remote->id, ov_tcp_msg_type_to_string (reply->type,
            reply->version));
      goto err;
  }

  ret = TRUE;
err:
  ov_tcp_msg_free (reply);
no_reply:
  return ret;
}

void
ov_local_peer_send_end_call (OvLocalPeer * local)
{
  guint ii;
  OvTcpMsg *msg;
  gchar *local_id;
  const gchar *variant_type;
  OvLocalPeerPrivate *local_priv;

  local_priv = ov_local_peer_get_private (local);
  g_object_get (OV_PEER (local), "id", &local_id, NULL);

  if (!local_priv->active_call_id)
    /* No active call */
    return;

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_END_CALL, OV_TCP_MAX_VERSION);
  msg = ov_tcp_msg_new (OV_TCP_MSG_TYPE_END_CALL,
      g_variant_new (variant_type, local_priv->active_call_id, local_id));
  g_free (local_id);

  GST_DEBUG ("Sending END_CALL to remote peers");
  for (ii = 0; ii < local_priv->remote_peers->len; ii++) {
    OvRemotePeer *remote;

    remote = g_ptr_array_index (local_priv->remote_peers, ii);
    ov_remote_peer_tcp_client_end_call (remote, msg, NULL, NULL);
  }

  local_priv->active_call_id = 0;

  ov_tcp_msg_free (msg);
}
