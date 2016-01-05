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
#include "utils.h"
#include "lib-priv.h"

#include <string.h>

#include <glib/gstdio.h>

GInetSocketAddress *
ov_inet_socket_address_from_string (const gchar * addr_s)
{
  guint port;
  gchar **split = NULL;
  GSocketAddress *addr = NULL;

  g_return_val_if_fail (addr_s != NULL, NULL);

  if (strlen (addr_s) < 1) {
    GST_ERROR ("Received empty string as argument");
    goto out;
  }

  split = g_strsplit (addr_s, ":", 2);

  if (g_strv_length (split) == 1) {
    port = OV_DEFAULT_COMM_PORT;
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
ov_inet_socket_address_to_string (const GInetSocketAddress * addr)
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
ov_inet_socket_address_equal (GInetSocketAddress * addr1,
    GInetSocketAddress * addr2)
{
  if (g_inet_socket_address_get_port (addr1) ==
      g_inet_socket_address_get_port (addr2) &&
      g_inet_address_equal (g_inet_socket_address_get_address (addr1),
        g_inet_socket_address_get_address (addr2)))
    return TRUE;
  return FALSE;
}

gboolean
ov_inet_socket_address_is_iface (GInetSocketAddress * saddr, GList * ifaces,
    guint16 port)
{
  GList *l;
  GInetAddress *addr;
  gboolean ret = TRUE;

  for (l = ifaces; l != NULL; l = l->next) {
    GInetSocketAddress *iface_saddr;
    addr = ov_get_inet_addr_for_iface (l->data);
    iface_saddr =
      G_INET_SOCKET_ADDRESS (g_inet_socket_address_new (addr, port));
    g_object_unref (addr);
    if (ov_inet_socket_address_equal (saddr, iface_saddr)) {
      g_object_unref (iface_saddr);
      goto out;
    }
    g_object_unref (iface_saddr);
  }

  ret = FALSE;
out:
  return ret;
}

#ifdef __linux
GstDevice *
ov_get_device_from_device_path (GList * devices, const gchar * device_path)
{
  GList *device;
  gboolean some_device_had_props = FALSE;

  if (devices == NULL) {
    GST_ERROR ("No video sources detected");
    return NULL;
  }

  for (device = devices; device; device = device->next) {
    const gchar *path;
    GstStructure *props;

    g_object_get (GST_DEVICE (device->data), "properties", &props, NULL);
    if (props == NULL)
      continue;

    some_device_had_props = TRUE;
    path = gst_structure_get_string (props, "device.path");

    if (g_strcmp0 (path, device_path) == 0) {
      GST_DEBUG ("Found device for path '%s'\n", device_path);
      gst_structure_free (props);
      return GST_DEVICE (device->data);
    }

    gst_structure_free (props);
  }

  if (!some_device_had_props)
    GST_ERROR ("None of the probed devices had properties set; unable to"
        " match with specified device path! Falling back to using the test"
        " video source. Upgrade GStreamer or choose the device yourself.");
  else
    GST_ERROR ("Selected device path '%s' wasn't found. Falling back to"
        " using the test video source.", device_path);

  return NULL;
}
#endif

#define OV_PACKAGE_BASE "gst-plugins-base"
#define OV_PACKAGE_GOOD "gst-plugins-good"
#define OV_PACKAGE_BAD  "gst-plugins-bad"
#define OV_PACKAGE_UGLY "gst-plugins-ugly"

struct plugin_names {
  const gchar *feature_name;
  const gchar *package_name;
};

static struct plugin_names plugins_req[] = {
  { "jpegenc",            OV_PACKAGE_GOOD },
  { "jpegdec",            OV_PACKAGE_GOOD },
  { "pulsesrc",           OV_PACKAGE_GOOD },
  { "pulsesink",          OV_PACKAGE_GOOD },
  { "rtpjpegpay",         OV_PACKAGE_GOOD },
  { "rtpjpegdepay",       OV_PACKAGE_GOOD },
  { "rtpbin",             OV_PACKAGE_GOOD },
  { "udpsink",            OV_PACKAGE_GOOD },
  { "udpsrc",             OV_PACKAGE_GOOD },

  { "audiomixer",         OV_PACKAGE_BAD },
  { "glimagesink",        OV_PACKAGE_BAD },
  { "opusenc",            OV_PACKAGE_BAD },
  { "opusdec",            OV_PACKAGE_BAD },
};

static struct plugin_names plugins_gtk = { "gtkglsink", OV_PACKAGE_BAD };
static struct plugin_names plugins_gtk_alt = { "gtksink", OV_PACKAGE_BAD };

GHashTable *
ov_get_missing_gstreamer_plugins (const gchar * toolkit)
{
  int ii;
  GstRegistry *reg;
  GHashTable *missing;

  if (!gst_is_initialized ()) {
    g_printerr ("Must initialize GStreamer before calling this!\n");
    return NULL;
  }

  reg = gst_registry_get ();
  missing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (ii = 0; ii < G_N_ELEMENTS (plugins_req); ii++) {
    if (gst_registry_check_feature_version (reg, plugins_req[ii].feature_name,
          1, 6, 0))
      continue;
    g_hash_table_insert (missing, g_strdup (plugins_req[ii].feature_name),
        g_strdup (plugins_req[ii].package_name));
  }

  /* If neither gtlgksink nor gtksink are found,
   * then return both as missing plugins */
  if (toolkit != NULL &&
      g_strcmp0 (toolkit, "gtk") == 0 &&
      !gst_registry_check_feature_version (reg, plugins_gtk.feature_name,
        1, 6, 0) &&
      !gst_registry_check_feature_version (reg, plugins_gtk_alt.feature_name,
        1, 6, 0)) {
    g_hash_table_insert (missing, g_strdup (plugins_gtk.feature_name),
        g_strdup (plugins_gtk.package_name));
    g_hash_table_insert (missing, g_strdup (plugins_gtk_alt.feature_name),
        g_strdup (plugins_gtk_alt.package_name));
  }

  if (g_hash_table_size (missing) > 0)
    return missing;
  
  g_hash_table_unref (missing);
  return NULL;
}

/* OS-specific implementations */

#ifdef G_OS_UNIX

#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>

/* Copy of nice_interfaces_get_local_interfaces() from libnice.
 * Same for win32 version below. */
GList *
ov_get_network_interfaces (void)
{
  GList *interfaces = NULL;
  struct ifaddrs *ifa, *results;

  if (getifaddrs (&results) < 0) {
    return NULL;
  }

  /* Loop and get each interface the system has, one by one... */
  for (ifa = results; ifa; ifa = ifa->ifa_next) {
    /* no ip address from interface that is down */
    if ((ifa->ifa_flags & IFF_UP) == 0)
      continue;

    if (ifa->ifa_addr == NULL)
      continue;

    if (ifa->ifa_addr->sa_family == AF_INET) {
      if (g_strcmp0 (ifa->ifa_name, "lo") == 0)
        continue;
      GST_DEBUG ("Found interface : %s", ifa->ifa_name);
      interfaces = g_list_prepend (interfaces, g_strdup (ifa->ifa_name));
    }
  }

  freeifaddrs (results);

  return interfaces;
}

/* Copy of nice_interfaces_get_ip_for_interface() from libnice.
 * Replace with that if/when we use libnice. Same for win32 version below. */
GInetAddress *
ov_get_inet_addr_for_iface (const gchar * iface_name)
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
  GST_TRACE ("Address for %s: %s", iface_name, inet_ntoa (sa.in->sin_addr));

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

GList *
ov_get_network_interfaces (void)
{
  ULONG size = 0;
  PMIB_IFTABLE if_table;
  GList * ret = NULL;

  GetIfTable(NULL, &size, TRUE);

  if (!size)
    return NULL;

  if_table = (PMIB_IFTABLE)g_malloc0(size);

  if (GetIfTable(if_table, &size, TRUE) == ERROR_SUCCESS) {
    DWORD i;
    for (i = 0; i < if_table->dwNumEntries; i++) {
      gchar *iface_name = if_table->table[i].bDescr;
      GST_DEBUG ("Found interface : %s", iface_name);
      ret = g_list_prepend (ret, g_strdup (iface_name));
    }
  }

  g_free(if_table);

  return ret;
}

GInetAddress *
ov_get_inet_addr_for_iface (const gchar * iface_name)
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
