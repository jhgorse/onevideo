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

#include <string.h>

#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

#include <glib/gstdio.h>

GInetSocketAddress *
one_video_inet_socket_address_from_string (const gchar * addr_s)
{
  guint port;
  gchar **split;
  GSocketAddress *addr = NULL;

  g_return_val_if_fail (addr_s != NULL, NULL);

  if (strlen (addr_s) < 1) {
    GST_ERROR ("Received empty string as argument");
    goto out;
  }

  split = g_strsplit (addr_s, ":", 2);

  if (g_strv_length (split) == 1) {
    port = ONE_VIDEO_DEFAULT_COMM_PORT;
  } else {
    port = g_ascii_strtoull (split[1], NULL, 10);
    if (!port) {
      GST_ERROR ("Invalid port: %s", split[1]);
      goto out;
    }
  }

  /* Special-case the string 'localhost' */
  if (g_strcmp0 (split[0], "localhost") == 0)
    addr = g_inet_socket_address_new_from_string ("127.0.0.1", port);
  else
    addr = g_inet_socket_address_new_from_string (split[0], port);

out:
  g_strfreev (split);
  return G_INET_SOCKET_ADDRESS (addr);
}

gchar *
one_video_inet_socket_address_to_string (const GInetSocketAddress * addr)
{
  gchar *addr_s, *inet_addr_s;

  inet_addr_s = g_inet_address_to_string (
      g_inet_socket_address_get_address ((GInetSocketAddress*)addr));
  addr_s = g_strdup_printf ("%s:%u", inet_addr_s,
      g_inet_socket_address_get_port ((GInetSocketAddress*)addr));
  g_free (inet_addr_s);

  return addr_s;
}

gboolean
one_video_inet_socket_address_equal (GInetSocketAddress * addr1,
    GInetSocketAddress * addr2)
{
  if (g_inet_socket_address_get_port (addr1) ==
      g_inet_socket_address_get_port (addr2) &&
      g_inet_address_equal (g_inet_socket_address_get_address (addr1),
        g_inet_socket_address_get_address (addr2)))
    return TRUE;
  return FALSE;
}

/* BEGIN: Copy of nice_interfaces_get_ip_for_interface() from libnice.
 * Replace with that if/when we use libnice */

#ifdef G_OS_UNIX

GInetAddress *
one_video_get_inet_addr_for_iface (const gchar * iface_name)
{
  struct ifreq ifr = {0};
  union {
    struct sockaddr *addr;
    struct sockaddr_in *in;
  } sa;
  gint sockfd;

  g_return_val_if_fail (iface_name != NULL, NULL);

  ifr.ifr_addr.sa_family = AF_INET;
  g_strlcpy (ifr.ifr_name, iface_name, sizeof (ifr.ifr_name));

  if ((sockfd = socket (AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0) {
    GST_ERROR ("Cannot open socket to retreive interface list");
    return NULL;
  }

  if (ioctl (sockfd, SIOCGIFADDR, &ifr) < 0) {
    GST_ERROR ("Unable to get IP information for interface %s", iface_name);
    g_close (sockfd, NULL);
    return NULL;
  }

  g_close (sockfd, NULL);
  sa.addr = &ifr.ifr_addr;
  GST_DEBUG ("Address for %s: %s", iface_name, inet_ntoa (sa.in->sin_addr));

  return g_inet_address_new_from_string (inet_ntoa (sa.in->sin_addr));
}

#elif defined(G_OS_WIN32)

#include <winsock2.h>
#include <iphlpapi.h>

// Should be in Iphlpapi.h, but mingw doesn't seem to have these
// Values copied directly from:
// http://msdn.microsoft.com/en-us/library/aa366845(v=vs.85).aspx
// (Title: MIB_IPADDRROW structure)

#ifndef MIB_IPADDR_DISCONNECTED
#define MIB_IPADDR_DISCONNECTED 0x0008
#endif

#ifndef MIB_IPADDR_DELETED
#define MIB_IPADDR_DELETED 0x0040
#endif

GInetAddress *
one_video_get_inet_addr_for_iface (const gchar * iface_name)
{
  DWORD i;
  ULONG size = 0;
  PMIB_IFTABLE if_table;
  gchar *ret, *tmp_str;
  GInetAddress *ip = NULL;

  GetIfTable (NULL, &size, TRUE);

  if (!size)
    return NULL;

  if_table = (PMIB_IFTABLE)g_malloc0 (size);

  if (GetIfTable (if_table, &size, TRUE) != ERROR_SUCCESS)
    goto out;

  for (i = 0; i < if_table->dwNumEntries; i++) {
    tmp_str = g_utf16_to_utf8 (
        if_table->table[i].wszName, MAX_INTERFACE_NAME_LEN,
        NULL, NULL, NULL);
    if (strlen (iface_name) == strlen (tmp_str) &&
        g_ascii_strncasecmp (iface_name, tmp_str, strlen (iface_name)) == 0) {
      ret = win32_get_ip_for_interface (if_table->table[i].dwIndex);
      g_free (tmp_str);
      break;
    }
    g_free (tmp_str);
  }

  ip = g_inet_socket_address_new_from_string (ret);
  g_free (ret);

out:
  g_free (if_table);
  return ip;
}

#endif /* G_OS_UNIX/WIN32 */

/* END: Copy of nice_interfaces_get_ip_for_interface() from libnice.
 * Replace with that if/when we use libnice */
