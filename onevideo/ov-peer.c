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
#include "ov-peer.h"

struct _OvPeerPrivate {
  /* Address of the peer */
  GInetSocketAddress *addr;
  /* String representation of the above address (for logging, etc) */
  gchar *addr_s;
  /* Unique id string representing this peer */
  gchar *id;
};

G_DEFINE_TYPE_WITH_PRIVATE (OvPeer, ov_peer, G_TYPE_OBJECT)

enum
{
  PROP_ID = 1,
  PROP_ADDR = 2,
  PROP_ADDR_S = 3,
  N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL, };

static void ov_peer_dispose (GObject *object);
static void ov_peer_finalize (GObject *object);

static void
ov_peer_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  OvPeer *self = OV_PEER (object);
  OvPeerPrivate *priv = ov_peer_get_instance_private (self);

  switch (prop_id) {
    case PROP_ID:
      g_free (priv->id);
      priv->id = g_value_dup_string (value);
      break;
    case PROP_ADDR:
      g_clear_object (&priv->addr);
      g_free (priv->addr_s);
      priv->addr = g_value_dup_object (value);
      priv->addr_s = ov_inet_socket_address_to_string (priv->addr);
      break;     
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ov_peer_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  OvPeer *self = OV_PEER (object);
  OvPeerPrivate *priv = ov_peer_get_instance_private (self);

  switch (prop_id) {
    case PROP_ID:
      g_value_set_string (value, priv->id);
      break;
    case PROP_ADDR:
      g_value_set_object (value, priv->addr);
      break;
    case PROP_ADDR_S:
      g_value_set_string (value, priv->addr_s);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ov_peer_class_init (OvPeerClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ov_peer_dispose;
  object_class->finalize = ov_peer_finalize;

  object_class->set_property = ov_peer_set_property;
  object_class->get_property = ov_peer_get_property;

  properties[PROP_ID] =
    g_param_spec_string ("id", "Unique ID", "Unique identifier of the peer",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ADDR] =
    g_param_spec_object ("address", "Address", "Address of the peer",
        G_TYPE_INET_SOCKET_ADDRESS, G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
        G_PARAM_STATIC_STRINGS);

  properties[PROP_ADDR_S] =
    g_param_spec_string ("address-string", "Address string",
        "Address of the peer as a string", NULL, G_PARAM_READABLE |
        G_PARAM_STATIC_STRINGS);
  
  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}

static void
ov_peer_init (OvPeer * self)
{
}

static void
ov_peer_dispose (GObject * object)
{
  OvPeerPrivate *priv = ov_peer_get_instance_private (OV_PEER (object));

  g_clear_object (&priv->addr);
  
  G_OBJECT_CLASS (ov_peer_parent_class)->dispose (object);
}

static void
ov_peer_finalize (GObject * object)
{
  OvPeerPrivate *priv = ov_peer_get_instance_private (OV_PEER (object));

  g_free (priv->id);
  g_free (priv->addr_s);
  
  G_OBJECT_CLASS (ov_peer_parent_class)->finalize (object);
}

OvPeer *
ov_peer_new (GInetSocketAddress * addr)
{
  return g_object_new (OV_TYPE_PEER, "addr", addr, NULL);
}
