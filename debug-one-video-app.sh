#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)

GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${build_dir}/gst/proxy/.libs" G_DEBUG="$G_DEBUG,fatal-criticals" GST_DEBUG="*:3,onevideo:6,$GST_DEBUG" \
	libtool --mode=execute \
	gdb --args \
	"${build_dir}"/app/one-video-app "$@"
