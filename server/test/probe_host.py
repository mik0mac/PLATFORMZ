#!/usr/bin/env python3
"""#108: the host is the room's CREATOR and stays put, migrating only on leaving.

The old rule recomputed "lowest connected non-bot slot" every time it was asked,
so host followed slot order rather than intent. In LOBBY that is nearly invisible
- slots compact every tick, so the lowest slot IS the longest-present player - and
it looks harmless right up until a match is underway, where compaction stops:

    host (slot 0) leaves mid-match -> slot 1 inherits the room
    a latecomer takes over the vacated bot in slot 0
    ...and under the old rule instantly becomes host, able to END everyone's match

That is the case this probe drives. It also checks the plain one: the creator can
start their own room and a joiner cannot.

    cd server && ./gameserver &
    python3 test/probe_host.py

Takes ~40 s: it has to outlast MID_MATCH_LEAVE_GRACE_SEC (15 s) for the leaver's
body to become a bot the latecomer can claim.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

MID_MATCH_LEAVE_GRACE_SEC = 15.0   # constants.h

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def hostnames(c):
    return sorted(n for n, p in c.players.items() if p.get("host"))

print("the creator hosts the room they made")
a = C("ALPHA")
a.hello(); time.sleep(1.0)
a.send({"type": "create", "n": "ALPHA HOUSE", "pre": "DEFAULT", "priv": False, "code": ""})
time.sleep(1.5)
code = a.created[-1] if a.created else ""
check(bool(code), f"room created: {code}")

b = C("BRAVO")
b.hello(); time.sleep(0.8)
b.send({"type": "join", "m": code, "code": ""})
time.sleep(1.5)
check(b.slot is not None and b.slot != a.slot, f"BRAVO joined it (slots {a.slot}, {b.slot})")
check(hostnames(a) == ["ALPHA"], f"exactly one host, and it is the creator: {hostnames(a)}")

print("a joiner cannot start it")
b.send({"type": "start", **OPTS})
time.sleep(3.0)
check(a.phase == "lobby", f"BRAVO's START ignored (phase={a.phase})")

c = C("CHARLIE")
c.hello(); time.sleep(0.8)
c.send({"type": "join", "m": code, "code": ""})
time.sleep(1.5)
check(c.slot is not None, f"CHARLIE joined too (slot={c.slot})")

print("the creator can")
a.send({"type": "start", **OPTS})
time.sleep(8.0)
check(a.phase == "playing", f"ALPHA's START is obeyed (phase={a.phase})")
bravoSlot, charlieSlot = b.slot, c.slot

print("the host leaves mid-match")
a.alive = False
a.send({"type": "goodbye"})
# Outlast the leave grace so slot 0's body is botified and claimable.
time.sleep(MID_MATCH_LEAVE_GRACE_SEC + 5.0)
check(b.phase == "playing", f"the match carries on without them (phase={b.phase})")
check(hostnames(b) == ["BRAVO"], f"host migrated to the lowest remaining slot: {hostnames(b)}")

print("a latecomer takes the vacated slot 0")
d = C("DELTA")
d.hello(); time.sleep(0.8)
d.send({"type": "join", "m": code, "code": ""})
time.sleep(2.0)
check(d.slot is not None, f"DELTA got in (slot={d.slot})")
# The point of the whole issue: DELTA now holds a LOWER slot than the host.
check(d.slot is not None and bravoSlot is not None and d.slot < bravoSlot,
      f"...into a slot below the host's (DELTA={d.slot}, BRAVO={bravoSlot})")
check(hostnames(b) == ["BRAVO"], f"host did NOT jump to them: {hostnames(b)}")

print("and cannot end everyone's match")
d.send({"type": "endmatch"})
time.sleep(2.0)
check(b.phase == "playing", f"DELTA's ENDMATCH ignored (phase={b.phase})")
b.send({"type": "endmatch"})
time.sleep(2.0)
check(b.phase == "gameover", f"the real host's ENDMATCH works (phase={b.phase})")

for x in (b, c, d):
    x.alive = False
    x.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "host ownership looks correct")
sys.exit(1 if fails else 0)
