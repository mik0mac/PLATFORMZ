#!/usr/bin/env python3
"""Put a full match under load so A4's PERF line measures something realistic.

Connects N clients, starts a match at a chosen map size with the roster maxed to
8, and holds it while everyone sends input at 60 Hz. Empty slots fill with bots,
so an 8-slot match is exercised whether 2 clients connect or 8.

    cd server && PLATFORMZ_PERF=1 ./gameserver &
    python3 test/probe_load.py --clients 8 --map XL --seconds 60

Egress scales with connected CLIENTS, tick cost with the ROSTER. Both matter:
use --clients 8 for a realistic egress figure, since the server sends a full
state packet to each one 60 times a second.
"""
import argparse, time, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

MAPS = {"SMALL":  (90.0,  64, 12),
        "MEDIUM": (120.0, 128, 18),
        "LARGE":  (240.0, 256, 24),
        "XL":     (360.0, 576, 36)}

ap = argparse.ArgumentParser()
ap.add_argument("--clients", type=int, default=8)
ap.add_argument("--map", choices=list(MAPS), default="MEDIUM")
ap.add_argument("--seconds", type=int, default=45)
a = ap.parse_args()

half, plat, roid = MAPS[a.map]
cs = []
for i in range(a.clients):
    c = C(f"LOAD{i}")
    c.send({"type": "hello", "name": f"LOAD{i}"})
    cs.append(c)
    time.sleep(0.15)

# UDP is unreliable, so a hello can be lost; the real client resends until it is
# welcomed. Do the same rather than assuming one shot lands.
for _ in range(10):
    time.sleep(0.4)
    missing = [c for c in cs if c.slot is None]
    if not missing: break
    for c in missing: c.send({"type": "hello", "name": c.name})
joined = sum(1 for c in cs if c.slot is not None)
print(f"{joined}/{a.clients} clients joined")
if joined < a.clients:
    print("  (short of the full roster - egress figures will be low)")

host = cs[0]
opts = dict(OPTS)
opts.update({"half": half, "plat": plat, "roid": roid, "nplayers": 8})
host.send({"type": "start", **opts})
time.sleep(7.0)
print(f"map={a.map} phase={host.phase} roster={host.nplayers}")

print(f"driving input for {a.seconds}s...")
end = time.time() + a.seconds
i = 0
while time.time() < end:
    for c in cs:
        c.seq += 1
        c.send({"seq": c.seq, "ep": c.epoch, "mx": 0.4, "mz": 1.0, "jp": i % 20 == 0,
                "grav": False, "fire": i % 12 == 0, "yaw": -1.57 + (i % 60) * 0.02,
                "pitch": 0.0})
    i += 1
    time.sleep(1.0 / 60.0)

print(f"done: phase={host.phase} roster={host.nplayers}")
for c in cs:
    c.alive = False
    c.send({"type": "goodbye"})
