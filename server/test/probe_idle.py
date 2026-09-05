#!/usr/bin/env python3
"""A1b point 3: what a match does once it has been sitting in GAMEOVER.

The normal probe.py restarts within a second of the match ending, so it never
reaches the wind-down. This one ends a match and then waits, checking the two
thresholds in constants.h:

    GAMEOVER_SIM_SECONDS   15s  stop ticking the world (still broadcasting)
    GAMEOVER_LOBBY_SECONDS 60s  free the world, phase returns to LOBBY

Takes ~80s. Expected to DIFFER from a pre-A1b server, which simulates an ended
match forever - that difference is the feature.

    cd server && ./gameserver &
    python3 test/probe_idle.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import socket, json, struct, threading
from probe import C, OPTS, enc   # reuse the client

a = C("IDLER")
a.hello()
time.sleep(1.0)
print(f"joined: slot={a.slot} phase={a.phase}")

a.send({"type": "start", **OPTS})
time.sleep(7.0)
print(f"match running: phase={a.phase} epoch={a.epoch}")
assert a.phase == "playing", f"expected playing, got {a.phase}"

a.send({"type": "endmatch"})
time.sleep(1.0)
print(f"ended: phase={a.phase}")
assert a.phase == "gameover", f"expected gameover, got {a.phase}"

t0 = time.time()
for target in (10, 20, 40, 58, 66, 75):
    while time.time() - t0 < target:
        time.sleep(0.2)
    print(f"  +{target:3d}s  phase={a.phase}")

elapsed = time.time() - t0
assert a.phase == "lobby", f"expected lobby after {elapsed:.0f}s, got {a.phase}"
print("PASS: wound down to lobby")
