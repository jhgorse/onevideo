#!/bin/bash
# vim: set sts=4 sw=4 et tw=0 :

set -e

DEFAULT_IFACE="lo"
DEFAULT_LOSS="1%"
DEFAULT_DELAY="10ms 2ms"

if [[ $1 == "--help" || $1 == "-h" ]]; then
    echo "Usage: $0 <packet loss> <delay> <network interface>"
    echo
    echo "Default packet loss is '$DEFAULT_LOSS'"
    echo "Default delay is '$DEFAULT_DELAY' (means ${DEFAULT_DELAY%% *}±${DEFAULT_DELAY##* })"
    echo "Default network interface is '$DEFAULT_IFACE'"
    exit
fi

LOSS=${1:-$DEFAULT_LOSS}
DELAY=${2:-$DEFAULT_DELAY}
IFACE=${3:-$DEFAULT_IFACE}

run_tc_qdisc() {
    sudo tc qdisc "$@"
}

set_qdisc() {
    run_tc_qdisc del dev "$IFACE" root &>/dev/null \
        && echo "Removed existing qdisc on $IFACE"
    run_tc_qdisc add dev "$IFACE" root handle 1:0 netem loss $LOSS delay $DELAY \
        && echo "Inserted netem qdisc on $IFACE with loss $LOSS and delay $DELAY"
}

unset_qdisc() {
    if run_tc_qdisc del dev "$IFACE" root handle 1:0 netem; then
        echo "Cleaned up"
    else
        echo "Couldn't cleanup"
    fi
}

trap unset_qdisc EXIT

# Block forever. When we exit, we undo the above changes.
set_qdisc && read i
