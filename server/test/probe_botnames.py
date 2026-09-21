#!/usr/bin/env python3
"""#102: bot names are shuffled per match, and per room.

Three things, which are the three ways this can be wrong:

  1. two matches in a row must not field the same lineup in the same slots
  2. names must hold STEADY within a match - refreshBotSlots runs sixty times a
     second, so a re-roll in the wrong place renames every bot every tick
  3. two rooms running at once must not share an order - a single global shuffle
     would look random from one room and identical from two

    cd server && ./gameserver &
    python3 test/probe_botnames.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def lineup(c):
    """slot -> name, for the bot slots only, off the last state packet."""
    return {slot: row["name"] for slot, row in sorted(c.slots.items()) if row["bot"]}

def run_match(host, secs=8.0):
    host.send({"type": "start", **OPTS})
    time.sleep(secs)

def end_match(host):
    host.send({"type": "endmatch"})
    time.sleep(2.0)

print("a lineup exists at all")
a = C("WATCHER")
a.hello()
time.sleep(1.0)
# The landing room is official and starts itself, which would re-roll the lineup
# mid-assertion. Host a room so this probe owns when matches begin and end.
check(bool(host_room(a, "BOT NAMES")), f"hosted a room to watch ({a.matchCode})")
time.sleep(1.0)
check(a.slot is not None, "seated")
lobby = lineup(a)
check(len(lobby) >= 2, f"the lobby previews bots: {lobby}")

print("names hold steady WITHIN a match")
run_match(a)
check(a.phase == "playing", f"match running (phase={a.phase})")
first = lineup(a)
samples = [first]
for _ in range(6):
    time.sleep(0.5)
    samples.append(lineup(a))
check(all(s == first for s in samples),
      f"the lineup did not change across 3s of play: {first}")

# The lobby preview has to be the lineup you actually get, or it is not a
# preview. Re-rolling at match START rather than on the way back to the lobby is
# exactly what breaks this.
check(all(lobby.get(k) == v for k, v in first.items() if k in lobby),
      f"...and it is the one the lobby advertised: lobby={lobby} match={first}")

print("a second match gets a different lineup")
end_match(a)
# The re-roll happens when the room drops its world and returns to LOBBY, which
# is GAMEOVER_LOBBY_SECONDS after the match ends - so wait for the phase rather
# than guessing at the clock.
for _ in range(60):
    time.sleep(0.5)
    if a.phase == "lobby": break
check(a.phase == "lobby", f"the room returned to its lobby (phase={a.phase})")

run_match(a)
second = lineup(a)
check(a.phase == "playing", "second match running")
print(f"    match 1: {first}")
print(f"    match 2: {second}")
# Shuffles can legitimately repeat, so this is a weak check by nature - with 9
# names the odds of an identical slot-for-slot lineup are low but not zero. It is
# still worth having: the failure it catches is "never shuffles at all", which
# would repeat every single time.
check(first != second,
      "the two matches differ (a 1-in-many false failure is possible; re-run)")
end_match(a)

print("two rooms do not share an order")
# A single global shuffle looks random from one room and identical from two,
# which is the bug a per-match order is here to avoid.
b = C("ROOM_B")
b.hello()
time.sleep(1.2)
b.send({"type": "create", "n": "SECOND", "pre": "DEFAULT", "priv": False, "code": ""})
time.sleep(1.5)
check(b.matchCode and b.matchCode != a.matchCode,
      f"a second room exists ({b.matchCode} vs {a.matchCode})")

c = C("ROOM_C")
c.hello()
time.sleep(1.2)
c.send({"type": "create", "n": "THIRD", "pre": "DEFAULT", "priv": False, "code": ""})
time.sleep(1.5)

lb, lc = lineup(b), lineup(c)
print(f"    room {b.matchCode}: {lb}")
print(f"    room {c.matchCode}: {lc}")
check(bool(lb) and bool(lc), "both rooms preview bots")
check(lb != lc,
      "two rooms field different lineups (same caveat: a repeat is possible)")

for x in (a, b, c):
    x.drop(goodbye=True)

print()
if fails:
    print(f"probe_botnames: {fails} CHECK(S) FAILED")
    sys.exit(1)
print("probe_botnames: all checks passed")
