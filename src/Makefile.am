# Define sources/libs/etc for the library and the test client

lib_LTLIBRARIES = libonevideo.la
libonevideo_la_SOURCES = lib.c lib.h utils.c utils.h
libonevideo_la_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS)
libonevideo_la_LIBADD = $(GLIB_LIBS) $(GST_LIBS)
libonevideo_la_LDFLAGS = $(ONE_VIDEO_LT_LDFLAGS) -no-undefined
libonevideo_la_LIBTOOLFLAGS = --tag=disable-static

bin_PROGRAMS = one-video-app

one_video_app_SOURCES = main.c
one_video_app_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS)
one_video_app_LDADD = libonevideo.la
