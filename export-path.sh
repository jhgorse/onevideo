#!/bin/echo Should be run as: source
extra_plugin_path="${build_dir}/gst/proxy/.libs"

if [[ -n "${GST_PLUGIN_PATH_1_0}" ]]; then
  export GST_PLUGIN_PATH_1_0="${GST_PLUGIN_PATH_1_0}:${extra_plugin_path}"
else
  export GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${extra_plugin_path}"
fi
