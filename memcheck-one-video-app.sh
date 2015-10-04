#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)

: ${VALGRIND_LOG:="valgrind-memcheck.log"}
GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${build_dir}/gst/proxy/.libs" G_SLICE="always-malloc" G_DEBUG="$G_DEBUG,gc-friendly" \
	libtool --mode=execute \
	valgrind --suppressions="${build_dir}"/tests/supp/gst.supp --tool=memcheck --leak-check=full --leak-resolution=high --num-callers=12 --log-file="${VALGRIND_LOG}" \
	"${build_dir}"/app/one-video-app "$@"
