#!/bin/echo Should be run as: source
# vim: set sts=2 sw=2 et :
extra_plugin_path="${build_dir}/gst/proxy/.libs"

if [[ -n "${GST_PLUGIN_PATH_1_0}" ]]; then
  export GST_PLUGIN_PATH_1_0="${GST_PLUGIN_PATH_1_0}:${extra_plugin_path}"
else
  export GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${extra_plugin_path}"
fi

progname=$(basename $0)
if [[ ${progname} =~ cli\.sh ]]; then
  progtype="cli"
elif [[ ${progname} =~ gui\.sh ]]; then
  progtype="gui"
else
  echo "Unknown program type, exiting"
  return 1
fi
unset progname
