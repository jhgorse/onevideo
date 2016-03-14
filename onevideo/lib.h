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

#ifndef __OV_LIB_H__
#define __OV_LIB_H__

#include <gst/gst.h>
#include <gio/gio.h>
#include "ov-peer.h"
#include "ov-local-peer.h"
#include "ov-discovered-peer.h"
#include "ov-remote-peer.h"

G_BEGIN_DECLS

typedef enum _OvVideoQuality OvVideoQuality;

enum _OvVideoQuality {
  OV_VIDEO_QUALITY_INVALID      = 0,

  /* Lower 8 bits are for resolution. These only specify the height; the
   * width (and hence aspect ratio) is dependent on the video source. */
  OV_VIDEO_QUALITY_240P         = 1 << 1,
  OV_VIDEO_QUALITY_360P,
  OV_VIDEO_QUALITY_480P,
  OV_VIDEO_QUALITY_720P,
  OV_VIDEO_QUALITY_1080P,

  /* Bitmask for extracting resolutions */
  OV_VIDEO_QUALITY_RESO_RANGE   = 0xff,

  /* Next 8 bits are for FPS. The same resolution can be transmitted at multiple
   * FPSes, so these can all be ORed with each other to allow easy combining. */
  OV_VIDEO_QUALITY_5FPS         = 1 << 8,
  OV_VIDEO_QUALITY_10FPS        = 1 << 9,
  OV_VIDEO_QUALITY_15FPS        = 1 << 10,
  OV_VIDEO_QUALITY_20FPS        = 1 << 11,
  OV_VIDEO_QUALITY_25FPS        = 1 << 12,
  OV_VIDEO_QUALITY_30FPS        = 1 << 13,
  OV_VIDEO_QUALITY_45FPS        = 1 << 14,
  OV_VIDEO_QUALITY_60FPS        = 1 << 15,

  /* Bitmask for extracting FPSes */
  OV_VIDEO_QUALITY_FPS_RANGE    = 0xff00,

  /* Quality combinations */
  OV_VIDEO_QUALITY_240P10       = OV_VIDEO_QUALITY_240P | OV_VIDEO_QUALITY_10FPS,
  OV_VIDEO_QUALITY_360P15       = OV_VIDEO_QUALITY_360P | OV_VIDEO_QUALITY_15FPS,
  OV_VIDEO_QUALITY_480P30       = OV_VIDEO_QUALITY_480P | OV_VIDEO_QUALITY_30FPS,
  OV_VIDEO_QUALITY_720P30       = OV_VIDEO_QUALITY_720P | OV_VIDEO_QUALITY_30FPS,
  OV_VIDEO_QUALITY_1080P30      = OV_VIDEO_QUALITY_1080P | OV_VIDEO_QUALITY_30FPS,
};

/* Peer discovery */
gboolean            ov_local_peer_discovery_start   (OvLocalPeer *local,
                                                     guint interval,
                                                     GError **error);
void                ov_local_peer_discovery_stop    (OvLocalPeer *local);

/* Setup, negotiation, and calling */
void                ov_local_peer_add_remote        (OvLocalPeer *local,
                                                     OvRemotePeer *remote);
void                ov_local_peer_remove_remote     (OvLocalPeer *local,
                                                     OvRemotePeer *remote);
gboolean            ov_local_peer_start             (OvLocalPeer *local);
/* Asynchronously negotiate with all setup remote peers */
gboolean            ov_local_peer_negotiate_start   (OvLocalPeer *local);
gboolean            ov_local_peer_negotiate_abort   (OvLocalPeer *local);
gboolean            ov_local_peer_call_start        (OvLocalPeer *local);
void                ov_local_peer_call_hangup       (OvLocalPeer *local);
void                ov_local_peer_stop              (OvLocalPeer *local);

/* Video device discovery */
GList*              ov_local_peer_get_video_devices (OvLocalPeer *local);
gboolean            ov_local_peer_set_video_device  (OvLocalPeer *local,
                                                     GstDevice *device);

/* Video quality settings (only after negotiation and during a call) */
OvVideoQuality*     ov_local_peer_get_negotiated_video_qualities  (OvLocalPeer *local);
OvVideoQuality      ov_local_peer_get_video_quality               (OvLocalPeer *local);
OvVideoQuality      ov_local_peer_get_lowest_video_quality        (OvLocalPeer *local);
gboolean            ov_local_peer_set_video_quality               (OvLocalPeer *local,
                                                                   OvVideoQuality quality);
gchar*              ov_video_quality_to_string                    (OvVideoQuality quality);

/* Remote peers */
gpointer            ov_remote_peer_add_gtksink        (OvRemotePeer *remote);
void                ov_remote_peer_set_muted          (OvRemotePeer *remote,
                                                       gboolean muted);
gboolean            ov_remote_peer_get_muted          (OvRemotePeer *remote);
void                ov_remote_peer_pause              (OvRemotePeer *remote);
void                ov_remote_peer_resume             (OvRemotePeer *remote);

GPtrArray*          ov_local_peer_get_remotes         (OvLocalPeer *local);
OvRemotePeer*       ov_local_peer_get_remote_by_id    (OvLocalPeer *local,
                                                       const gchar *peer_id);

G_END_DECLS

#endif /* __OV_LIB_H__ */
