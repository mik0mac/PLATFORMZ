#!/usr/bin/env python3
"""E2: what happens when the server is out of room, and the budgets around it.

The headline claim is that fullness NEVER ends a connection. Before E2 a client
that arrived when every slot was taken got a "full" packet and had its socket
dropped, which over UDP is indistinguishable from an unreachable server - so the
client sat behind "MATCH IN PROGRESS - WAITING FOR A SLOT..." re-helloing into
nothing, forever. Being hung up on is the worst possible answer to "this room is
full", because the one thing you want next is the list of rooms that are not.

So: fill the default room, then prove the next client is still a working client -
it can list, it can join elsewhere, and it takes a seat by itself the moment one
frees up.

Then the budgets: rooms per address, and moves per second.

    cd server && ./gameserver &
    python3 test/probe_capacity.py
"""
import sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, take_any_room, handshake_landed

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def wait(t=0.8): time.sleep(t)

print("fill a room")
# One client first, to learn a room's code off its welcome. It has to ASK for a
# seat now - connecting lands you in the directory holding nothing (C6b) - so
# this is the probe's QUICK MATCH. Everyone after it names the room it landed in.
first = C("P0")
first.hello()
wait(1.2)
home = take_any_room(first)
check(first.slot is not None, f"first client seated in {home!r}")

crowd = [first]
for i in range(1, 12):                  # more than the 8 slots, on purpose
    c = C(f"P{i}", match=home)
    c.hello()
    crowd.append(c)
    time.sleep(0.15)
wait(1.5)

seated = [c for c in crowd if c.slot is not None and c.matchCode == home]
check(len(seated) == 8, f"exactly the room's 8 slots filled (got {len(seated)})")

# Everyone who did not get in must still be a live client. That is the whole
# point: before E2 these connections were hung up on.
spare = [c for c in crowd if c not in seated]
check(len(spare) >= 1, f"{len(spare)} clients could not be seated in {home}")
check(all(c.joinfails for c in spare),
      f"...and each was TOLD why: {[c.joinfails[-1] for c in spare]}")

print("a client with no seat is still a client")
odd = spare[0]
odd.matchlists.clear()
odd.send({"type": "list", "cur": 0})
wait(0.8)
check(len(odd.matchlists) == 1, "it can still ask for the match list")
rows = odd.matchlists[-1].get("m", []) if odd.matchlists else []
check(bool(rows), f"...and gets one: {[r['c'] for r in rows]}")

# Full rooms must be visible AS full, or "pick another" is not advice anybody
# can act on.
mine = [r for r in rows if r["c"] == home]
check(bool(mine) and mine[0]["p"] == mine[0]["max"],
      f"the full room reads {mine[0]['p']}/{mine[0]['max']}" if mine else "the room is listed")
check(bool(mine) and not mine[0]["j"], "...and is not advertised as joinable")

other = [r for r in rows if r["c"] != home and r["j"]]
if other:
    odd.joinfails.clear()
    odd.send({"type": "join", "m": other[0]["c"], "code": ""})
    wait(1.0)
    check(odd.matchCode == other[0]["c"] and odd.slot is not None,
          f"it can join a room that has space ({odd.matchCode}, slot {odd.slot})")
else:
    check(False, "no other joinable room to try")

print("a seat freeing up is taken by the next hello")
waiting = spare[1] if len(spare) > 1 else None
if waiting is None:
    check(False, "needed a second unseated client")
else:
    check(waiting.slot is None, "still unseated to begin with")
    # C6a: being parked is ANNOUNCED. Without this the client cannot tell a
    # connection that got through and found no seat from one whose handshake
    # never landed - a welcome is the only other proof it is connected, and that
    # cannot exist without a seat.
    check(waiting.unseated > 0,
          f"...and was told so, not left to infer it ({waiting.unseated} acks)")
    check(bool(waiting.leaderboards),
          "...and still got the leaderboard, which is not a property of a room")
    seated[0].drop(goodbye=True)         # frees a slot in `home` immediately
    wait(0.4)
    waiting.hello()                      # the real client does this every 0.5s
    wait(1.2)
    check(waiting.slot is not None and waiting.matchCode == home,
          f"it seated itself in {home} (slot {waiting.slot}) without being asked twice")

for c in crowd:
    if c.alive: c.drop(goodbye=True)
wait(0.5)

print("rooms per address")
maker = C("MAKER")
maker.hello()
wait(1.2)
check(handshake_landed(maker), "the room-maker is through")
# Creating does not need a seat first - a roomless client is exactly who
# makes a room, and `create` puts them in the one they just made.
# The first one carries a deliberately hostile name, because there is nowhere
# better to test that and it costs nothing. Sent as RAW BYTES: the server parses
# JSON by string search, so a control character has to actually be in the
# datagram - json.dumps would escape it into the harmless letters \\ and u.
DIRTY = b"ROOM\x07\x01NAME" + b"X" * 60
maker.s.send(b'{"type":"create","n":"' + DIRTY +
             b'","pre":"DEFAULT","priv":false,"code":""}')
time.sleep(0.3)
for i in range(1, 5):
    maker.send({"type": "create", "n": f"MINE{i}", "pre": "DEFAULT",
                "priv": False, "code": ""})
    time.sleep(0.3)
wait(0.8)
# Three tokens in the bucket, refilling at one per two minutes, so a burst of
# five makes exactly three rooms.
check(len(maker.created) == 3,
      f"3 rooms minted from one address, then refused: {maker.created}")
check(maker.joinfails.count("server_full") == 2,
      f"the extra attempts were refused: {maker.joinfails}")

print("match names are sanitised like player names")
maker.matchlists.clear()
maker.send({"type": "list", "cur": 0})
wait(0.8)
listed = maker.matchlists[-1].get("m", []) if maker.matchlists else []
names = [r["n"] for r in listed]
check(all(all(32 <= ord(ch) <= 126 for ch in n) for n in names),
      f"no control characters survived into the list: {names}")
# PLAYER_NAME_MAX_CHARS is 32 - the SAME cap player names get, which is the whole
# point: a room name is rendered in everybody's browser, so it is not allowed to
# be unbounded just because it arrived through a different verb.
check(all(len(n) <= 32 for n in names), f"...and every name is within 32 chars: {names}")
hostile = [n for n in names if n.startswith("ROOM")]
check(bool(hostile) and hostile[0] == "ROOMNAME" + "X" * 24,
      f"the hostile name was stripped and truncated, not rejected: {hostile}")

print("moves per second")
mover = C("MOVER")
mover.hello()
wait(1.2)
mover.send({"type": "list", "cur": 0})
wait(0.8)
rooms = [r["c"] for r in (mover.matchlists[-1].get("m", []) if mover.matchlists else [])
         if r["j"]]
check(len(rooms) >= 2, f"at least two joinable rooms to bounce between: {rooms}")
if len(rooms) >= 2:
    mover.joinfails.clear()
    for i in range(12):                  # all REAL codes, so only the move budget bites
        mover.send({"type": "join", "m": rooms[i % len(rooms)], "code": ""})
        time.sleep(0.03)
    wait(1.0)
    limited = mover.joinfails.count("rate_limited")
    check(limited >= 5, f"a burst of 12 real joins was throttled ({limited} refused)")
    check("notfound" not in mover.joinfails and "badcode" not in mover.joinfails,
          "and none were charged as bad codes - these were all real rooms")

maker.drop(goodbye=True)
mover.drop(goodbye=True)

print()
if fails:
    print(f"probe_capacity: {fails} CHECK(S) FAILED")
    sys.exit(1)
print("probe_capacity: all checks passed")
