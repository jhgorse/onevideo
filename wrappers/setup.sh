#!/bin/echo Should be run as: source
# vim: set sts=2 sw=2 et :
extra_plugin_path="${build_dir}/gst/proxy/.libs"

if [[ -n "${GST_PLUGIN_PATH_1_0}" ]]; then
  export GST_PLUGIN_PATH_1_0="${GST_PLUGIN_PATH_1_0}:${extra_plugin_path}"
else
  export GST_PLUGIN_PATH="${GST_PLUGIN_PATH}:${extra_plugin_path}"
fi

case "$(uname -s)" in
  Darwin)
    DEBUGGER="lldb --";;
  Linux)
    DEBUGGER="gdb -q --args";;
  *)
    # Fall back to gdb on other platforms too...
    DEBUGGER="gdb -q --args";;
esac

# Most of the time, the default libtool is what we want
if [[ -x "$(type -P libtool)" ]]; then
  export LIBTOOL=$(type -P libtool)
fi

# On OS X, if Homebrew is used, libtool is installed as glibtool
if [[ "$(uname -s)" = "Darwin" && -x "$(type -P glibtool)" ]]; then
  export LIBTOOL=$(type -P glibtool)
fi

progname=$(basename $0)
if [[ ${progname} =~ cli\.sh ]]; then
  progtype="cli"
elif [[ ${progname} =~ gui\.sh ]]; then
  progtype="gui"
elif [[ ${progname} =~ test\.sh ]]; then
  progtype="test"
else
  echo "Unknown program type, exiting"
  return 1
fi
unset progname

# Enable audio echo cancellation in Pulseaudio if supported
export PULSE_PROP='filter.want=echo-cancel'
