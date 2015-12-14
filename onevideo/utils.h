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

#ifndef __OV_UTILS_H__
#define __OV_UTILS_H__

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

GInetSocketAddress* ov_inet_socket_address_from_string  (const gchar *addr_s);
gchar*              ov_inet_socket_address_to_string    (const GInetSocketAddress *addr);
gboolean            ov_inet_socket_address_equal        (GInetSocketAddress *addr1,
                                                         GInetSocketAddress *addr2);
gboolean            ov_inet_socket_address_is_iface     (GInetSocketAddress *addr,
                                                         GList *ifaces,
                                                         guint16 port);

#if defined(G_OS_UNIX) || defined (G_OS_WIN32)
GInetAddress*       ov_get_inet_addr_for_iface          (const gchar *iface_name);
GList*              ov_get_network_interfaces           (void);
#endif

#ifdef __linux__
GstDevice*          ov_get_device_from_device_path      (GList *devices,
                                                         const gchar *path);
#endif

G_END_DECLS

#endif /* __OV_UTILS_H__ */
