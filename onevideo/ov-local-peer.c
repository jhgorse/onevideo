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
#include "outgoing.h"
#include "ov-local-peer.h"
#include "ov-local-peer-priv.h"

G_DEFINE_TYPE_WITH_PRIVATE (OvLocalPeer, ov_local_peer, OV_TYPE_PEER)

enum
{
  PROP_0,

  PROP_IFACE,

  N_PROPERTIES
};

OvLocalPeerPrivate* ov_local_peer_get_private (OvLocalPeer *self);
static void ov_local_peer_dispose (GObject *object);
static void ov_local_peer_finalize (GObject *object);
static void ov_local_peer_constructed (GObject *object);

static void
ov_local_peer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (OV_LOCAL_PEER (object));

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
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (OV_LOCAL_PEER (object));

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

  object_class->constructed = ov_local_peer_constructed;
  object_class->dispose = ov_local_peer_dispose;
  object_class->finalize = ov_local_peer_finalize;

  object_class->set_property = ov_local_peer_set_property;
  object_class->get_property = ov_local_peer_get_property;

  /**
   * OvLocalPeer::discovery-sent:
   * @local: the local peer
   * 
   * Emitted when a discovery multicast message has just been sent.
   *
   * Emissions of this signal are guaranteed to happen from the main thread.
   **/
  ov_local_peer_signals[DISCOVERY_SENT] =
    g_signal_new ("discovery-sent", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (OvLocalPeerClass, discovery_sent),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 0);

  /**
   * OvLocalPeer::peer-discovered:
   * @local: the local peer
   * @peer: an #OvDiscoveredPeer
   * 
   * Emitted when a peer is discovered in response to a multicast discover being
   * sent after calling ov_local_peer_discovery_start().
   *
   * Emissions of this signal are guaranteed to happen from the main thread.
   **/
  ov_local_peer_signals[PEER_DISCOVERED] =
    g_signal_new ("peer-discovered", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (OvLocalPeerClass, peer_discovered),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 1,
        OV_TYPE_DISCOVERED_PEER);

  /**
   * OvLocalPeer::negotiate-incoming:
   * @local: the local peer
   * @peer: the #OvPeer remote peer
   * 
   * Emitted when we receive an incoming negotiation (VoIP call) request from
   * a remote peer and we are currently not already busy (either negotiating or
   * in a call). If no callbacks are connected to this signal, all calls will be
   * refused.
   *
   * Returns: %TRUE if the incoming call was accepted
   **/
  ov_local_peer_signals[NEGOTIATE_INCOMING] =
    g_signal_new ("negotiate-incoming", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (OvLocalPeerClass, negotiate_incoming),
        NULL, NULL,
        NULL,
        G_TYPE_BOOLEAN, 1,
        OV_TYPE_PEER);

  /**
   * OvLocalPeer::negotiate-started:
   * @local: the local peer
   *
   * Emitted for an outgoing call when the local peer has successfully started
   * negotiation with all or some of the selected remote peers after
   * ov_local_peer_negotiate_start() or for an accepted incoming call after the
   * negotiating peer notifies us that it has successfully started negotiation
   * with all or some of the peers in the call.
   *
   * For outgoing calls, if a remote peer is skipped because it could not be
   * contacted, OvLocalPeer::negotiate-skipped-remote is emitted.
   *
   * For both incoming and outgoing calls, if negotiation finishes successfully,
   * #OvLocalPeer::negotiate_finished is emitted. If negotiation fails due to
   * a network error or due to an error returned by a remote peer, or if
   * negotiation is cancelled by calling ov_local_peer_negotiate_cancel(),
   * #OvLocalPeer::negotiate_aborted is emitted.
   **/
  ov_local_peer_signals[NEGOTIATE_STARTED] =
    g_signal_new ("negotiate-started", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (OvLocalPeerClass, negotiate_started),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 0);

  /**
   * OvLocalPeer::negotiate-skipped-remote:
   * @local: the local peer
   * @skipped: the skipped #OvPeer
   * @error: the #GError describing the error
   * 
   * While negotiating an outgoing call, this is emitted for each remote peer
   * that the local peer skips because it did not respond at the start of
   * negotiation. If all remote peers fail to respond,
   * #OvLocalPeer::negotiate_aborted is also emitted in the end.
   **/
  ov_local_peer_signals[NEGOTIATE_SKIPPED_REMOTE] =
    g_signal_new ("negotiate-skipped-remote", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (OvLocalPeerClass, negotiate_skipped_remote),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 2,
        OV_TYPE_PEER,
        G_TYPE_ERROR);

  /**
   * OvLocalPeer::negotiate-finished:
   * @local: the local peer
   * 
   * Emitted when the local peer finishes negotiation successfully and the call
   * can be started by invoking ov_local_peer_call_start()
   **/
  ov_local_peer_signals[NEGOTIATE_FINISHED] =
    g_signal_new ("negotiate-finished", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (OvLocalPeerClass, negotiate_finished),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 0);

  /**
   * OvLocalPeer::negotiate-aborted:
   * @local: the local peer
   * @error: a #GError describing the error
   * 
   * Emitted when negotiation was aborted by the local peer. This can happen
   * either due to a network error, or an error returned by a remote peer, or
   * because negotiation was cancelled by calling
   * ov_local_peer_negotiate_cancel().
   **/
  ov_local_peer_signals[NEGOTIATE_ABORTED] =
    g_signal_new ("negotiate-aborted", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (OvLocalPeerClass, negotiate_aborted),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 1,
        G_TYPE_ERROR);

  /**
   * OvLocalPeer::call-remote-hangup:
   * @local: the local peer
   * 
   * Emitted when the call is ended because of all remote peers hanging up.
   * This signal is not emitted when a call is ended because we called
   * ov_local_peer_call_hangup()
   *
   * TODO: This is the intended behaviour of this signal, but currently it's
   * emitted as soon as any remote hangs up because implementing partial call
   * continuation is on the TODO list.
   **/
  ov_local_peer_signals[CALL_REMOTES_HUNGUP] =
    g_signal_new ("call-remotes-hungup", G_OBJECT_CLASS_TYPE (object_class),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (OvLocalPeerClass, call_remotes_hungup),
        NULL, NULL,
        NULL,
        G_TYPE_NONE, 0);

  g_object_class_install_property (object_class, PROP_IFACE,
      g_param_spec_string ("iface", "Network Interface",
        "User-supplied network interface", NULL, G_PARAM_CONSTRUCT_ONLY |
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
ov_local_peer_init (OvLocalPeer * self)
{
  GstCaps *vcaps;
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);

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
}

static void
ov_local_peer_constructed (GObject * object)
{
  guint16 tcp_port;
  GInetSocketAddress *addr;
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (OV_LOCAL_PEER (object));
  
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
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (OV_LOCAL_PEER (object));
  
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
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (OV_LOCAL_PEER (object));

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
  return ov_local_peer_get_instance_private (self);
}

void
ov_local_peer_lock (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  g_rec_mutex_lock (&priv->lock);
}

void
ov_local_peer_unlock (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  g_rec_mutex_unlock (&priv->lock);
}

/*~~ State manipulation ~~*/

/* This is the only one that is publicly exposed */
OvLocalPeerState
ov_local_peer_get_state (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  return priv->state;
}

void
ov_local_peer_set_state (OvLocalPeer * self, OvLocalPeerState state)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  priv->state = state;
}

void
ov_local_peer_set_state_failed (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  priv->state |= OV_LOCAL_STATE_FAILED;
}

void
ov_local_peer_set_state_timedout (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  priv->state |= OV_LOCAL_STATE_TIMEOUT;
}

void
ov_local_peer_set_state_negotiator (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  /* Can't be negotiator and negotiatee at the same time */
  g_return_if_fail (!(priv->state & OV_LOCAL_STATE_NEGOTIATEE));
  priv->state |= OV_LOCAL_STATE_NEGOTIATOR;
}

void
ov_local_peer_set_state_negotiatee (OvLocalPeer * self)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (self);
  /* Can't be negotiator and negotiatee at the same time */
  g_return_if_fail (!(priv->state & OV_LOCAL_STATE_NEGOTIATOR));
  priv->state |= OV_LOCAL_STATE_NEGOTIATEE;
}

/*~~ Negotiation ~~*/

static void
negotiate_async_ready_cb (OvLocalPeer * local, GAsyncResult * result,
    gpointer user_data)
{
  OvLocalPeerPrivate *priv = ov_local_peer_get_private (local);
  priv->negotiator_task = NULL;
}

/* Will send each remote peer the list of all other remote peers, and each
 * remote peer replies with the recv/send caps it supports. Once all the peers
 * have replied, we'll decide caps for everyone and send them to everyone. All
 * this will happen asynchronously. The caller should just call 
 * ov_local_peer_call_start() when it wants to start the call, and it will 
 * start when everyone is ready. */
gboolean
ov_local_peer_negotiate_start (OvLocalPeer * local)
{
  GTask *task;
  GCancellable *cancellable;
  OvLocalPeerPrivate *priv;
  OvLocalPeerState state;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);

  state = ov_local_peer_get_state (local);

  if (!(state & OV_LOCAL_STATE_STARTED)) {
    GST_ERROR ("State is %u instead of STARTED", state);
    return FALSE;
  }
  ov_local_peer_set_state (local, OV_LOCAL_STATE_STARTED);

  cancellable = g_cancellable_new ();

  task = g_task_new (local, cancellable,
      (GAsyncReadyCallback) negotiate_async_ready_cb, NULL);
  g_task_set_return_on_cancel (task, TRUE);
  g_task_run_in_thread (task,
      (GTaskThreadFunc) ov_local_peer_negotiate_thread);
  priv->negotiator_task = task;
  g_object_unref (cancellable); /* Hand over ref to the task */
  g_object_unref (task);

  ov_local_peer_unlock (local);

  return TRUE;
}

gboolean
ov_local_peer_negotiate_abort (OvLocalPeer * local)
{
  OvLocalPeerState state;
  OvLocalPeerPrivate *priv;
  
  ov_local_peer_lock (local);
  priv = ov_local_peer_get_private (local);

  state = ov_local_peer_get_state (local);
  if (!(state & OV_LOCAL_STATE_NEGOTIATING) &&
      !(state & OV_LOCAL_STATE_NEGOTIATED)) {
    GST_ERROR ("Can't stop negotiating when not negotiating");
    ov_local_peer_unlock (local);
    return FALSE;
  }

  GST_DEBUG ("Cancelling call negotiation");

  if (state & OV_LOCAL_STATE_NEGOTIATOR) {
    if (state & OV_LOCAL_STATE_FAILED)
      /* Negotiation has already failed, cleanup has already been done,
       * nothing to do */
      goto out;
    g_assert (priv->negotiator_task != NULL);
    GST_DEBUG ("Stopping negotiation as the negotiator");
    g_cancellable_cancel (
        g_task_get_cancellable (priv->negotiator_task));
    /* Unlock mutex so that the other thread gets access */
  } else if (state & OV_LOCAL_STATE_NEGOTIATEE) {
    GST_DEBUG ("Stopping negotiation as the negotiatee");
    g_source_remove (priv->negotiate->check_timeout_id);
    g_clear_pointer (&priv->negotiate->remotes,
        (GDestroyNotify) g_hash_table_unref);
    g_clear_pointer (&priv->negotiate, g_free);
    /* Reset state so we accept incoming connections again */
    ov_local_peer_set_state (local, OV_LOCAL_STATE_STARTED);
  } else {
    g_assert_not_reached ();
  }

  ov_local_peer_set_state_failed (local);

out:
  ov_local_peer_unlock (local);

  return TRUE;
}
