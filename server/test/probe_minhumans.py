#!/usr/bin/env python3
"""An official room arms its auto-start on ITS OWN threshold, not a constant.

PUBLIC_MIN_PLAYERS used to be the answer for every room. It is now only the
default for MatchOptions::minHumansToStart, which a preset may raise - THE VOID
asks for 3, because an XL arena with no walls is a dull place for two people.

The threshold is read off the wire rather than hard-coded, for two reasons: a
preset can be retuned at any time, and PRESET_TESTING_MODE in options.h drops
every preset to 1 so a single tester can start a match. Either would break a
probe that assumed a number. What is actually under test is the RELATIONSHIP -
one short does not arm, meeting it does - and that holds at any threshold.

    cd server && ./gameserver &
    python3 test/probe_minhumans.py

Takes ~30 s when the threshold is above 1: the point is waiting out a countdown
that must not fire.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C

VOID_LABEL = "THE VOID"
PUBLIC_AUTOSTART_SECONDS = 10.0   # constants.h

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

print(f"finding the official {VOID_LABEL} room")
scout = C("SCOUT")
scout.hello()
time.sleep(0.6)
scout.send({"type": "list", "cur": 0})
time.sleep(1.2)
rows = scout.matchlists[-1].get("m", []) if scout.matchlists else []
void = next((r for r in rows if r.get("n") == VOID_LABEL), None)
check(void is not None, f"{VOID_LABEL} is listed: {[r.get('n') for r in rows]}")
scout.alive = False
scout.send({"type": "goodbye"})
if not void:
    print()
    print("FAILURES")
    sys.exit(1)
code = void.get("c")

# One player, so we can read what the room asks for before testing against it.
first = C("FIRST")
first.hello()
time.sleep(0.5)
first.send({"type": "join", "m": code, "code": ""})
time.sleep(1.5)
check(first.matchCode == code, f"joined it (in {first.matchCode})")
check(first.matchKind == "official", f"and it is official (got {first.matchKind!r})")

need = first.minHumans
check(need >= 1, f"the room publishes a threshold it means ({need})")
print(f"    (this room starts itself at {need} human(s))")

joined = [first]

if need > 1:
    print(f"one short of its {need} does not arm it")
    # Fill to need-1 and prove nothing happens, which is the assertion with
    # teeth: a server still reading the compile-time constant would arm here.
    for i in range(need - 2):
        c = C(f"PLAYER{i + 2}")
        c.hello()
        time.sleep(0.5)
        c.send({"type": "join", "m": code, "code": ""})
        time.sleep(1.2)
        joined.append(c)
    check(all(c.matchCode == code for c in joined),
          f"{len(joined)} player(s) in the room, one short of {need}")

    deadline = time.time() + PUBLIC_AUTOSTART_SECONDS + 5.0
    while time.time() < deadline:
        time.sleep(0.5)
        if first.phase != "lobby": break
    check(first.phase == "lobby", f"did NOT start below the threshold (phase={first.phase})")
    check(first.countdown == 0.0, f"and never even armed (countdown={first.countdown})")
else:
    print("    (skipping the below-threshold checks: at 1 there is no 'one short')")

print(f"meeting {need} arms it")
if need > 1:
    last = C(f"PLAYER{need}")
    last.hello()
    time.sleep(0.5)
    last.send({"type": "join", "m": code, "code": ""})
    time.sleep(1.5)
    joined.append(last)
    check(last.matchCode == code, f"the {need}th player joined (in {last.matchCode})")

armed = first.countdown
check(armed > 0.0 or first.phase != "lobby",
      f"the countdown is armed and published ({armed:.1f}s, phase={first.phase})")

deadline = time.time() + PUBLIC_AUTOSTART_SECONDS + 8.0
while time.time() < deadline and first.phase == "lobby":
    time.sleep(0.5)
check(first.phase in ("countdown", "playing"),
      f"the room started itself once its threshold was met (phase={first.phase})")

for c in joined:
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "auto-start follows the room's own threshold")
sys.exit(1 if fails else 0)
