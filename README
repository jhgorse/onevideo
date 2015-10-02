Steps:
1) Attach webcams equal to the number of peers you want to test and a microphone
2) Compile ./autogen.sh && make
3) Run ./one-video-app.sh as follows:

* Listening peer that listens on port 6000 for TCP comms and uses free ports
  starting from 6001 for RTP/UDP communication:
 $ ./one-video-app.sh -p 6000 -d /dev/video1

* More listening peers (optional):
 $ ./one-video-app.sh -p 7000 -d /dev/video2

* Call initiator (and participator peer):
 $ ./one-video-app.sh -i lo -p 5000 -d /dev/video0 -c localhost:6000 # Add -c localhost:7000 etc as required

The initiator will dial all the specified remotes and start a call. The only way
to end it properly is currently to Ctrl+C all the remotes.

To see debug output, use GST_DEBUG=onevideo:6

For memcheck and gdb, use ./memcheck-one-video-app.sh or ./debug-one-video-app.sh respectively
