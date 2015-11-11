# Define sources/libs/etc for the library and the test client

EXTRA_DIST = tests/supp/gst.supp

lib_LTLIBRARIES = lib/libonevideo.la
plugin_LTLIBRARIES = gst/proxy/libgstproxy.la

noinst_HEADERS = \
	lib/lib-priv.h \
	lib/lib-setup.h \
	gst/proxy/gstproxysink-priv.h \
	gst/proxy/gstproxysrc-priv.h

lib_libonevideo_la_SOURCES = \
	lib/lib.c lib/lib-priv.h lib/lib.h \
	lib/lib-setup.c lib/lib-setup.h \
	lib/outgoing.c lib/outgoing.h \
	lib/incoming.c lib/incoming.h \
	lib/utils.c lib/utils.h \
	lib/discovery.c lib/discovery.h \
	lib/comms.c lib/comms.h
lib_libonevideo_la_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS)
lib_libonevideo_la_LIBADD = $(GLIB_LIBS) $(GST_LIBS)
lib_libonevideo_la_LDFLAGS = $(ONE_VIDEO_LT_LDFLAGS) -no-undefined
lib_libonevideo_la_LIBTOOLFLAGS = --tag=disable-static

gst_proxy_libgstproxy_la_SOURCES = \
	gst/proxy/gstproxy.c \
	gst/proxy/gstproxysink.c \
	gst/proxy/gstproxysink.h \
	gst/proxy/gstproxysink-priv.h \
	gst/proxy/gstproxysrc.c	\
	gst/proxy/gstproxysrc.h \
	gst/proxy/gstproxysrc-priv.h
gst_proxy_libgstproxy_la_CFLAGS = $(GST_CFLAGS)
gst_proxy_libgstproxy_la_LIBADD = $(GST_LIBS)
gst_proxy_libgstproxy_la_LDFLAGS = -no-undefined
gst_proxy_libgstproxy_la_LIBTOOLFLAGS = --tag=disable-static

onevideoincludedir = $(includedir)/onevideo/
onevideoinclude_HEADERS = lib/lib.h

bin_PROGRAMS = app/one-video-app

app_one_video_app_SOURCES = app/main.c
app_one_video_app_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS) -I$(top_srcdir)/lib
app_one_video_app_LDADD = \
	$(top_srcdir)/lib/libonevideo.la \
	$(top_srcdir)/gst/proxy/libgstproxy.la \
	$(GLIB_LIBS) $(GST_LIBS)
