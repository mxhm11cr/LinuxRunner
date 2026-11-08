#!/bin/sh
# LinuxRunner session sound, started by session.sh in its own process group as
# the session user. PulseAudio plays every program into a null sink; parec
# records the sink's monitor with low latency and audio-relay offers the PCM
# (48 kHz stereo s16le) on the loopback port the LinuxRunner app reads.
# Programs (and terminals joining the desktop) find the server through
# $XDG_RUNTIME_DIR/pulse/native.
set -u
cd "$(dirname "$0")"
session=$1 port=$2
# Plasma already owns the whole session process group; keep its audio
# children there so the same shutdown covers them without an orphan race.
case "${3:-own}" in
 own) echo $$ > "$session/audio-group";;
 inherited-group) ;;
 *) echo 'AUDIO: invalid process-group mode'; exit 2;;
esac
pulseaudio --daemonize=no --exit-idle-time=-1 --disallow-exit -n --use-pid-file=no --log-target=stderr \
    -L "module-native-protocol-unix auth-anonymous=1" \
    -L "module-null-sink sink_name=linuxrunner sink_properties=device.description=LinuxRunner rate=48000 channels=2 format=s16le" &
server=$!
i=0
until pactl info >/dev/null 2>&1; do
    kill -0 "$server" 2>/dev/null || { echo 'AUDIO: pulseaudio exited during startup'; exit 1; }
    i=$((i+1)); [ "$i" -lt 120 ] || { echo 'AUDIO: server not ready after 30 s'; exit 1; }
    sleep .25
done
echo "AUDIO: server ready port=$port"
# 40 ms blocks: low enough for lip sync, few enough wakeups for the interpreter.
parec --raw --latency-msec=40 --format=s16le --rate=48000 --channels=2 -d linuxrunner.monitor | ./audio-relay "$port"
echo 'AUDIO: recorder or relay ended'
