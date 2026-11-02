#!/usr/bin/env python3
"""Judges a GM_DEBUG_CURSOR log: what real macOS delivered to the real app.

`hid` lines are the raw device motion, which warps can't touch: the truth about
the hand. The rules, in view points:
  teleport  our cursor moved far more than the hand did around that event
  exit      the system cursor did not appear where our cursor left the window
  double    the same event delivered twice, counted twice
  click     a real click (hidclick) that never reached the view
usage: cursorcheck.py <log> [-v]
"""
import bisect
import math
import sys

WINDOW = 0.040   # seconds of hand motion to compare an event with
RATIO, FLOOR = 4.0, 30.0  # raw units aren't points, and acceleration differs

events = [l.split() for l in open(sys.argv[1]) if l.strip()]
verbose = "-v" in sys.argv
hid = [(float(f[0]), math.hypot(float(f[2]), float(f[3]))) for f in events if f[1] == "hid"]
times = [t for t, _ in hid]
cum = [0.0]
for _, v in hid:
    cum.append(cum[-1] + v)

def hand(t):
    i, j = bisect.bisect_left(times, t - WINDOW), bisect.bisect_right(times, t + WINDOW)
    return cum[j] - cum[i]

bad = {"teleport": [], "exit": [], "double": [], "click": []}
last = None
moves = captures = releases = 0
pending = None
for f in events:
    t, kind = float(f[0]), f[1]
    if kind == "capture":
        captures += f[2] == "1"
        releases += f[2] == "0"
    elif kind == "move":
        moves += 1
        if last is not None and t - float(last[0]) < 0.002 and last[3:8] == f[3:8] and f[9] == "1":
            bad["double"].append(f"t={t:.3f} delta {f[6]} {f[7]}")
        last = f
        loc = (float(f[3]), float(f[4]))
        captured = f[9] == "1"
        before, after = (float(f[11]), float(f[12])), (float(f[14]), float(f[15]))
        if captured:
            step = math.dist(before, after)
            if hid and step > FLOOR and step > RATIO * hand(t):
                bad["teleport"].append(f"t={t:.3f} moved {step:.0f} hand {hand(t):.0f} {before}->{after}")
            pending = after
        elif pending is not None:
            if f[-1] == "0" and math.dist(pending, loc) > 60:
                bad["exit"].append(f"t={t:.3f} left at {pending} appeared at {loc}")
            pending = None

viewed = [float(f[0]) for f in events if f[1] == "click"]
for f in events:
    if f[1] == "hidclick":
        t = float(f[0])
        if not any(abs(t - v) < 0.1 for v in viewed):
            bad["click"].append(f"t={t:.3f} never reached the view")

print(f"{sum(f[1] == 'hidclick' for f in events)} clicks, {moves} moves, {len(hid)} hid, {captures} captures, {releases} releases")
if not hid:
    print("no hid lines: the hand is unknown, this log proves nothing")
    sys.exit(2)
for name, items in bad.items():
    print(f"{name}: {len(items)} {'FAIL' if items else 'ok'}")
    for i in items[: (len(items) if verbose else 6)]:
        print("   ", i)
sys.exit(1 if any(bad.values()) else 0)
