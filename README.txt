Introduction
============
These are testing instructions when running all instances on a single machine.
As you will see, the -p/--port option needs to be passed to instances of the
GUI/CLI after the first GUI/CLI to use non-default ports for communication for
doing so. One must also use the -d/--device option when running multiple
instances or when one wants to select a particular video source device.

In general, a peer can either act as a call initiator or a call receiver. This
distinction is only valid for initiation and during the negotiation process.
Thereafter, all peers are in the same overall state, and communicate to each
other on an even footing. (The protocol embeds this behaviour but in practice
this has not been fully implemented yet).

When run without any arguments, both the CLI (command-line interface) and the
GUI (graphical user interface) listen on all interfaces on TCP port 5000 and
pick the first V4L2 camera they find. The GUI also sends out multicast discovery
probes on all interfaces to find other instances of OneVideo applications.

Hence, the CLI when called without any arguments will just idle and listen for
incoming discovery probes and respond to them to indicate its existence. It will
also auto-accept any incoming calls (although it is trivial to add a prompt for
the same).

The CLI can also be called in a mode in which it will discover peers for
5 seconds and then initiate a call with all of them. The GUI on the other hand
will show a constantly-updating list of discovered peers; any of which can be
selected and a call can be initiated with them.

Building
========
You will need to install the appropriate development packages for GStreamer 1.6
(or later) and GTK+ 3.10 (or later). With that done, run the following:

$ ./autogen.sh
$ make -j4

CLI Examples
============
* Listening peer that listens on port 6000 for TCP comms and uses free ports
  starting from 6001 for RTP/UDP communication:
 $ ./one-video-cli.sh -p 6000 -d /dev/video1

* More listening peers (optional):
 $ ./one-video-cli.sh -p 7000 -d /dev/video2

* Manual call initiator (and participating peer):
 $ ./one-video-cli.sh -d /dev/video0 -c localhost:6000 # Add -c localhost:7000 etc as required

* Automatic call initiator (and participating peer):
 $ ./one-video-cli.sh -d /dev/video0 --discover

See all the help options by passing --help

The initiator will dial all the specified remotes and start a call. The way to
end a call is by pressing Ctrl+C.

Using the GUI
=============
By design, only a single instance of the GUI can be run at once on a machine.
Hence, to test calling on the same machine, one must run CLI interfaces as well.

* Listening or call-initiating peer:
 $ ./one-video-gui.sh

The GUI itself is currently very simple, and is completely self-explanatory.
More features will be added later, but the intent is to continue to keep it
self-explanatory.

Known bugs:
1) The window does not shrink back once a call is ended
2) Currently, gtksink is used instead of gtkglsink, so performance can be bad

See all the help options by passing --help

Debugging
=========
To see debug output, pass GST_DEBUG=onevideo:6 as an environment variable.

For memcheck and gdb, use ./memcheck-one-video-(cli|gui).sh or
./debug-one-video-(cli|gui).sh (use cli or gui as appropriate)

Passing an invalid device to the -d/--device option will cause the library to
insert a test video source instead. This can be useful for debugging or for
testing in the absence of cameras.
