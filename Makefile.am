# Define sources/libs/etc for the library and the test cli client

EXTRA_DIST = tests/supp/gst.supp

lib_LTLIBRARIES = onevideo/libonevideo.la
plugin_LTLIBRARIES = gst/proxy/libgstproxy.la

noinst_HEADERS = \
	onevideo/lib-priv.h \
	onevideo/outgoing.h \
	onevideo/incoming.h \
	onevideo/comms.h \
	onevideo/discovery.h \
	onevideo/ov-local-peer-priv.h \
	onevideo/ov-local-peer-setup.h \
	gst/proxy/gstproxysink-priv.h \
	gst/proxy/gstproxysrc-priv.h

onevideo_libonevideo_la_SOURCES = \
	onevideo/ov-peer.c onevideo/ov-peer.h \
	onevideo/ov-local-peer.c onevideo/ov-local-peer.h \
	onevideo/ov-local-peer-setup.c onevideo/ov-local-peer-setup.h \
	onevideo/ov-local-peer-audio.c onevideo/ov-local-peer-audio.h \
	onevideo/ov-local-peer-priv.h onevideo/ov-remote-peer.h \
	onevideo/ov-discovered-peer.c onevideo/ov-discovered-peer.h \
	onevideo/lib.c onevideo/lib-priv.h onevideo/lib.h \
	onevideo/outgoing.c onevideo/outgoing.h \
	onevideo/incoming.c onevideo/incoming.h \
	onevideo/utils.c onevideo/utils.h \
	onevideo/discovery.c onevideo/discovery.h \
	onevideo/comms.c onevideo/comms.h onevideo/zhelpers.h \
	onevideo/ov-local-peer-audio-processing.cc onevideo/ov-local-peer-audio-processing.h
onevideo_libonevideo_la_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS) -arch x86_64
onevideo_libonevideo_la_LIBADD = $(GLIB_LIBS) $(GST_LIBS) onevideo/libffts.la -lm
onevideo_libonevideo_la_LDFLAGS = $(ONE_VIDEO_LT_LDFLAGS) -no-undefined -Wl,-headerpad_max_install_names \
	-headerpad_max_install_names $(top_builddir)/../webrtc-audio-processing/webrtc/modules/audio_processing/.libs/libwebrtc_audio_processing.la \
	$(top_builddir)/../gstreamer/libs/gst/base/.libs/libgstbase-1.0.la
onevideo_libonevideo_la_LIBTOOLFLAGS = --tag=disable-static
WEBRTC_TRUNK_DIR = $(top_builddir)/../webrtc-audio-processing
onevideo_libonevideo_la_CXXFLAGS = -O0 -g3 -std=c++11 -DWEBRTC_POSIX -DWEBRTC_AUDIO_PROCESSING_ONLY_BUILD -c -Wall -I$(WEBRTC_TRUNK_DIR) -I$(WEBRTC_TRUNK_DIR)/build/include/webrtc_audio_processing $(GST_CFLAGS)

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
onevideoinclude_HEADERS = onevideo/lib.h

bin_PROGRAMS = cli/one-video-cli gui/one-video-gui tests/audio-processing-test

tests_audio_processing_test_SOURCES = tests/audio-processing-test.c onevideo/ov-local-peer-audio-processing.h
tests_audio_processing_test_CFLAGS = -I$(top_builddir)/onevideo -O0 -g3 -arch x86_64
tests_audio_processing_test_LDADD = $(top_builddir)/onevideo/libonevideo.la -lm
tests_audio_processing_test_LDFLAGS =

cli_one_video_cli_SOURCES = cli/main.c
cli_one_video_cli_CFLAGS = \
	-I$(top_srcdir) \
	$(GLIB_CFLAGS) $(GST_CFLAGS)
cli_one_video_cli_LDADD = \
	$(top_builddir)/onevideo/libonevideo.la \
	$(top_builddir)/gst/proxy/libgstproxy.la \
	$(GLIB_LIBS) $(GST_LIBS)

gui_resource_files = $(shell glib-compile-resources --sourcedir=$(srcdir)/gui --generate-dependencies $(srcdir)/gui/ovg.gresource.xml)
gui/ovg-resources.c: gui/ovg.gresource.xml $(gui_resource_files)
	glib-compile-resources --target=$@ --sourcedir=$(srcdir)/gui --generate-source --c-name ovg $(srcdir)/gui/ovg.gresource.xml
gui/ovg-resources.h: gui/ovg.gresource.xml $(gui_resource_files)
	glib-compile-resources --target=$@ --sourcedir=$(srcdir)/gui --generate-header --c-name ovg $(srcdir)/gui/ovg.gresource.xml

gui_one_video_gui_SOURCES = \
	gui/ovg-app.c gui/ovg-app.h \
	gui/ovg-appwin.c gui/ovg-appwin.h \
	gui/ovg-main.c gui/ovg-resources.c
gui_one_video_gui_CFLAGS = \
	-I$(top_srcdir) \
	$(GLIB_CFLAGS) $(GST_CFLAGS) $(GTK_CFLAGS)
gui_one_video_gui_LDADD = \
	$(top_builddir)/onevideo/libonevideo.la \
	$(top_builddir)/gst/proxy/libgstproxy.la \
	-lm $(GLIB_LIBS) $(GST_LIBS) $(GTK_LIBS)
