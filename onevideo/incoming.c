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
#include "ov-local-peer-priv.h"

static guint timeout_value = 0;

#define OV_NEGOTIATE_TIMEOUT_SECONDS 5

static gboolean
check_negotiate_timeout (OvLocalPeer * local)
{
  OvLocalPeerState state;

  state = ov_local_peer_get_state (local);
  /* Quit if we failed due to some reason */
  if (state & OV_LOCAL_STATE_FAILED)
    return G_SOURCE_REMOVE;

  timeout_value += 1;

  if (timeout_value > OV_NEGOTIATE_TIMEOUT_SECONDS) {
    GST_DEBUG ("Timed out during negotiation, stopping...");
    ov_local_peer_set_state_failed (local);
    ov_local_peer_set_state_timedout (local);
    ov_local_peer_negotiate_stop (local);
    timeout_value = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static gboolean
ov_local_peer_handle_start_negotiate (OvLocalPeer * local,
    GSocketConnection * connection, OvTcpMsg * msg)
{
  guint64 call_id;
  GOutputStream *output;
  OvTcpMsg *reply;
  const gchar *variant_type;
  GSocketAddress *remote_addr, *negotiator_addr;
  guint16 negotiator_port;
  gchar *local_id, *negotiator_id;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  gboolean ret = FALSE;

  priv = ov_local_peer_get_private (local);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_START_NEGOTIATE, OV_TCP_MAX_VERSION);
  if (!g_variant_is_of_type (msg->variant, G_VARIANT_TYPE (variant_type))) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid message data");
    goto send_reply;
  }
  g_variant_get (msg->variant, variant_type, &call_id, &negotiator_id,
      &negotiator_port);

  ov_local_peer_lock (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_STARTED)) {
    reply = ov_tcp_msg_new_error (msg->id, "Busy");
    goto send_reply_unlock;
  }

  priv->negotiate = g_new0 (OvNegotiate, 1);
  priv->negotiate->call_id = call_id;

  /* We receive the port to use while talking to the negotiator, but we must
   * derive the host to use from the connection itself because the negotiator
   * does not always know what address we're resolving it as */
  remote_addr = g_socket_connection_get_remote_address (connection, NULL);
  negotiator_addr = g_inet_socket_address_new (
      g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (remote_addr)),
      negotiator_port);
  g_object_unref (remote_addr);
  priv->negotiate->negotiator =
    ov_remote_peer_new (local, G_INET_SOCKET_ADDRESS (negotiator_addr));
  g_object_unref (negotiator_addr);
  priv->negotiate->negotiator->id = negotiator_id;

  /* Set a rough timer for timing out the negotiation */
  timeout_value = 0;
  priv->negotiate->check_timeout_id = 
    g_timeout_add_seconds_full (G_PRIORITY_DEFAULT_IDLE, 1,
        (GSourceFunc) check_negotiate_timeout, local, NULL);

  ov_local_peer_set_state (local, OV_LOCAL_STATE_NEGOTIATING);
  ov_local_peer_set_state_negotiatee (local);

  g_object_get (OV_PEER (local), "id", &local_id, NULL);
  reply = ov_tcp_msg_new_ok_negotiate (msg->id, local_id);
  g_free (local_id);

  ret = TRUE;

send_reply_unlock:
  ov_local_peer_unlock (local);
send_reply:
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  ov_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  ov_tcp_msg_free (reply);
  return ret;
}

static gboolean
ov_local_peer_handle_cancel_negotiate (OvLocalPeer * local,
    GOutputStream * output, OvTcpMsg * msg)
{
  guint64 call_id;
  OvTcpMsg *reply;
  const gchar *variant_type;
  OvLocalPeerPrivate *priv;
  gboolean ret = FALSE;

  priv = ov_local_peer_get_private (local);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_CANCEL_NEGOTIATE, OV_TCP_MAX_VERSION);
  if (!g_variant_is_of_type (msg->variant, G_VARIANT_TYPE (variant_type))) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid message data");
    goto send_reply;
  }
  g_variant_get (msg->variant, variant_type, &call_id, NULL);
  
  if (call_id != priv->negotiate->call_id) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid call id");
    goto send_reply;
  }

  GST_DEBUG ("Received a CANCEL_NEGOTIATE");

  /* TODO: For now, we completely cancel the negotiation process. Should have
   * a list of peers that have signalled a cancel and just append to it. It
   * should be the job of the negotiating thread to check this and handle it
   * gracefully. */
  if (!ov_local_peer_negotiate_stop (local)) {
    reply = ov_tcp_msg_new_error (msg->id, "Unable to stop negotiation");
    goto send_reply;
  }

  reply = ov_tcp_msg_new_ack (msg->id);

  ret = TRUE;

send_reply:
  ov_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  ov_tcp_msg_free (reply);
  return ret;
}

/* Called with the lock TAKEN */
static gboolean
setup_negotiate_remote_peers (OvLocalPeer * local, OvTcpMsg * msg)
{
  GVariantIter *iter;
  GHashTable *remotes;
  const gchar *variant_type;
  gchar *peer_id, *peer_addr_s;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  remotes = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
      (GDestroyNotify) ov_remote_peer_free);

  /* Add the negotiator; it won't be in the list of remotes below because
   * those are all new remotes */
  g_hash_table_insert (remotes, priv->negotiate->negotiator->id,
      priv->negotiate->negotiator);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_QUERY_CAPS, OV_TCP_MAX_VERSION);
  g_variant_get (msg->variant, variant_type, NULL, &iter);
  while (g_variant_iter_loop (iter, "(ss)", &peer_id, &peer_addr_s)) {
    OvRemotePeer *remote;

    if (g_hash_table_contains (remotes, peer_id)) {
      GST_ERROR ("Query caps contains duplicate remote: %s", peer_id);
      g_free (peer_id);
      goto err;
    }

    /* FIXME: Check whether we can actually route to this peer at all before
     * adding it */
    remote = ov_remote_peer_new_from_string (local, peer_addr_s);
    g_assert (remote != NULL);
    remote->id = g_strdup (peer_id);

    g_hash_table_insert (remotes, remote->id, remote);
  }
  g_variant_iter_free (iter);

  g_assert (priv->negotiate->remotes == NULL);
  priv->negotiate->remotes = remotes;
  return TRUE;
err:
  g_hash_table_unref (remotes);
  return FALSE;
}

static gboolean
ov_local_peer_handle_query_reply_caps (OvLocalPeer * local,
    GSocketConnection * connection, OvTcpMsg * msg)
{
  gchar *tmp;
  gboolean ret;
  guint64 call_id;
  GOutputStream *output;
  GHashTableIter iter;
  GVariantBuilder *peers;
  const gchar *variant_type;
  /* The caps that we can receive */
  gchar *recv_acaps, *recv_vcaps;
  /* The caps that we can send */
  gchar *send_acaps, *send_vcaps;
  OvRemotePeer *remote;
  OvTcpMsg *reply;
  OvLocalPeerState state;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);

  /* Get the call id */
  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_QUERY_CAPS, OV_TCP_MAX_VERSION);
  if (!g_variant_is_of_type (msg->variant, G_VARIANT_TYPE (variant_type))) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid message data");
    goto send_reply;
  }
  g_variant_get (msg->variant, variant_type, &call_id, NULL);

  ov_local_peer_lock (local);

  state = ov_local_peer_get_state (local);
  if (!(state & (OV_LOCAL_STATE_NEGOTIATING | OV_LOCAL_STATE_NEGOTIATEE))) {
    reply = ov_tcp_msg_new_error (msg->id, "Busy");
    ov_local_peer_unlock (local);
    goto send_reply;
  }

  if (priv->negotiate->call_id != call_id) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid call id");
    ov_local_peer_unlock (local);
    goto send_reply;
  }

  timeout_value = 0;

  /* Allocate ports for all peers listed (pre-setup) */
  setup_negotiate_remote_peers (local, msg);

  /* Build the 'reply-caps' msg */
  peers = g_variant_builder_new (G_VARIANT_TYPE ("a(suuuu)"));
  g_hash_table_iter_init (&iter, priv->negotiate->remotes);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer) &remote))
    g_variant_builder_add (peers, "(suuuu)", remote->id,
        remote->priv->recv_ports[0], remote->priv->recv_ports[1],
        remote->priv->recv_ports[2], remote->priv->recv_ports[3]);

  ov_local_peer_unlock (local);

  send_acaps = gst_caps_to_string (priv->supported_send_acaps);
  /* TODO: Decide send_vcaps based on our upload bandwidth limit */
  send_vcaps = gst_caps_to_string (priv->supported_send_vcaps);
  /* TODO: Fixate and restrict recv_?caps as per CPU and download
   * bandwidth limits based on the number of peers */
  recv_acaps = gst_caps_to_string (priv->supported_recv_acaps);
  recv_vcaps = gst_caps_to_string (priv->supported_recv_vcaps);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_REPLY_CAPS, OV_TCP_MAX_VERSION);
  reply = ov_tcp_msg_new (OV_TCP_MSG_TYPE_REPLY_CAPS,
      g_variant_new (variant_type, call_id, priv->recv_rtcp_ports[0],
        priv->recv_rtcp_ports[1], send_acaps, send_vcaps, recv_acaps,
        recv_vcaps, peers));
  g_variant_builder_unref (peers);

  g_free (send_acaps); g_free (send_vcaps);
  g_free (recv_acaps); g_free (recv_vcaps);

  tmp = g_variant_print (reply->variant, FALSE);
  GST_DEBUG ("Replying to 'query caps' with %s", tmp);
  g_free (tmp);

send_reply:
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  ret = ov_tcp_msg_write_to_stream (output, reply, NULL, NULL);
  ov_tcp_msg_free (reply);

  return ret;
}

/* Called with the lock TAKEN */
static gboolean
set_call_details (OvLocalPeer * local, OvTcpMsg * msg)
{
  GVariantIter *iter;
  const gchar *vtype;
  gchar *peer_id, *acaps, *vcaps;
  OvLocalPeerPrivate *priv;
  GHashTable *remotes;
  guint32 ports[6] = {};

  priv = ov_local_peer_get_private (local);
  remotes = priv->negotiate->remotes;

  vtype = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_CALL_DETAILS, OV_TCP_MAX_VERSION);

  g_variant_get (msg->variant, vtype, NULL, &acaps, &vcaps, &iter);

    /* Set our send_caps; unreffing any existing ones if necessary */
  if (priv->send_acaps != NULL)
    gst_caps_unref (priv->send_acaps);
  priv->send_acaps = gst_caps_from_string (acaps);
  if (priv->send_vcaps != NULL)
    gst_caps_unref (priv->send_vcaps);
  priv->send_vcaps = gst_caps_from_string (vcaps);
  g_free (acaps); g_free (vcaps);

  while (g_variant_iter_loop (iter, "(sssuuuuuu)", &peer_id, &acaps, &vcaps,
        &ports[0], &ports[1], &ports[2], &ports[3], &ports[4], &ports[5])) {
    OvRemotePeer *remote;

    remote = g_hash_table_lookup (remotes, peer_id);
    if (!remote) {
      GST_ERROR ("Call details contain invalid remote: %s", peer_id);
      g_assert_not_reached ();
      goto err;
    }

    remote->priv->send_ports[0] = ports[0];
    remote->priv->send_ports[1] = ports[1];
    remote->priv->send_ports[2] = ports[2];
    remote->priv->send_ports[3] = ports[3];
    remote->priv->send_ports[4] = ports[4];
    remote->priv->send_ports[5] = ports[5];

    /* Set the expected recv_caps; unreffing any existing ones if necessary */
    if (remote->priv->recv_acaps != NULL)
      gst_caps_unref (remote->priv->recv_acaps);
    remote->priv->recv_acaps = gst_caps_from_string (acaps);
    if (remote->priv->recv_vcaps != NULL)
      gst_caps_unref (remote->priv->recv_vcaps);
    remote->priv->recv_vcaps = gst_caps_from_string (vcaps);
  }

  ov_local_peer_set_state (local, OV_LOCAL_STATE_NEGOTIATED);
  ov_local_peer_set_state_negotiatee (local);

  g_variant_iter_free (iter);
  return TRUE;
err:
  g_free (peer_id); g_free (acaps); g_free (vcaps);
  g_variant_iter_free (iter);
  return FALSE;
}

static gboolean
ov_local_peer_handle_call_details (OvLocalPeer * local, GOutputStream * output,
    OvTcpMsg * msg)
{
  guint64 call_id;
  OvTcpMsg *reply;
  const gchar *variant_type;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  gboolean ret = FALSE;

  priv = ov_local_peer_get_private (local);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_CALL_DETAILS, OV_TCP_MAX_VERSION);
  if (!g_variant_is_of_type (msg->variant, G_VARIANT_TYPE (variant_type))) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid message data");
    goto send_reply;
  }
  g_variant_get (msg->variant, variant_type, &call_id, NULL, NULL, NULL);

  ov_local_peer_lock (local);

  state = ov_local_peer_get_state (local);
  if (!(state & (OV_LOCAL_STATE_NEGOTIATING | OV_LOCAL_STATE_NEGOTIATEE))) {
    reply = ov_tcp_msg_new_error (msg->id, "Busy");
    goto send_reply_unlock;
  }

  if (priv->negotiate == NULL || priv->negotiate->call_id != call_id) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid call id");
    goto send_reply_unlock;
  }

  timeout_value = 0;

  /* Set call details */
  if (!set_call_details (local, msg)) {
    reply = ov_tcp_msg_new_error_call (call_id, "Invalid call details");
    goto send_reply_unlock;
  }

  reply = ov_tcp_msg_new_ack (msg->id);
  ret = TRUE;

send_reply_unlock:
  ov_local_peer_unlock (local);
send_reply:
  ov_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  ov_tcp_msg_free (reply);
  return ret;
}

/* Called with the lock TAKEN */
static gboolean
start_call (OvLocalPeer * local, OvTcpMsg * msg)
{
  gchar *peer_id;
  const gchar *vtype;
  GVariantIter *iter;
  GHashTable *remotes;
  OvLocalPeerPrivate *priv;

  priv = ov_local_peer_get_private (local);
  remotes = priv->negotiate->remotes;

  g_assert (priv->remote_peers->len == 0);

  vtype = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_START_CALL, OV_TCP_MAX_VERSION);

  g_variant_get (msg->variant, vtype, NULL, &iter);
  /* Move remote peers from the "negotiating" list to "negotiated" list */
  while (g_variant_iter_loop (iter, "s", &peer_id)) {
    OvRemotePeer *remote;

    remote = g_hash_table_lookup (remotes, peer_id);
    if (!remote) {
      GST_ERROR ("Start call contains invalid remote: %s", peer_id);
      goto err;
    }

    /* Move from the negotiating hash table to the local peer */
    ov_local_peer_add_remote (local, remote);
    g_hash_table_steal (remotes, peer_id);
  }
  g_variant_iter_free (iter);

  /* Move the call id */
  priv->active_call_id = priv->negotiate->call_id;
  ov_local_peer_set_state (local, OV_LOCAL_STATE_READY |
      OV_LOCAL_STATE_NEGOTIATEE);

  /* Negotiation has finished, remove timer */
  g_source_remove (priv->negotiate->check_timeout_id);

  /* Clear out negotiate struct. Freeing the remotes removes the allocated
   * udp ports as well */
  g_clear_pointer (&priv->negotiate->remotes,
      (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&priv->negotiate, g_free);

  return ov_local_peer_start_call (local);
err:
  g_free (peer_id);
  g_variant_iter_free (iter);
  return FALSE;
}

static gboolean
ov_local_peer_handle_start_call (OvLocalPeer * local, GOutputStream * output,
    OvTcpMsg * msg)
{
  guint64 call_id;
  OvTcpMsg *reply;
  const gchar *variant_type;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  gboolean ret = FALSE;

  priv = ov_local_peer_get_private (local);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_START_CALL, OV_TCP_MAX_VERSION);
  if (!g_variant_is_of_type (msg->variant, G_VARIANT_TYPE (variant_type))) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid message data");
    goto send_reply;
  }
  g_variant_get (msg->variant, variant_type, &call_id, NULL);

  ov_local_peer_lock (local);

  state = ov_local_peer_get_state (local);
  if (!(state & (OV_LOCAL_STATE_NEGOTIATED | OV_LOCAL_STATE_NEGOTIATEE))) {
    reply = ov_tcp_msg_new_error (msg->id, "Busy");
    goto send_reply_unlock;
  }

  if (priv->negotiate == NULL ||
      priv->negotiate->call_id != call_id) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid call id");
    goto send_reply_unlock;
  }

  timeout_value = 0;

  /* Start calling the specified list of peers */
  if (!start_call (local, msg)) {
    reply = ov_tcp_msg_new_error_call (call_id, "Invalid list of peers");
    goto send_reply_unlock;
  }

  reply = ov_tcp_msg_new_ack (msg->id);
  ret = TRUE;

send_reply_unlock:
  ov_local_peer_unlock (local);
send_reply:
  ov_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  ov_tcp_msg_free (reply);
  return ret;
}

static gboolean
ov_local_peer_remove_peer_from_call (OvLocalPeer * local, GOutputStream * output,
    OvTcpMsg * msg)
{
  gchar *peer_id;
  guint64 call_id;
  OvTcpMsg *reply;
  OvRemotePeer *remote;
  const gchar *variant_type;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  gboolean ret = FALSE;

  priv = ov_local_peer_get_private (local);

  variant_type = ov_tcp_msg_type_to_variant_type (
      OV_TCP_MSG_TYPE_END_CALL, OV_TCP_MAX_VERSION);
  if (!g_variant_is_of_type (msg->variant, G_VARIANT_TYPE (variant_type))) {
    reply = ov_tcp_msg_new_error (msg->id, "Invalid message data");
    goto send_reply;
  }
  g_variant_get (msg->variant, variant_type, &call_id, &peer_id);

  ov_local_peer_lock (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_PAUSED ||
        state & OV_LOCAL_STATE_PLAYING)) {
    reply = ov_tcp_msg_new_error (msg->id, "Busy");
    goto send_reply_unlock;
  }

  remote = ov_local_peer_get_remote_by_id (local, peer_id);
  if (!remote) {
    reply = ov_tcp_msg_new_error_call (call_id, "Invalid peer id");
    goto send_reply_unlock;
  }

  GST_DEBUG ("Removing remote peer %s from the call", remote->id);

  /* Remove the specified peer from the call */
  ov_remote_peer_remove (remote);

  if (priv->remote_peers->len == 0) {
    GST_DEBUG ("No peers left in call, ending call...");
    ov_local_peer_end_call (local);
  }

  reply = ov_tcp_msg_new_ack (msg->id);

  ret = TRUE;

send_reply_unlock:
  ov_local_peer_unlock (local);
send_reply:
  ov_tcp_msg_write_to_stream (output, reply, NULL, NULL);

  ov_tcp_msg_free (reply);
  g_free (peer_id);
  return ret;
}

/* TODO: This does blocking reads over the network, which is ok for now because
 * we're using a threaded listener with 10 threads. However, this makes us
 * susceptible to DoS attacks. Needs fixing. */
gboolean
on_incoming_peer_tcp_connection (GSocketService * service,
    GSocketConnection * connection, GObject * source_object G_GNUC_UNUSED,
    OvLocalPeer * local)
{
  gchar *tmp;
  gboolean ret;
  GInputStream *input;
  GOutputStream *output;
  OvTcpMsg *msg;
  GError *error = NULL;

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  msg = g_new0 (OvTcpMsg, 1);

  ret = ov_tcp_msg_read_header_from_stream (input, msg, NULL,
      &error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message length prefix: %s",
        error ? error->message : "Unknown error");
    if (msg->id)
      /* TODO: Make this more specific; add GError types and send back and
       * forth. In general, error handling is quite crap everywhere right
       * now. */
      ov_tcp_msg_write_new_error_to_stream (output, msg->id,
          "Couldn't finish reading header", NULL, NULL);
    goto out;
  }

  GST_DEBUG ("Incoming message type '%s' and version %u of length %u bytes",
      ov_tcp_msg_type_to_string (msg->type, msg->version),
      msg->version, msg->size);

  /* TODO: Handle incoming messages when we're busy negotiating a call, or
   * are in a call, etc. */

  if (msg->size == 0)
    goto body_done;

  /* Read the rest of the message */
  ret = ov_tcp_msg_read_body_from_stream (input, msg, NULL, &error);
  if (ret != TRUE) {
    GST_ERROR ("Unable to read message body: %s",
        error ? error->message : "Unknown error");
    ov_tcp_msg_write_new_error_to_stream (output, msg->id,
        "Couldn't read body", NULL, NULL);
    goto out;
  }

  tmp = g_variant_print (msg->variant, FALSE);
  GST_DEBUG ("Received message body: %s", tmp);
  g_free (tmp);

body_done:
  switch (msg->type) {
    case OV_TCP_MSG_TYPE_START_NEGOTIATE:
      ov_local_peer_handle_start_negotiate (local, connection, msg);
      break;
    case OV_TCP_MSG_TYPE_CANCEL_NEGOTIATE:
      ov_local_peer_handle_cancel_negotiate (local, output, msg);
      break;
    case OV_TCP_MSG_TYPE_QUERY_CAPS:
      ov_local_peer_handle_query_reply_caps (local, connection, msg);
      break;
    case OV_TCP_MSG_TYPE_CALL_DETAILS:
      ov_local_peer_handle_call_details (local, output, msg);
      break;
    case OV_TCP_MSG_TYPE_START_CALL:
      ov_local_peer_handle_start_call (local, output, msg);
      break;
    case OV_TCP_MSG_TYPE_END_CALL:
      ov_local_peer_remove_peer_from_call (local, output, msg);
      break;
    default:
      ov_tcp_msg_write_new_error_to_stream (output, msg->id,
          "Unknown message type", NULL, NULL);
  }

out:
  /* FIXME: Check error */
  g_io_stream_close (G_IO_STREAM (connection), NULL, &error);
  g_clear_error (&error);
  ov_tcp_msg_free (msg);
  /* Call again for new connections */
  return FALSE;
}
