# Define sources/libs/etc for the library and the test client

EXTRA_DIST = tests/supp/gst.supp

lib_LTLIBRARIES = lib/libonevideo.la
lib_libonevideo_la_SOURCES = lib/lib.c lib/lib.h lib/utils.c lib/utils.h
lib_libonevideo_la_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS)
lib_libonevideo_la_LIBADD = $(GLIB_LIBS) $(GST_LIBS)
lib_libonevideo_la_LDFLAGS = $(ONE_VIDEO_LT_LDFLAGS) -no-undefined
lib_libonevideo_la_LIBTOOLFLAGS = --tag=disable-static

onevideoincludedir = $(includedir)/onevideo/
onevideoinclude_HEADERS = lib/lib.h

bin_PROGRAMS = app/one-video-app

app_one_video_app_SOURCES = app/main.c
app_one_video_app_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS) -I$(top_srcdir)/lib
app_one_video_app_LDADD = $(top_srcdir)/lib/libonevideo.la
