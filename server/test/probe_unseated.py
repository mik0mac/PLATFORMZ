#!/usr/bin/env python3
"""C6: you hold no room until you choose one, and holding none is a real state.

Connecting used to put you in a room the server picked. It now puts you in the
directory holding nothing: you browse, you choose, and only then do you have a
slot. LEAVE returns you to that state, and a refused join leaves you there.

Every other probe exercises this incidentally - they all have to ask for a room
now. This one is about the state itself, and in particular about the two ways it
could silently stop working:

  1. A BARE HELLO MUST NOT SEAT YOU. The real client re-sends hello while it has
     no slot, so if the server answers one by seating you, parking is undone
     half a second after it happens and nothing anywhere reports a problem. That
     was the actual bug in the first cut of #150, and it survived a full probe
     run because every probe asked for a room explicitly.
  2. A REFUSAL MUST NOT HAND YOU A DIFFERENT ROOM. The whole point is not being
     put somewhere you did not choose; a consolation seat is that bug wearing a
     friendlier face.

What this probe canNOT check is the CLIENT's keepalive gate (#151): a parked
client sends no input, and UDP_CLIENT_TIMEOUT_LOBBY is 3s, so the native client
has to heartbeat while holding no room or the server drops it. That lives in
main.cpp, not on the wire. It is verified by hand against the real client - see
the C6c commit. What IS checked here is the server half: a parked connection
that behaves like a live client stays live, and can still act afterwards.

    cd server && ./gameserver &
    python3 test/probe_unseated.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, host_room, join_room

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

print("connecting lands you nowhere")
a = C("WANDERER")
a.hello()
time.sleep(1.2)
check(a.unseated > 0, f"the server said so, rather than leaving us to guess ({a.unseated})")
check(a.slot is None, f"no slot (got {a.slot})")
check(not a.matchCode, f"no room (got {a.matchCode!r})")

print("...and STAYS nowhere, however many times we say hello")
# The regression guard for #150. A real client re-hellos every 0.5s while it
# holds no slot; if the server answers that by seating us, parking is undone the
# moment it happens. Four of them, spaced like the real retry.
before = a.unseated
for _ in range(4):
    a.hello()
    time.sleep(0.5)
check(a.slot is None, f"still no slot after four hellos (got {a.slot})")
check(not a.matchCode, f"still no room (got {a.matchCode!r})")
check(a.unseated > before, f"each bare hello is re-acked, not re-seated ({before} -> {a.unseated})")

print("a client with no room is a working client")
a.matchlists.clear()
a.send({"type": "list", "cur": 0})
time.sleep(1.0)
rows = a.matchlists[-1].get("m", []) if a.matchlists else []
check(bool(a.matchlists), "it can ask for the match list")
check(len(rows) >= 1, f"...and gets one: {[r.get('n') for r in rows]}")
check(bool(a.leaderboards), "it was sent the leaderboard too - that is not a property of a room")

print("a refusal leaves you where you were: nowhere")
# No fallback seat. This is the assertion that would catch a "put them somewhere
# rather than nowhere" line creeping back in.
n = len(a.joinfails)
a.send({"type": "join", "m": "ZZZZ", "code": ""})
time.sleep(1.2)
check(len(a.joinfails) > n and a.joinfails[-1] == "notfound",
      f"an unknown room is refused by name: {a.joinfails[-3:]}")
check(a.slot is None and not a.matchCode,
      f"...and we were NOT given some other room (slot={a.slot}, room={a.matchCode!r})")

print("choosing a room is how you get one")
room = host_room(a, "CHOSEN")
check(bool(room), f"created and seated in it ({room})")
check(a.slot is not None, f"now holding a slot ({a.slot})")

print("leaving puts you back")
was = a.unseated
a.send({"type": "leave"})
time.sleep(1.5)
check(a.unseated > was, f"told we are unseated again ({was} -> {a.unseated})")
check(a.slot is None and not a.matchCode,
      f"...and holding nothing (slot={a.slot}, room={a.matchCode!r})")

print("and you can choose again afterwards")
# A round trip proves leave did not leave the connection in a half state - the
# thing that would show up as "the browser works but JOIN does nothing".
b = C("HOST")
b.hello()
time.sleep(0.6)
shared = host_room(b, "SECOND ROOM")
check(bool(shared), f"a second client hosts a room ({shared})")
check(join_room(a, shared), f"the wanderer joins it ({a.matchCode})")
check(a.slot is not None, f"seated again ({a.slot})")

print("a parked connection stays live while it behaves like one")
# The server half of #151. UDP_CLIENT_TIMEOUT_LOBBY is 3s, so a parked
# connection that keeps up its heartbeat must survive well past that and still
# be able to act - the reaper must not treat "holds no room" as "is not here".
c = C("LOITERER")
c.hello()
time.sleep(1.0)
check(c.slot is None and c.unseated > 0, "parked to begin with")
time.sleep(6.0)                      # twice the lobby timeout, pinging throughout
c.matchlists.clear()
c.send({"type": "list", "cur": 0})
time.sleep(1.0)
check(bool(c.matchlists), "still answered after twice the idle timeout")
check(join_room(c, shared), f"and can still join a room ({c.matchCode})")

for cl in (a, b, c):
    cl.alive = False
    cl.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "holding no room is a real, working state")
sys.exit(1 if fails else 0)
