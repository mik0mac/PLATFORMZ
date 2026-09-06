#!/usr/bin/env python3
"""A5: dropping into a match that is already being played.

A match browser is only worth having if PLAYING rooms are joinable. This checks a
latecomer can take over a bot, and - the part that is easy to get wrong - that
they arrive as a viable player rather than inheriting a half-dead bot's body,
score and colour.

    cd server && ./gameserver &
    python3 test/probe_joinprogress.py
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

# Host opens an 8-slot match alone, so slots 1-7 fill with bots and there is
# somewhere for a latecomer to land.
# TWO humans, because with one the match ends the moment they die (aliveHumans
# == 0) and there is nothing left to join. A buddy keeps it alive long enough for
# the bots to hurt each other, which is what makes the inheritance checks mean
# something.
host = C("HOST")
host.hello()
time.sleep(0.4)
buddy = C("BUDDY")
buddy.hello()
time.sleep(1.0)
# friendlyFire off and a passive host: with one human, the host dying ends the
# match (aliveHumans == 0) and there is nothing left to join. The bots fight
# each other regardless, which is what damages them.
opts = dict(OPTS); opts["nplayers"] = 8; opts["ff"] = False
host.send({"type": "start", **opts})
time.sleep(7.5)
check(host.phase == "playing", f"match running (phase={host.phase})")
check(host.nplayers == 8, f"roster is 8 with bots filling it (got {host.nplayers})")

# Play a while so the bots take damage and score - the whole point is that a
# joiner inherits neither.
for i in range(420):   # long enough for the bots to hurt each other
    for c in (host, buddy):
        c.seq += 1
        c.send({"seq": c.seq, "ep": c.epoch, "mx": 0.0, "mz": 0.0, "jp": i % 20 == 0,
                "grav": False, "fire": False, "yaw": -1.57, "pitch": 0.0})
    time.sleep(1.0 / 60.0)
check(host.phase == "playing", f"still playing before the join (phase={host.phase})")

# Which bot the joiner will displace, and how hurt it is. If nothing got damaged
# the health assertion below proves nothing, so say so rather than reporting a
# green tick that means "100 == 100".
hurt = {n: p["hp"] for n, p in host.players.items() if p["bot"] and p["hp"] < 100}
print(f"  bots below full health before the join: {hurt or 'none'}")
if not hurt:
    print("  NOTE: no bot was damaged, so the health check below is vacuous this run")

print("a latecomer joins the live match")
late = C("LATECOMER")
late.hello()
# Sample the score as early as we can see ourselves, and again later. Inheriting
# the bot's points and EARNING points from the bot's still-in-flight rockets look
# identical if you only look once.
firstScore = None
for _ in range(60):
    time.sleep(0.05)
    if "LATECOMER" in late.players:
        firstScore = late.players["LATECOMER"]["score"]
        break
time.sleep(1.2)
check(late.slot is not None, f"got a slot in a PLAYING match (slot={late.slot})")
check(late.slot != host.slot, "and not the host's slot")
check(late.phase == "playing", f"dropped straight into play (phase={late.phase})")

print("what they inherited")
time.sleep(0.6)
me = late.players.get("LATECOMER")
check(me is not None, f"the joiner appears in the roster: {sorted(late.players)}")
if me:
    check(me["alive"], "alive, not a corpse someone abandoned")
    check(not me["bot"], "flagged human, so it renders as a player not a bot")
    if hurt:
        check(me["hp"] == 100, f"full health, not a damaged bot's remaining HP (hurt: {hurt})")
    else:
        check(me["hp"] == 100, "full health (weak: no bot was damaged to inherit from)")
    print(f"    score at first sighting: {firstScore}, a second later: {me['score']}")
    check(firstScore == 0,
          f"arrived on zero - did NOT inherit the bot's points (first sighting: {firstScore})")

for c in (host, buddy, late):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "join-in-progress works")
sys.exit(1 if fails else 0)
