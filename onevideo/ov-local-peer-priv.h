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

#ifndef __OV_LOCAL_PEER_PRIV_H__
#define __OV_LOCAL_PEER_PRIV_H__

#include "lib.h"
#include "lib-priv.h"

G_BEGIN_DECLS

enum
{
  DISCOVERY_SENT,
  PEER_DISCOVERED,
  NEGOTIATE_INCOMING,
  NEGOTIATE_STARTED,
  NEGOTIATE_SKIPPED_REMOTE,
  NEGOTIATE_FINISHED,
  NEGOTIATE_ABORTED,
  CALL_REMOTES_HUNGUP,
  OV_LOCAL_PEER_N_SIGNALS
};

extern guint ov_local_peer_signals[OV_LOCAL_PEER_N_SIGNALS];

typedef struct _OvNegotiate OvNegotiate;

struct _OvNegotiate {
  /* Call id while negotiating */
  guint64 call_id;
  /* The remote that we're talking to */
  OvRemotePeer *negotiator;
  /* Potential remotes while negotiating */
  GHashTable *remotes;
  /* A GSourceFunc id that checks for timeouts */
  guint check_timeout_id;
};

struct _OvLocalPeerPrivate {
  /*~ Transmit pipeline ~*/
  GstElement *transmit;
  /* Transmit A/V data, rtcp send/recv RTP bin */
  GstElement *rtpbin;
  /* udpsinks transmitting RTP and RTCP and udpsrcs receiving rtcp */
  GstElement *asend_rtp_sink;
  GstElement *asend_rtcp_sink;
  GstElement *arecv_rtcp_src;
  GstElement *vsend_rtp_sink;
  GstElement *vsend_rtcp_sink;
  GstElement *vrecv_rtcp_src;

  /*~ Playback pipeline ~*/
  GstElement *playback;
  /* primary audio playback elements */
  GstElement *audiomixer;
  GstElement *audiosink;

  /* A unique id representing an active call (0 if no active call) */
  guint64 active_call_id;

  /* Struct used for holding info while negotiating */
  OvNegotiate *negotiate;
  /* The task used for doing negotiation when we're the negotiator */
  GTask *negotiator_task;

  /* The video device monitor being used */
  GstDeviceMonitor *dm;
  /* Video device being used */
  GstDevice *video_device;

  /* The caps that we support sending */
  GstCaps *supported_send_acaps;
  GstCaps *supported_send_vcaps;
  /* The caps that we support receiving */
  GstCaps *supported_recv_acaps;
  GstCaps *supported_recv_vcaps;
  /* The caps that we *will* send */
  GstCaps *send_acaps;
  GstCaps *send_vcaps;
  
  /* User-specified interface */
  gchar *iface;
  /* List of network interfaces we're listening for multicast on
   * This is the same as *iface above if it's not NULL; otherwise
   * we auto-detect all the network interfaces available and
   * populate this ourselves */
  GList *mc_ifaces;
  /* TCP Server for comms (listens on all interfaces if none are specified) */
  GSocketService *tcp_server;
  /* The incoming multicast UDP message listener for all interfaces */
  GSource *mc_socket_source;
  /* The incoming discovery unicast UDP message listener for all interfaces */
  GSource *discover_socket_source;

  /* The local ports we receive rtcp data on with udpsrc, in order:
   * {audio_recv_rtcp RRs, video_recv_rtcp RRs}
   * We recv RTCP RRs from all remotes on the same port for each media type
   * since we send them all to the same rtpbin */
  guint16 recv_rtcp_ports[2];

  /* Array of UDP ports that are either reserved or in use for receiving */
  GArray *used_ports;
  /* Array of OvRemotePeers: peers we want to connect to */
  GPtrArray *remote_peers;

  /* Lock to access non-thread-safe structures like GPtrArray */
  GRecMutex lock;

  OvLocalPeerState state;
};

OvLocalPeerPrivate*   ov_local_peer_get_private   (OvLocalPeer *self);
void                  ov_local_peer_lock          (OvLocalPeer *self);
void                  ov_local_peer_unlock        (OvLocalPeer *self);


void                  ov_local_peer_set_state             (OvLocalPeer *self,
                                                           OvLocalPeerState state);
void                  ov_local_peer_set_state_failed      (OvLocalPeer *self);
void                  ov_local_peer_set_state_timedout    (OvLocalPeer *self);
void                  ov_local_peer_set_state_negotiator  (OvLocalPeer *self);
void                  ov_local_peer_set_state_negotiatee  (OvLocalPeer *self);

G_END_DECLS

#endif /* __OV_LOCAL_PEER_PRIV_H__ */
