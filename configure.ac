#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.
# vim: set sts=4 sw=4 et :

AC_PREREQ([2.69])
AC_INIT([one-video], [0.0.1], [nirbheek@centricular.com])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_HEADERS([config.h])

# Save this value here, since automake will set cflags later
cflags_set=${CFLAGS:+set}

AM_INIT_AUTOMAKE([-Wno-portability 1.14 no-dist-gzip dist-xz tar-ustar subdir-objects])

GLIB_REQ=2.32.0
GST_REQ=1.5.2
GTK_REQ=3.10

# Checks for programs.
AC_PROG_CC
LT_INIT([dlopen win32-dll disable-static])

# Check for options
AC_ARG_ENABLE(debug,
              AS_HELP_STRING([--enable-debug=yes/no],          
                             [turn on debugging default=yes]),,
              enable_debug=yes)

if test "x$enable_debug" = "xyes"; then
    if test "x$cflags_set" != "xset" ; then
        CFLAGS="-O0 -ggdb -Wall"
    else
        CFLAGS="$CFLAGS -g -Wall"
    fi
else
    CFLAGS="$CFLAGS -g -Wall"
fi

dnl set the plugindir where plugins should be installed (for src/Makefile.am)
if test "x${prefix}" = "x$HOME"; then
  plugindir="$HOME/.gstreamer-1.0/plugins"
else
  plugindir="\$(libdir)/gstreamer-1.0"
fi
AC_SUBST(plugindir)

# Check for libraries
PKG_CHECK_MODULES(GLIB, glib-2.0 >= $GLIB_REQ gio-2.0 >= GLIB_REQ gmodule-no-export-2.0)
PKG_CHECK_MODULES(GST, gstreamer-1.0 >= $GST_REQ)
PKG_CHECK_MODULES(GTK, gtk+-3.0 >= $GTK_REQ)

# Check for header files
# FIXME: This is only for Linux
AC_CHECK_HEADERS([arpa/inet.h netinet/in.h net/if.h ifaddrs.h])

dnl FIXME: Properly tie this to the one-video version number
ONE_VIDEO_LT_LDFLAGS="-version-info 0:1:0"
AC_SUBST(ONE_VIDEO_LT_LDFLAGS)

AC_CONFIG_FILES(
Makefile
)
AC_OUTPUT
