#!/bin/bash
# vim: set sts=2 sw=2 et tw=0 :

build_dir=$(dirname $0)
cur_dir="${build_dir}/wrappers"

. "${cur_dir}/setup.sh"

"${build_dir}"/${progtype}/one-video-${progtype} "$@"
