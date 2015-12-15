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

#include "utils.h"
#include "ov-local-peer.h"
#include "ov-local-peer-priv.h"

struct _OvLocalPeer {
  GObject parent;

  OvLocalPeerPrivate *priv;
};

G_DEFINE_TYPE (OvLocalPeer, ov_local_peer, OV_TYPE_PEER)

enum
{
  PROP_IFACE = 1,
  N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL, };

static void ov_local_peer_dispose (GObject *object);
static void ov_local_peer_finalize (GObject *object);
static void ov_local_peer_constructed (GObject *object);

static void
ov_local_peer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  OvLocalPeerPrivate *priv = OV_LOCAL_PEER (object)->priv;

  switch (prop_id) {
    case PROP_IFACE:
      g_free (priv->iface);
      priv->iface = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ov_local_peer_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  OvLocalPeerPrivate *priv = OV_LOCAL_PEER (object)->priv;

  switch (prop_id) {
    case PROP_IFACE:
      g_value_set_string (value, priv->iface);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ov_local_peer_class_init (OvLocalPeerClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
   
  GST_DEBUG_CATEGORY_INIT (onevideo_debug, "onevideo", 0,
      "OneVideo VoIP library");

  g_type_class_add_private (klass, sizeof (OvLocalPeerPrivate));

  object_class->constructed = ov_local_peer_constructed;
  object_class->dispose = ov_local_peer_dispose;
  object_class->finalize = ov_local_peer_finalize;

  object_class->set_property = ov_local_peer_set_property;
  object_class->get_property = ov_local_peer_get_property;

  properties[PROP_IFACE] =
    g_param_spec_string ("iface", "Network Interface",
        "User-supplied network interface", NULL, G_PARAM_CONSTRUCT_ONLY |
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  
  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}

static void
ov_local_peer_init (OvLocalPeer * self)
{
  GstCaps *vcaps;
  OvLocalPeerPrivate *priv;

  priv = G_TYPE_INSTANCE_GET_PRIVATE (self, OV_TYPE_LOCAL_PEER,
      OvLocalPeerPrivate);

  /* Initialize the V4L2 device monitor */
  /* We only want native formats: JPEG, (and later) YUY2 and H.264 */
  vcaps = gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);
  /*gst_caps_append (vcaps,
      gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "YUY2"));
  gst_caps_append (vcaps, gst_caps_new_empty_simple (VIDEO_FORMAT_H264));*/
  priv->dm = gst_device_monitor_new ();
  gst_device_monitor_add_filter (priv->dm, "Video/Source", vcaps);
  gst_caps_unref (vcaps);

  /* NOTE: GArray and GPtrArray are not thread-safe; we must lock accesses */
  g_rec_mutex_init (&priv->lock);
  priv->used_ports = g_array_sized_new (FALSE, TRUE, sizeof (guint), 4);
  priv->remote_peers = g_ptr_array_new ();

  /*-- Initialize (non-RTP) caps supported by us --*/
  /* NOTE: Caps negotiated/exchanged between peers are always non-RTP caps */
  /* We will only ever use 48KHz Opus */
  priv->supported_send_acaps =
    gst_caps_from_string (AUDIO_FORMAT_OPUS CAPS_SEP AUDIO_CAPS_STR);
  /* supported_send_vcaps is set in set_video_device() */

  /* We will only ever use 48KHz Opus */
  priv->supported_recv_acaps =
    gst_caps_new_empty_simple (AUDIO_FORMAT_OPUS);
  /* For now, only support JPEG.
   * TODO: Add other supported formats here */
  priv->supported_recv_vcaps =
    gst_caps_new_empty_simple (VIDEO_FORMAT_JPEG);

  priv->state = OV_LOCAL_STATE_NULL;

  self->priv = priv;
}

static void
ov_local_peer_constructed (GObject * object)
{
  guint16 tcp_port;
  GInetSocketAddress *addr;
  OvLocalPeerPrivate *priv = OV_LOCAL_PEER (object)->priv;
  
  /* Allocate ports for recv RTCP RRs from all remotes */
  g_object_get (OV_PEER (object), "address", &addr, NULL);
  tcp_port = g_inet_socket_address_get_port (addr);
  priv->recv_rtcp_ports[0] = tcp_port + 1;
  priv->recv_rtcp_ports[1] = tcp_port + 2;
  g_object_unref (addr);
}

static void
ov_local_peer_dispose (GObject * object)
{
  OvLocalPeerPrivate *priv = OV_LOCAL_PEER (object)->priv;
  
  g_clear_object (&priv->dm);

  g_clear_object (&priv->tcp_server);
  g_clear_pointer (&priv->mc_socket_source, g_source_destroy);

  g_clear_pointer (&priv->supported_send_acaps, gst_caps_unref);
  g_clear_pointer (&priv->supported_send_vcaps, gst_caps_unref);
  g_clear_pointer (&priv->supported_recv_acaps, gst_caps_unref);
  g_clear_pointer (&priv->supported_recv_vcaps, gst_caps_unref);

  g_clear_object (&priv->transmit);
  g_clear_object (&priv->playback);

  G_OBJECT_CLASS (ov_local_peer_parent_class)->dispose (object);
}

static void
ov_local_peer_finalize (GObject * object)
{
  OvLocalPeerPrivate *priv = OV_LOCAL_PEER (object)->priv;

  GST_DEBUG ("Freeing local peer");
  g_rec_mutex_clear (&priv->lock);
  g_ptr_array_free (priv->remote_peers, TRUE);
  g_list_free_full (priv->mc_ifaces, g_free);
  g_array_free (priv->used_ports, TRUE);
  g_free (priv->iface);
  
  G_OBJECT_CLASS (ov_local_peer_parent_class)->finalize (object);
}

OvLocalPeer *
ov_local_peer_new (const gchar * iface, guint16 port)
{
  GObject *peer;
  gchar *tmp, *id;
  GInetAddress *addr;
  GSocketAddress *saddr;

  if (iface == NULL)
    addr = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
  else
    addr = ov_get_inet_addr_for_iface (iface);

  if (addr == NULL)
    return NULL;

  saddr = g_inet_socket_address_new (addr, port ? port : OV_DEFAULT_COMM_PORT);
  g_object_unref (addr);

  tmp = g_dbus_generate_guid (); /* Generate a UUIDesque string */
  id = g_strdup_printf ("%s:%u-%s", g_get_host_name (),
      g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (saddr)), tmp);
  g_free (tmp);
  
  peer = g_object_new (OV_TYPE_LOCAL_PEER, "address", saddr, "iface", iface,
      "id", id, NULL);
  g_object_unref (saddr);
  g_free (id);

  return OV_LOCAL_PEER (peer);
}

OvLocalPeerPrivate *
ov_local_peer_get_private (OvLocalPeer * self)
{
  return self->priv;
}

void
ov_local_peer_lock (OvLocalPeer * self)
{
  g_rec_mutex_lock (&self->priv->lock);
}

void
ov_local_peer_unlock (OvLocalPeer * self)
{
  g_rec_mutex_unlock (&self->priv->lock);
}

OvLocalPeerState
ov_local_peer_get_state (OvLocalPeer * self)
{
  return self->priv->state;
}

void
ov_local_peer_set_state (OvLocalPeer * self, OvLocalPeerState state)
{
  self->priv->state = state;
}

void
ov_local_peer_set_state_failed (OvLocalPeer * self)
{
  self->priv->state |= OV_LOCAL_STATE_FAILED;
}

void
ov_local_peer_set_state_timedout (OvLocalPeer * self)
{
  self->priv->state |= OV_LOCAL_STATE_TIMEOUT;
}

void
ov_local_peer_set_state_negotiator (OvLocalPeer * self)
{
  /* Can't be negotiator and negotiatee at the same time */
  g_return_if_fail (!(self->priv->state & OV_LOCAL_STATE_NEGOTIATEE));
  self->priv->state |= OV_LOCAL_STATE_NEGOTIATOR;
}

void
ov_local_peer_set_state_negotiatee (OvLocalPeer * self)
{
  /* Can't be negotiator and negotiatee at the same time */
  g_return_if_fail (!(self->priv->state & OV_LOCAL_STATE_NEGOTIATOR));
  self->priv->state |= OV_LOCAL_STATE_NEGOTIATEE;
}
