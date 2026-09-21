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

# constants.h - if this changes there, change it here.
PUBLIC_AUTOSTART_SECONDS = 10.0

# How many humans a room waits for is NOT a constant any more: it is per-preset
# (MatchOptions::minHumansToStart) and PRESET_TESTING_MODE in options.h drops
# every preset to 1 so a single tester can start a match. So this probe reads the
# threshold the room itself publishes in every state packet and adapts, rather
# than hard-coding a number that two different knobs can move.

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
# The welcome names the room and how it is run. Neither is inferable: quick match
# picks the room for you, and connecting with no room named lands you in one you
# never chose - so the code you think you asked for proves nothing.
check(h.matchCode == (h.created[-1] if h.created else None),
      f"welcome names the room we are in: {h.matchCode!r}")
check(h.matchKind == "custom", f"...and calls it custom: {h.matchKind!r}")
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
check(a.matchKind == "official", f"welcome calls it official: {a.matchKind!r}")
check(bool(a.matchCode), f"and names it: {a.matchCode!r}")

need = a.minHumans
print(f"    (this room starts itself at {need} human(s))")

# The lock, tested WITHOUT a clock. `options` is refused in an official room by
# the same optionsLocked gate that refuses START, and the map is the easiest
# refusal to see: ask for one the room is not on and watch nothing happen. This
# assertion holds however many humans the room wants, which the two below cannot.
wrong_map = "SMALL" if a.mapSize != "SMALL" else "XL"
was_map   = a.mapSize
a.send({"type": "options", **{**OPTS, "map": wrong_map}})
time.sleep(1.2)
check(a.mapSize == was_map,
      f"OPTIONS refused: the map is still {was_map} after asking for {wrong_map} (got {a.mapSize})")

if need > 1:
    # Only meaningful while a lone player is BELOW the threshold. At 1 the room
    # is entitled to arm and start underneath these, which is not a bug and not
    # something to assert against.
    check(a.countdown == 0.0, f"alone and below {need}, no countdown yet ({a.countdown})")
    a.send({"type": "start", **OPTS})
    time.sleep(7.0)   # the same wait that had the custom room PLAYING by now
    # Not "nothing happened" - the SAME message started the custom room above,
    # and alone in the room this player is the host by every rule except the lock.
    check(a.phase == "lobby", f"START ignored, still in the lobby (phase={a.phase})")
    a.send({"type": "endmatch"})
    time.sleep(1.0)
    check(a.phase == "lobby", f"ENDMATCH ignored too (phase={a.phase})")
else:
    check(a.countdown > 0.0,
          f"a lone player is already enough here, so it armed ({a.countdown:.1f}s)")
    print("    (skipping the START/ENDMATCH-ignored checks: they need a room that")
    print("     is NOT allowed to start yet, and this one starts at one player)")

print(f"...and starts itself once {need} player(s) are in it")
# Fill to the threshold. quick match takes the fullest joinable official room,
# which is the one A is sitting in, so each arrival lands beside them.
others = []
for i in range(need - 1):
    o = C(f"PLAYER{i + 2}")
    o.hello()
    time.sleep(1.0)
    o.send({"type": "quick"})
    time.sleep(1.2)
    others.append(o)
    check(o.slot is not None and o.slot != a.slot,
          f"player {i + 2} joined (slots {a.slot}, {o.slot})")
    check(o.matchCode == a.matchCode,
          f"...into the SAME room ({a.matchCode} / {o.matchCode})")

# The room must be able to SAY it is about to start. Without this there was a
# silent window where it had committed and nobody in it could tell.
first = a.countdown
check(0.0 < first <= PUBLIC_AUTOSTART_SECONDS + 0.5,
      f"the auto-start countdown is published to the room ({first:.1f}s)")
if a.phase == "lobby":
    time.sleep(3.0)
    check(a.countdown < first or a.phase != "lobby",
          f"and it counts DOWN ({first:.1f}s -> {a.countdown:.1f}s)")
for o in others:
    check(o.countdown > 0.0 or o.phase != "lobby",
          f"everyone in the room sees it ({o.name}={o.countdown:.1f}s)")

# Armed already, so wait out the countdown with slack for the tick.
deadline = time.time() + PUBLIC_AUTOSTART_SECONDS + 8.0
while time.time() < deadline and a.phase == "lobby":
    time.sleep(0.5)
check(a.phase in ("countdown", "playing"),
      f"the room started itself with nobody pressing START (phase={a.phase})")
for o in others:
    check(o.phase == a.phase, f"everyone is in it (A={a.phase}, {o.name}={o.phase})")

for c in [a] + others:
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "official/custom governance looks correct")
sys.exit(1 if fails else 0)
