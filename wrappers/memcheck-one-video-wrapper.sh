#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)
cur_dir="${build_dir}/wrappers"

. "${cur_dir}/setup.sh"

: ${VALGRIND_LOG:="valgrind-memcheck.log"}
G_SLICE="always-malloc" G_DEBUG="$G_DEBUG,gc-friendly" GST_DEBUG="*:3,onevideo:6,$GST_DEBUG" \
	$LIBTOOL --mode=execute \
	valgrind --suppressions="${build_dir}"/tests/supp/gst.supp --tool=memcheck --leak-check=full --leak-resolution=high --num-callers=30 --log-file="${VALGRIND_LOG}" \
	"${build_dir}"/${progtype}/one-video-${progtype} "$@"
