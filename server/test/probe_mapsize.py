#!/usr/bin/env python3
"""B3: the map lives in MatchOptions, so the lobby shows it BEFORE start.

It used to be whichever of four START buttons you pressed - it existed only at
the instant of starting, so nobody waiting in the room could see which arena they
were about to play. Now it rides the options bundle like every other rule.

Two separate claims, and this checks both:

  advertised  the pending choice reaches EVERY client in the lobby, live, with no
              match running (state packet, option flags bits 16/32)
  built       starting actually generates that arena (welcome halfSize)

    cd server && ./gameserver &
    python3 test/probe_mapsize.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

# constants.h mapSizePresets - halfSize per preset.
HALF = {"SMALL": 90.0, "MEDIUM": 120.0, "LARGE": 240.0, "XL": 360.0}

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

host = C("HOST"); host.hello(); time.sleep(1.0)
peer = C("PEER"); peer.hello(); time.sleep(1.2)
check(host.slot is not None and peer.slot is not None,
      f"two clients in the default room (slots {host.slot}, {peer.slot})")

print("the host's choice reaches the lobby, before anything starts")
for want in ("SMALL", "LARGE", "XL", "MEDIUM"):
    o = dict(OPTS); o["map"] = want
    host.send({"type": "options", **o})
    time.sleep(0.9)
    # The point of the issue: visible to someone who is NOT the host and has not
    # pressed anything, while the room is still sitting in its lobby.
    check(peer.mapSize == want, f"PEER sees {want} (got {peer.mapSize})")
    check(peer.phase == "lobby", f"...with no match running (phase={peer.phase})")

print("and starting builds that arena, not some other one")
for want in ("SMALL", "XL"):
    o = dict(OPTS); o["map"] = want; o["nplayers"] = 2
    host.send({"type": "options", **o}); time.sleep(0.6)
    host.send({"type": "start", **o})
    time.sleep(8.0)
    check(host.phase == "playing", f"{want}: match running (phase={host.phase})")
    # The welcome re-sent at start carries the generated world's boundary.
    check(abs(peer.half - HALF[want]) < 0.5,
          f"{want}: world halfSize is {HALF[want]} (got {peer.half})")
    host.send({"type": "endmatch"}); time.sleep(1.5)

for c in (host, peer):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "map size travels with the options")
sys.exit(1 if fails else 0)
