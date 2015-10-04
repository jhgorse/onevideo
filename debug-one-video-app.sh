#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)

GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${build_dir}/gst/proxy/.libs" G_DEBUG="fatal-criticals" \
	libtool --mode=execute \
	gdb --args \
	"${build_dir}"/app/one-video-app "$@"
