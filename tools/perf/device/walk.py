# walk.py [hold_secs] [square|lr]: walks in a square (down, left, right, up) or just right and left on the real gamepad
# until killed; one process for the whole run (starting an interpreter per key press costs a CPU burst every time,
# which shows as hitches). The square drifts when something blocks a side and ends up in tall grass sooner or later
# (a wild battle: not the scene being measured); "lr" stays where it started.
import glob, os, signal, struct, sys, time
dev = next(d for d in sorted(glob.glob("/sys/class/input/event*")) if open(d + "/device/name").read().strip() == "retrogame_joypad")
fd = os.open("/dev/input/" + os.path.basename(dev), os.O_WRONLY)
hold = float(sys.argv[1]) if len(sys.argv) > 1 else 1.2
def ev(t, c, v): os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))
cur = [0]
def stop(*a):
    if cur[0]: ev(1, cur[0], 0); ev(0, 0, 0)
    sys.exit(0)
signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
while True:
    for k in ((547, 546) if len(sys.argv) > 2 and sys.argv[2] == "lr" else (545, 546, 547, 544)):
        cur[0] = k; ev(1, k, 1); ev(0, 0, 0); time.sleep(hold); ev(1, k, 0); ev(0, 0, 0); cur[0] = 0
