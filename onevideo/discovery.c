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
#include "utils.h"
#include "comms.h"
#include "discovery.h"
#include "ov-local-peer-priv.h"

#include <string.h>

/* Takes ownership of @data */
OvUdpMsg *
ov_udp_msg_new (OvUdpMsgType type, gchar * data, gsize size)
{
  OvUdpMsg *msg;

  msg = g_new0 (OvUdpMsg, 1);
  msg->version = OV_UDP_MAX_VERSION;
  /* FIXME: Collisions? */
  msg->id = g_get_monotonic_time ();
  msg->type = type;

  if (data != NULL && size > 0) {
    msg->size = size;
    msg->data = data;
  }

  return msg;
}

void
ov_udp_msg_free (OvUdpMsg * msg)
{
  if (!msg)
    return;

  if (msg->data)
    g_free (msg->data);
  g_free (msg);
}

gchar *
ov_udp_msg_to_buffer (OvUdpMsg * msg, gsize * size)
{
  gchar *buffer;

  *size = msg->size + OV_UDP_MSG_HEADER_SIZE;

  buffer = g_malloc0 (*size);
  
  GST_WRITE_UINT32_BE (buffer,      msg->version);
  GST_WRITE_UINT64_BE (buffer + 4,  msg->id);
  GST_WRITE_UINT32_BE (buffer + 12, msg->type);
  GST_WRITE_UINT32_BE (buffer + 16, msg->size);

  if (msg->size > 0)
    memcpy (buffer + 20, msg->data, msg->size);

  return buffer;
}

/* Assumes that *buffer is at least OV_UDP_MSG_HEADER_SIZE in size */
gboolean
ov_udp_msg_read_message_from (OvUdpMsg * msg,
    GSocketAddress ** addr, GSocket * socket, GCancellable * cancellable,
    GError ** error)
{
  gssize size;
  gchar buffer[OV_UDP_MAX_SIZE];
  gboolean ret = FALSE;

  g_return_val_if_fail (msg != NULL, FALSE);

  size = g_socket_receive_from (socket, addr, buffer, OV_UDP_MAX_SIZE,
      cancellable, error);

  if (size < OV_UDP_MSG_HEADER_SIZE) {
    GST_DEBUG ("Received invalid UDP message, ignoring");
    goto err;
  }

  if (size == OV_UDP_MAX_SIZE)
    GST_WARNING ("Buffer might have been bigger than the max UDP size. Data"
        " might have been discarded.");

  msg->version = GST_READ_UINT32_BE (buffer);
  if (msg->version != 1) {
    GST_ERROR ("Message version %u is not supported", msg->version);
    goto err;
  }

  msg->id = GST_READ_UINT64_BE (buffer + 4);
  msg->type = GST_READ_UINT32_BE (buffer + 12);
  msg->size = GST_READ_UINT32_BE (buffer + 16);

  if (msg->size == 0)
    goto out;

  if (size != (msg->size + OV_UDP_MSG_HEADER_SIZE)) {
    GST_WARNING ("Invalid-sized UDP message, ignoring");
    goto err;
  }

  /* Our payload is supposed to be a NUL-terminated string */
  msg->data = g_malloc0 (msg->size);
  memcpy (msg->data, buffer + OV_UDP_MSG_HEADER_SIZE, msg->size);
  /* Ensure NUL-terminated */
  msg->data[msg->size - 1] = '\0';

out:
  ret = TRUE;
err:
  return ret;
}

gboolean
ov_udp_msg_send_to_from (OvUdpMsg * msg, GSocketAddress * to,
    GSocketAddress * from, GCancellable * cancellable, GError ** error)
{
  gsize size;
  gssize written;
  GSocket *socket;
  gchar *buffer;
  gboolean ret = FALSE;

  socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, error);
  if (!socket)
    return FALSE;

  /* FIXME: This will cause operations to return G_IO_ERROR_WOULD_BLOCK if
   * there isn't enough space in the outgoing queue (very very unlikely) */
  g_socket_set_blocking (socket, FALSE);

  if (from != NULL) {
    ret = g_socket_bind (socket, from, TRUE, error);
    if (!ret)
      goto out;
  }

  buffer = ov_udp_msg_to_buffer (msg, &size);
  written = g_socket_send_to (socket, to, buffer, size, cancellable, error);
  g_free (buffer);

  if (written < 0)
    goto out;

out:
  g_object_unref (socket);
  return ret;
}

static void
ov_local_peer_send_info (OvLocalPeer * local, GSocketAddress * addr,
    OvUdpMsg * msg)
{
  gchar *tmp;
  OvUdpMsg *send;
  GSocketAddress *local_addr;

  send = ov_udp_msg_new (OV_UDP_MSG_TYPE_UNICAST_HI_THERE,
      NULL, 0);

  tmp = ov_inet_socket_address_to_string (G_INET_SOCKET_ADDRESS (addr));
  GST_TRACE ("Sending HI_THERE to %s, id: %lu", tmp, send->id);
  g_free (tmp);

  g_object_get (OV_PEER (local), "address", &local_addr, NULL);
  ov_udp_msg_send_to_from (send, addr, local_addr, NULL, NULL);
  g_object_unref (addr);

  ov_udp_msg_free (send);
}

gboolean
on_incoming_udp_message (GSocket * socket, GIOCondition condition G_GNUC_UNUSED,
    OvLocalPeer * local)
{
  gchar *tmp;
  gboolean ret;
  OvUdpMsg *msg;
  GInetSocketAddress *sfrom, *local_addr;
  OvLocalPeerPrivate *priv;
  GSocketAddress *from = NULL;

  priv = ov_local_peer_get_private (local);
  g_object_get (OV_PEER (local), "address", &local_addr, NULL);

  msg = g_new0 (OvUdpMsg, 1);

  /* For now, the only UDP messages we care about are those that want to
   * discover us. If this is extended, something like OvTcpMsg will be
   * implemented. */
  ret = ov_udp_msg_read_message_from (msg, &from, socket,
      NULL, NULL);
  if (!ret)
    goto out;

  sfrom = G_INET_SOCKET_ADDRESS (from);
  tmp = ov_inet_socket_address_to_string (sfrom);
  
  if (ov_inet_socket_address_is_iface (sfrom, priv->mc_ifaces,
        g_inet_socket_address_get_port (local_addr))) {
    GST_TRACE ("Ignoring incoming UDP msg sent by us of type: %u, id: %lu",
        msg->type, msg->id);
    g_free (tmp);
    goto out;
  }

  GST_TRACE ("Incoming UDP msg: %s, id: %lu, from: %s", msg->data, msg->id, tmp);
  g_free (tmp);

  switch (msg->type) {
    case OV_UDP_MSG_TYPE_MULTICAST_DISCOVER:
      ov_local_peer_send_info (local, from, msg);
      break;
    default:
      GST_ERROR ("Received unknown udp msg type: %u", msg->type);
  }

out:
  ov_udp_msg_free (msg);
  g_object_unref (local_addr);
  return G_SOURCE_CONTINUE;
}

gboolean
ov_discovery_send_multicast_discover (OvLocalPeer * local,
    GCancellable * cancellable, GError ** error)
{
  OvUdpMsg *msg;
  GInetAddress *group, *addr;
  GInetSocketAddress *local_addr;
  OvLocalPeerPrivate *priv;
  gboolean ret = FALSE;
  GSocketAddress *mc_addr = NULL;

  priv = ov_local_peer_get_private (local);

  group = g_inet_address_new_from_string (OV_MULTICAST_GROUP);
  /* The multicast port is always the default comms port since
   * we need the group + port combo to be a canonical address. */
  mc_addr = g_inet_socket_address_new (group, OV_DEFAULT_COMM_PORT);
  g_object_unref (group);

  msg = ov_udp_msg_new (OV_UDP_MSG_TYPE_MULTICAST_DISCOVER,
      NULL, 0);

  GST_TRACE ("Sending multicast discover (id %lu) to %s:%u",
      msg->id, OV_MULTICAST_GROUP, OV_DEFAULT_COMM_PORT);

  g_object_get (OV_PEER (local), "address", &local_addr, NULL);
  addr = g_inet_socket_address_get_address (local_addr);
  if (!g_inet_address_get_is_any (addr)) {
    ret = ov_udp_msg_send_to_from (msg, mc_addr,
        G_SOCKET_ADDRESS (local_addr), cancellable, error);
  } else {
    GList *l;
    gboolean res;
    for (l = priv->mc_ifaces; l != NULL; l = l->next) {
      GSocketAddress *saddr;
      addr = ov_get_inet_addr_for_iface (l->data);
      saddr = g_inet_socket_address_new (addr,
          g_inet_socket_address_get_port (local_addr));
      g_object_unref (addr);
      res = ov_udp_msg_send_to_from (msg, mc_addr, saddr, cancellable,
          error);
      g_object_unref (saddr);
      if (res)
        /* We succeed if sending on any of the interfaces succeeds */
        ret = TRUE;
    }
  }
  g_object_unref (local_addr);
  ov_udp_msg_free (msg);
  if (!ret)
    goto out;

  ret = TRUE;
out:
  g_object_unref (mc_addr);
  return ret;
}
