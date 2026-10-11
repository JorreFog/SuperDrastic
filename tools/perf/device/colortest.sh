#!/bin/sh
# colortest.sh: smoke test of rocknixds-colors under sway (ES idle): the daemon's tables, then the calibration screen
T=/storage/dsflip/opt; B=$T/rocknixds-colors; CF=/tmp/colors-test.conf
export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 ROCKNIXDS_COLORS=$CF
systemctl is-active -q dsflip-game && { echo "a game is running"; exit 3; }
chmod +x $B; rm -f $CF /tmp/rocknixds-colors.pid
echo "== before"; python3 $T/lutprobe.py
printf 'bottom.blue=0.9\nbottom.gamma=1.1\n' > $CF
$B --daemon 2>/tmp/colors-daemon.err & DP=$!
sleep 1.5; echo "== daemon with bottom.blue=0.9 gamma=1.1 (pid $DP, pidfile $(cat /tmp/rocknixds-colors.pid 2>/dev/null))"; python3 $T/lutprobe.py
kill -USR1 $DP; sleep 1; echo "== SIGUSR1 (off)"; python3 $T/lutprobe.py
printf 'top.red=0.95\n' > $CF; kill -HUP $DP; sleep 1; echo "== SIGHUP with top.red=0.95"; python3 $T/lutprobe.py
echo "== calibration screen"
$B --calibrate 2>/tmp/colors-cal.err & CP=$!
sleep 2
grim -o DSI-2 /tmp/cal-top.png 2>/dev/null; grim -o DSI-1 /tmp/cal-bottom.png 2>/dev/null; ls -la /tmp/cal-*.png 2>/dev/null | awk '{print $5, $9}'
echo "-- tables while it is up:"; python3 $T/lutprobe.py
k() { python3 $T/padkey.py $1 0.08; sleep 0.25; }
k 546; k 546; k 546; k 546          # red: 4 steps down (bottom screen)
k 545; k 546; k 546                 # green: 2 steps down
k 311                               # R: the next pattern
k 305                               # A: hide the settings
sleep 0.5; grim -o DSI-1 /tmp/cal-bottom2.png 2>/dev/null; grim -o DSI-2 /tmp/cal-top2.png 2>/dev/null
k 315                               # START: save
sleep 1.5
kill -0 $CP 2>/dev/null && { echo "the calibration screen is STILL UP: killing it"; kill $CP; } || echo "the calibration screen closed"
echo "-- saved file:"; cat $CF
echo "-- tables after saving:"; python3 $T/lutprobe.py
kill $DP; sleep 1; echo "== daemon stopped"; python3 $T/lutprobe.py
cat /tmp/colors-daemon.err /tmp/colors-cal.err 2>/dev/null | head -5
rm -f $CF; curl -s localhost:1234/isIdle
