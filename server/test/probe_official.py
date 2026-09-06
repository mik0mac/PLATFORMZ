#!/usr/bin/env python3
"""#107: OFFICIAL rooms are locked and self-starting; CUSTOM rooms are host-run.

The registry test proves the two flags follow the room's KIND. This proves what
those flags actually MEAN to a connected player, which is the part a unit test
cannot see: an official room refuses your START and then starts itself, and a
public custom room - the combination that could not be expressed before the split
- still takes its host's orders.

    cd server && ./gameserver &
    python3 test/probe_official.py

Takes ~25 s: PUBLIC_AUTOSTART_SECONDS is 10, and waiting it out is the test.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

# constants.h - if these change there, change them here.
PUBLIC_MIN_PLAYERS = 2
PUBLIC_AUTOSTART_SECONDS = 10.0

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

print("a public CUSTOM room still obeys its host")
# The whole point of the split. Public used to imply locked + auto-start, so this
# room could not exist: it is listed for anyone to find, AND its creator runs it.
h = C("HOST")
h.hello()
time.sleep(1.0)
h.send({"type": "create", "n": "OPEN HOUSE", "pre": "DEFAULT", "priv": False, "code": ""})
time.sleep(1.2)
check(len(h.created) == 1, f"created a public room: {h.created}")
check(h.phase == "lobby", f"and it waits in the lobby (phase={h.phase})")
h.send({"type": "start", **OPTS})
time.sleep(7.0)   # START -> COUNTDOWN -> PLAYING; long enough to clear the countdown
check(h.phase == "playing", f"the host's START is obeyed (phase={h.phase})")
h.send({"type": "endmatch"})
time.sleep(1.0)
check(h.phase == "gameover", f"and so is the host's ENDMATCH (phase={h.phase})")
h.alive = False
h.send({"type": "goodbye"})

print("an OFFICIAL room refuses a player's START")
a = C("SOLO")
a.hello()
time.sleep(1.0)
a.send({"type": "quick"})            # quick goes to official rooms only
time.sleep(1.2)
check(a.slot is not None, f"quick match put us in a room (slot={a.slot})")
check(a.phase == "lobby", f"which is in its lobby (phase={a.phase})")
a.send({"type": "start", **OPTS})
time.sleep(7.0)   # the same wait that had the custom room PLAYING by now
# Not "nothing happened" - the SAME message started the custom room above, and
# alone in the room this player is the host by every rule except the lock.
check(a.phase == "lobby", f"START ignored, still in the lobby (phase={a.phase})")
a.send({"type": "endmatch"})
time.sleep(1.0)
check(a.phase == "lobby", f"ENDMATCH ignored too (phase={a.phase})")

print(f"...and starts itself once {PUBLIC_MIN_PLAYERS} players are in it")
b = C("SECOND")
b.hello()
time.sleep(1.0)
b.send({"type": "quick"})            # fullest joinable official room = the one A is in
time.sleep(1.2)
check(b.slot is not None and b.slot != a.slot,
      f"second player joined the same room (slots {a.slot}, {b.slot})")
check(a.phase == "lobby", "the countdown has not fired early")

# Armed on arrival, so wait out the countdown with slack for the tick.
deadline = time.time() + PUBLIC_AUTOSTART_SECONDS + 8.0
while time.time() < deadline and a.phase == "lobby":
    time.sleep(0.5)
check(a.phase in ("countdown", "playing"),
      f"the room started itself with nobody pressing START (phase={a.phase})")
check(b.phase == a.phase, f"both players are in it (A={a.phase}, B={b.phase})")

for c in (a, b):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "official/custom governance looks correct")
sys.exit(1 if fails else 0)
