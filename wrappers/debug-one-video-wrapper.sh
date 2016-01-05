#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)
cur_dir="${build_dir}/wrappers"

. "${cur_dir}/setup.sh"

G_DEBUG="$G_DEBUG,fatal-criticals" GST_DEBUG="*:3,onevideo:6,$GST_DEBUG" \
	$LIBTOOL --mode=execute \
	$DEBUGGER \
	"${build_dir}"/${progtype}/one-video-${progtype} "$@"
