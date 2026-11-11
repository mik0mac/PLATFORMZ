#!/usr/bin/env python3
"""The first frame of a match: nobody is already dead, and everybody faces in.

Two rules, both of which broke in playtesting without a single test noticing:

  NO CORPSES AT GO-LIVE. Every slot the server SHOWS (the `active` flag) is
  alive through the countdown and into play. A shown slot that is dead on the
  first PLAYING tick is a body the client explodes the moment the match begins.
  The case that produced one: a room with empty seats (maxBots under the roster),
  and somebody joining DURING the countdown. The bot set shifts down a slot, an
  empty seat becomes a bot seat - and it used to stay as dead as the empty seat
  had been.

  SPAWN FACING. A body starts out looking LEVEL at the centre of its own
  horizontal plane: yaw toward the arena's vertical axis, pitch zero. Not the
  absolute centre of the cube. That holds at a match start and - the part that
  was missing - for a player who joins a match already running and takes a
  bot's body over, who used to inherit wherever the bot was aiming.

    cd server && ./gameserver &
    python3 test/probe_matchstart.py
"""
import sys, os, time, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room, wait_until

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def corpses(c):
    """Slots the server is showing that are not alive."""
    return sorted(s for s, r in c.slots.items() if r["active"] and not r["alive"])

def off_centre_deg(row):
    """How far this body's yaw is from pointing at the arena's vertical axis."""
    x, _, z = row["pos"]
    d = row["yaw"] - math.atan2(-z, -x)
    return abs(math.degrees((d + math.pi) % (2 * math.pi) - math.pi))

def faces_in(row):
    # Half a degree: a u16 angle and a 21 cm position step are both far finer.
    return off_centre_deg(row) < 0.5 and abs(math.degrees(row["pitch"])) < 0.5

def watch(c, until, timeout=12.0):
    """Poll until `until()`, collecting every corpse seen on the way."""
    seen, deadline = set(), time.time() + timeout
    while time.time() < deadline and not until():
        seen.update((c.phase, s) for s in corpses(c))
        time.sleep(0.02)
    return seen

print("a room with empty seats")
host = C("HOST"); host.hello(); time.sleep(0.5)
room = host_room(host, "MATCH START")
check(bool(room), f"hosted a room (code={room})")
# Eight seats, five bots allowed: with one human that is two seats left EMPTY,
# which is what a join during the countdown then shuffles.
opts = dict(OPTS); opts["nplayers"] = 8; opts["maxbots"] = 5
host.send({"type": "options", **opts}); time.sleep(1.0)

print("someone joins during the countdown")
host.send({"type": "start", **opts})
check(wait_until(lambda: host.phase == "countdown", 5), f"countdown began (phase={host.phase})")
time.sleep(1.5)
late = C("LATE"); late.hello(); time.sleep(0.4)
check(join_room(late, room), "joined the room mid-countdown")
seen = watch(host, lambda: host.phase == "playing")
check(host.phase == "playing", f"the match went live (phase={host.phase})")
# The first PLAYING ticks, sampled as the client would see them.
for _ in range(15):
    seen.update((host.phase, s) for s in corpses(host)); time.sleep(0.02)
check(not seen, f"no shown slot was dead from countdown through go-live (saw {sorted(seen)})")
bots = [s for s, r in host.slots.items() if r["bot"]]
check(len(bots) == 5 and all(host.slots[s]["alive"] for s in bots),
      f"all five bots have a body ({ {s: host.slots[s]['alive'] for s in bots} })")

print("everyone starts out facing the centre of their own plane")
# Humans only: nobody here sends input, so their aim is still the spawn's. Bots
# have been turning to hunt since the first tick.
for c in (host, late):
    r = c.slots[c.slot]
    check(faces_in(r), f"{c.name}: {off_centre_deg(r):.2f} deg off in yaw, "
                       f"pitch {math.degrees(r['pitch']):.2f} deg")

print("a player joins the match already running")
time.sleep(3.0)                      # let the bots turn away from their spawn aim
bot_aims = [off_centre_deg(host.slots[s]) for s in bots if host.slots[s]["alive"]]
check(host.phase == "playing", f"still playing (phase={host.phase})")
check(any(a > 5.0 for a in bot_aims) or any(abs(math.degrees(host.slots[s]["pitch"])) > 5.0 for s in bots),
      f"the bots really have turned away, so inheriting their aim would show "
      f"(yaw offsets {[round(a) for a in bot_aims]})")
third = C("THIRD"); third.hello(); time.sleep(0.4)
check(join_room(third, room), "joined the running match")
time.sleep(0.3)
if third.slot is not None and third.slot in third.slots:
    r = third.slots[third.slot]
    check(r["alive"] and not r["bot"], "took a body over, alive and human")
    check(faces_in(r), f"and faces in: {off_centre_deg(r):.2f} deg off in yaw, "
                       f"pitch {math.degrees(r['pitch']):.2f} deg")
else:
    check(False, f"got a slot in the running match (slot={third.slot})")

for c in (host, late, third):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "a match starts clean and facing in")
sys.exit(1 if fails else 0)
