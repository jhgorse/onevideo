#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)
cur_dir="${build_dir}/wrappers"

. "${cur_dir}/setup.sh"

G_DEBUG="$G_DEBUG,fatal-warnings" GST_DEBUG="*:3,onevideo:6,$GST_DEBUG" \
	libtool --mode=execute \
	gdb --args \
	"${build_dir}"/${progtype}/one-video-${progtype} "$@"
