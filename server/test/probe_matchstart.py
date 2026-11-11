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
# The joiner takes the LOWEST free slot, which is the lowest bot. Wait until that
# bot has plainly turned away from its spawn facing, or a joiner who inherited
# its aim would pass by luck.
target = min(bots)
def turned_away():
    r = host.slots.get(target, {})
    return bool(r) and r["alive"] and (off_centre_deg(r) > 25.0 or abs(math.degrees(r["pitch"])) > 5.0)
check(wait_until(turned_away, 20, step=0.02),
      f"the bot in slot {target} has turned away from the centre "
      f"({off_centre_deg(host.slots[target]):.0f} deg yaw, {math.degrees(host.slots[target]['pitch']):.0f} deg pitch)")
check(host.phase == "playing", f"still playing (phase={host.phase})")
third = C("THIRD"); third.hello(); time.sleep(0.4)
third.send({"type": "join", "m": room, "code": ""})
# Read the body the moment it is ours. It INHERITS the bot's velocity, so it is
# moving, and the direction to the centre moves with it: the facing was exact at
# the tick of the takeover and drifts after. Hence the first packet, and a yaw
# tolerance that a moving body can meet but an inherited aim (25+ deg, above)
# cannot. Pitch has no such excuse - nothing changes it but input, and this
# client sends none - so it is held to level.
def seated():
    r = third.slots.get(third.slot) if third.slot is not None else None
    return third.matchCode == room and bool(r) and not r["bot"]
check(wait_until(seated, 5, step=0.01), f"joined the running match (slot={third.slot})")
if seated():
    r = third.slots[third.slot]
    check(third.slot == target, f"took the bot's body over (slot {third.slot})")
    check(r["alive"], "alive")
    check(off_centre_deg(r) < 10.0 and abs(math.degrees(r["pitch"])) < 0.5,
          f"and faces in, level: {off_centre_deg(r):.2f} deg off in yaw, "
          f"pitch {math.degrees(r['pitch']):.2f} deg")

for c in (host, late, third):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "a match starts clean and facing in")
sys.exit(1 if fails else 0)
