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

#ifndef __OV_DISCOVERED_PEER_H__
#define __OV_DISCOVERED_PEER_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OV_TYPE_DISCOVERED_PEER ov_discovered_peer_get_type ()
G_DECLARE_DERIVABLE_TYPE (OvDiscoveredPeer, ov_discovered_peer, OV, DISCOVERED_PEER, GObject)

typedef struct _OvDiscoveredPeerPrivate  OvDiscoveredPeerPrivate;

struct _OvDiscoveredPeerClass {
  GObjectClass parent_class;

  /* Padding to allow up to 12 new virtual functions without breaking ABI */
  gpointer padding[12];
};

OvDiscoveredPeer*     ov_discovered_peer_new         (GInetSocketAddress * addr);

G_END_DECLS

#endif /* __OV_DISCOVERED_PEER_H__ */
