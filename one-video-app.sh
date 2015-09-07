#!/bin/bash
# vim: set tw=0 :

build_dir=$(dirname $0)

GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${build_dir}/gst/proxy/.libs" "${build_dir}"/app/one-video-app "$@"
