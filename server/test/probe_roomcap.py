#!/usr/bin/env python3
"""A room's capacity is its own `numPlayers`, in the LOBBY and not just at START.

`numPlayers` is documented as the room's roster size - humans + bots + empty
slots - and it is the denominator the browser advertises. But nothing applied it
until a match actually started: a freshly created room spawned a full eight slots
whatever its preset said, so a four-player MAYHEM room seated SEVEN humans and
listed itself as 7/8. This probe is that bug's regression test.

Three things, and the third is the one with teeth:

  the cap is real      a 4-player room takes 4 humans, refuses the 5th, and the
                       directory says 4/4 and not-joinable
  the cap is the room's a 6-player room says x/6, never x/8
  it never evicts      a host who drags the size slider BELOW the people already
                       seated shrinks the room only as far as they allow - the
                       same clamp match start has always applied. Nobody loses a
                       slot they are sitting in.

Custom rooms the probe hosts, so nothing auto-starts underneath the assertions.

    cd server && ./gameserver &
    python3 test/probe_roomcap.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, wait_until, handshake_landed, host_room, join_room

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1


def listing(client, code, timeout=12.0):
    """The directory's row for `code`, as a (players, max, joinable) triple.

    RE-ASKS. `list` is rate-limited to a burst and then one a second, and an
    over-budget request is dropped WITHOUT A REPLY - so a single ask right after
    four clients have joined can go unanswered and look exactly like a room that
    fell out of the directory. Everything on this machine shares one address and
    therefore one bucket, which is why this bites in the suite and not in a run
    on its own.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        client.matchlists.clear()
        client.send({"type": "list", "cur": 0})
        for _ in range(12):                  # ~1.8s for this ask to be answered
            time.sleep(0.15)
            for ml in client.matchlists:
                for r in ml.get("m", []):
                    if r.get("c") == code:
                        return r.get("p"), r.get("max"), r.get("j")
        time.sleep(1.0)                      # let the bucket refill, then re-ask
    return None, None, None


def seated(client):
    """A client that holds a slot in the room it asked for."""
    return client.slot is not None and bool(client.matchCode)


print("a room advertises its own cap, not the build's eight slots")
host = C("HOST")
host.hello()
wait_until(lambda: handshake_landed(host), 8.0)
# MAYHEM is the 4-player preset. Named rather than dialed: the point is that the
# cap arrives with the room, before anybody has sent an options bundle.
room = host_room(host, "SMALL ROOM", preset="MAYHEM")
check(bool(room), f"hosted a MAYHEM room (code={room})")
time.sleep(1.0)
p, mx, j = listing(host, room)
check(mx == 4, f"the directory calls it a 4-slot room (got max={mx})")
check(p == 1 and j is True, f"with one player in it and joinable (got {p}/{mx}, j={j})")

print("it fills at four humans and refuses the fifth")
guests = []
for i in range(4):
    g = C(f"G{i}")
    g.hello()
    wait_until(lambda: handshake_landed(g), 8.0)
    join_room(g, room, timeout=4.0)
    guests.append(g)
time.sleep(1.5)
took = [g for g in guests if g.matchCode == room]
check(len(took) == 3, f"three more got in, filling the four slots (got {len(took)})")
turned_away = [g for g in guests if not seated(g)]
check(len(turned_away) == 1, f"the fifth human is refused (turned away: {len(turned_away)})")
check(turned_away and turned_away[0].joinfails,
      f"...with a reason, not silence (got {turned_away[0].joinfails if turned_away else []})")
p, mx, j = listing(host, room)
check((p, mx, j) == (4, 4, False), f"the row reads 4/4 and not joinable (got {p}/{mx}, j={j})")

print("a host may shrink the room, but never out from under anyone")
# Four humans are seated. Ask for two.
host.send({"type": "options", **{**OPTS, "nplayers": 2}})
time.sleep(1.5)
p, mx, j = listing(host, room)
check(mx == 4, f"the roster holds at the four people in it (got max={mx})")
still_in = [c for c in [host] + took if c.matchCode == room and c.slot is not None]
check(len(still_in) == 4, f"nobody was evicted (still seated: {len(still_in)})")

print("...and may grow it again")
host.send({"type": "options", **{**OPTS, "nplayers": 8}})
time.sleep(1.5)
p, mx, j = listing(host, room)
check((mx, j) == (8, True), f"raised to eight and joinable again (got {p}/{mx}, j={j})")

for c in [host] + guests:
    c.alive = False
    c.send({"type": "goodbye"})

print()
if fails:
    print(f"probe_roomcap: {fails} check(s) FAILED")
    sys.exit(1)
print("a room is exactly as big as its rules say")
