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

#include "ov-peer.h"
#include "ov-discovered-peer.h"

struct _OvDiscoveredPeerPrivate {
  /* Monotonic time when this peer was discovered */
  gint64 dtime;
};

G_DEFINE_TYPE_WITH_PRIVATE (OvDiscoveredPeer, ov_discovered_peer, OV_TYPE_PEER)

enum
{
  PROP_DTIME = 1,
  N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL, };

static void
ov_discovered_peer_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  OvDiscoveredPeerPrivate *priv;
  
  priv = ov_discovered_peer_get_instance_private (OV_DISCOVERED_PEER (object));

  switch (prop_id) {
    case PROP_DTIME:
      priv->dtime = g_value_get_int64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ov_discovered_peer_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  OvDiscoveredPeerPrivate *priv;
  
  priv = ov_discovered_peer_get_instance_private (OV_DISCOVERED_PEER (object));

  switch (prop_id) {
    case PROP_DTIME:
      g_value_set_int64 (value, priv->dtime);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ov_discovered_peer_class_init (OvDiscoveredPeerClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = ov_discovered_peer_set_property;
  object_class->get_property = ov_discovered_peer_get_property;

  properties[PROP_DTIME] =
    g_param_spec_int64 ("discover-time", "Discover time",
        "The monotonic time when this peer was discovered",
        0, G_MAXINT64, 0,
        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}

static void
ov_discovered_peer_init (OvDiscoveredPeer * self)
{
}

OvDiscoveredPeer *
ov_discovered_peer_new (GInetSocketAddress * addr)
{
  return g_object_new (OV_TYPE_DISCOVERED_PEER, "address", addr,
      "discover-time", g_get_monotonic_time (), NULL);
}
