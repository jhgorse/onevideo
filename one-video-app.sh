#!/bin/bash
# vim: set sts=2 sw=2 et tw=0 :

build_dir=$(dirname $0)

. "${build_dir}/export-path.sh"

"${build_dir}"/app/one-video-app "$@"
