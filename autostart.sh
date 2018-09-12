#!/bin/bash
set -eu

export XDG_RUNTIME_DIR="/run/user/$(id -u)"

#pulseaudio --check || pulseaudio --start
icecast2 -b -c ~/icecast2/icecast.xml
#Xvfb :1 -screen 0 1x1x8 &
#sleep 2
#export DISPLAY=:1
#~/client/ts3client_runscript.sh >/dev/null 2>&1 &
#sleep 2
mpd || true
mpc play
