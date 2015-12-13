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

#include <string.h>

/* Takes ownership of @data */
OneVideoUdpMsg *
one_video_udp_msg_new (OneVideoUdpMsgType type, gchar * data, gsize size)
{
  OneVideoUdpMsg *msg;

  msg = g_new0 (OneVideoUdpMsg, 1);
  msg->version = ONE_VIDEO_UDP_MAX_VERSION;
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
one_video_udp_msg_free (OneVideoUdpMsg * msg)
{
  if (!msg)
    return;

  if (msg->data)
    g_free (msg->data);
  g_free (msg);
}

gchar *
one_video_udp_msg_to_buffer (OneVideoUdpMsg * msg, gsize * size)
{
  gchar *buffer;

  *size = msg->size + ONE_VIDEO_UDP_MSG_HEADER_SIZE;

  buffer = g_malloc0 (*size);
  
  GST_WRITE_UINT32_BE (buffer,      msg->version);
  GST_WRITE_UINT64_BE (buffer + 4,  msg->id);
  GST_WRITE_UINT32_BE (buffer + 12, msg->type);
  GST_WRITE_UINT32_BE (buffer + 16, msg->size);

  if (msg->size > 0)
    memcpy (buffer + 20, msg->data, msg->size);

  return buffer;
}

/* Assumes that *buffer is at least ONE_VIDEO_UDP_MSG_HEADER_SIZE in size */
gboolean
one_video_udp_msg_read_message_from (OneVideoUdpMsg * msg,
    GSocketAddress ** addr, GSocket * socket, GCancellable * cancellable,
    GError ** error)
{
  gssize size;
  gchar buffer[ONE_VIDEO_UDP_MAX_SIZE];
  gboolean ret = FALSE;

  g_return_val_if_fail (msg != NULL, FALSE);

  size = g_socket_receive_from (socket, addr, buffer, ONE_VIDEO_UDP_MAX_SIZE,
      cancellable, error);

  if (size < ONE_VIDEO_UDP_MSG_HEADER_SIZE) {
    GST_DEBUG ("Received invalid UDP message, ignoring");
    goto err;
  }

  if (size == ONE_VIDEO_UDP_MAX_SIZE)
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

  if (size != (msg->size + ONE_VIDEO_UDP_MSG_HEADER_SIZE)) {
    GST_WARNING ("Invalid-sized UDP message, ignoring");
    goto err;
  }

  /* Our payload is supposed to be a NUL-terminated string */
  msg->data = g_malloc0 (msg->size);
  memcpy (msg->data, buffer + ONE_VIDEO_UDP_MSG_HEADER_SIZE, msg->size);
  /* Ensure NUL-terminated */
  msg->data[msg->size - 1] = '\0';

out:
  ret = TRUE;
err:
  return ret;
}

gboolean
one_video_udp_msg_send_to_from (OneVideoUdpMsg * msg, GSocketAddress * to,
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

  buffer = one_video_udp_msg_to_buffer (msg, &size);
  written = g_socket_send_to (socket, to, buffer, size, cancellable, error);
  g_free (buffer);

  if (written < 0)
    goto out;

out:
  g_object_unref (socket);
  return ret;
}

static void
one_video_local_peer_send_info (OneVideoLocalPeer * local,
    GSocketAddress * addr, OneVideoUdpMsg * msg)
{
  gchar *tmp;
  OneVideoUdpMsg *send;

  send = one_video_udp_msg_new (ONE_VIDEO_UDP_MSG_TYPE_UNICAST_HI_THERE,
      NULL, 0);

  tmp = one_video_inet_socket_address_to_string (G_INET_SOCKET_ADDRESS (addr));
  GST_DEBUG ("Sending HI_THERE to %s, id: %lu", tmp, send->id);
  g_free (tmp);

  one_video_udp_msg_send_to_from (send, addr, G_SOCKET_ADDRESS (local->addr),
      NULL, NULL);

  one_video_udp_msg_free (send);
}

gboolean
on_incoming_udp_message (GSocket * socket, GIOCondition condition G_GNUC_UNUSED,
    OneVideoLocalPeer * local)
{
  gchar *tmp;
  gboolean ret;
  OneVideoUdpMsg *msg;
  GInetSocketAddress *saddr;
  GSocketAddress *from = NULL;

  msg = g_new0 (OneVideoUdpMsg, 1);

  /* For now, the only UDP messages we care about are those that want to
   * discover us. If this is extended, something like OneVideoTcpMsg will be
   * implemented. */
  ret = one_video_udp_msg_read_message_from (msg, &from, socket,
      NULL, NULL);
  if (!ret)
    goto out;

  saddr = G_INET_SOCKET_ADDRESS (from);
  tmp = one_video_inet_socket_address_to_string (saddr);
  g_free (tmp);
  
  if (one_video_inet_socket_address_is_iface (saddr, local->priv->mc_ifaces,
        g_inet_socket_address_get_port (local->addr))) {
    GST_DEBUG ("Ignoring incoming UDP msg sent by us of type: %u, id: %lu",
        msg->type, msg->id);
    goto out;
  }

  GST_DEBUG ("Incoming UDP msg: %s, id: %lu, from: %s", msg->data, msg->id, tmp);

  switch (msg->type) {
    case ONE_VIDEO_UDP_MSG_TYPE_MULTICAST_DISCOVER:
      one_video_local_peer_send_info (local, from, msg);
      break;
    default:
      GST_ERROR ("Received unknown udp msg type: %u", msg->type);
  }

out:
  one_video_udp_msg_free (msg);
  g_object_unref (from);
  return G_SOURCE_CONTINUE;
}

gboolean
one_video_discovery_send_multicast_discover (OneVideoLocalPeer * local,
    GCancellable * cancellable, GError ** error)
{
  OneVideoUdpMsg *msg;
  GInetAddress *group, *addr;
  gboolean ret = FALSE;
  GSocketAddress *mc_addr = NULL;

  group = g_inet_address_new_from_string (ONE_VIDEO_MULTICAST_GROUP);
  /* The multicast port is always the default comms port since
   * we need the group + port combo to be a canonical address. */
  mc_addr = g_inet_socket_address_new (group, ONE_VIDEO_DEFAULT_COMM_PORT);
  g_object_unref (group);

  msg = one_video_udp_msg_new (ONE_VIDEO_UDP_MSG_TYPE_MULTICAST_DISCOVER,
      NULL, 0);

  GST_DEBUG ("Sending multicast discover (id %lu) to %s:%u",
      msg->id, ONE_VIDEO_MULTICAST_GROUP, ONE_VIDEO_DEFAULT_COMM_PORT);

  addr = g_inet_socket_address_get_address (local->addr);
  if (!g_inet_address_get_is_any (addr)) {
    ret = one_video_udp_msg_send_to_from (msg, mc_addr,
        G_SOCKET_ADDRESS (local->addr), cancellable, error);
  } else {
    GList *l;
    gboolean res;
    for (l = local->priv->mc_ifaces; l != NULL; l = l->next) {
      GSocketAddress *saddr;
      addr = one_video_get_inet_addr_for_iface (l->data);
      saddr = g_inet_socket_address_new (addr,
          g_inet_socket_address_get_port (local->addr));
      g_object_unref (addr);
      res = one_video_udp_msg_send_to_from (msg, mc_addr, saddr, cancellable,
          error);
      g_object_unref (saddr);
      if (res)
        /* We succeed if sending on any of the interfaces succeeds */
        ret = TRUE;
    }
  }
  one_video_udp_msg_free (msg);
  if (!ret)
    goto out;

  ret = TRUE;
out:
  g_object_unref (mc_addr);
  return ret;
}
