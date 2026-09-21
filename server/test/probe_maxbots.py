#!/usr/bin/env python3
"""A room whose preset caps maxBots leaves the extra slots genuinely EMPTY.

Before MatchOptions::maxBots, every unclaimed roster slot became a bot, so an
empty slot could not exist. Now a preset can decline to paper the room over, and
the slots past the cap have no body at all: nothing to shoot, nothing driving
them, not counted for last-man-standing - but still in the roster and still
joinable.

Drives a CUSTOM room the probe hosts, so the host presses START and nothing
auto-starts underneath the assertions. The cap is set in the START bundle rather
than inherited from a preset: maxBots is a rule like any other, a host client
sends the whole bundle, and pinning the test to whatever MAYHEM happens to be
tuned to today would make preset tuning break this probe.

The "still playing" check below is the regression test for the one thing this
feature could genuinely break: match-end counts PARTICIPANTS, not roster slots.
Counting the empty slots as bodies made a solo match satisfy "only one player
left standing" on its first PLAYING tick and end instantly.

    cd server && ./gameserver &
    python3 test/probe_maxbots.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

print("a humans-only room")
host = C("HOST")
host.hello()
time.sleep(0.5)
room = host_room(host, "NO BOTS")
check(bool(room), f"hosted a room (code={room})")
check(host.matchKind == "custom", f"player-made rooms are always custom (got {host.matchKind})")

# maxBots 0 in the lobby, so the roster preview shows open seats rather than a
# room that looks full of bots before anyone arrives.
opts = dict(OPTS); opts["nplayers"] = 4; opts["maxbots"] = 0
host.send({"type": "options", **opts})
time.sleep(1.2)
check(host.maxBots == 0, f"the room echoes the cap back (maxBots={host.maxBots})")

print("the lobby fields no bots at all")
botNames = [r["name"] for r in host.slots.values() if r["bot"]]
check(not botNames, f"no slot is bot-driven (got {botNames})")

print("the match starts, and keeps running with one human and no bots")
host.send({"type": "start", **opts})
time.sleep(8.0)
check(host.phase == "playing", f"still playing, not ended on the first tick (phase={host.phase})")
check(host.nplayers == 4, f"roster is still the preset's 4 slots (got {host.nplayers})")

alive = [r["name"] for r in host.slots.values() if r["alive"]]
check(len(alive) == 1, f"exactly one body in the arena - the human (got {alive})")
inactive = [s for s, r in host.slots.items() if not r["active"]]
check(len(inactive) == 3, f"the other three slots are empty, not hidden (got {sorted(inactive)})")

print("an empty slot is still joinable mid-match")
late = C("LATECOMER")
late.hello()
time.sleep(0.4)
join_room(late, room)
time.sleep(1.2)
check(late.slot is not None, f"got a slot in the PLAYING room (slot={late.slot})")
check(late.slot != host.slot, "and not the host's slot")
me = late.players.get("LATECOMER")
check(me is not None, f"appears in the roster: {sorted(late.players)}")
if me:
    check(me["alive"], "arrived with a body, not into an empty slot's corpse")
    check(not me["bot"], "flagged human")
check(late.nplayers == 4, f"roster did not grow to seat them (got {late.nplayers})")

for c in (host, late):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "maxBots leaves real empty slots")
sys.exit(1 if fails else 0)
