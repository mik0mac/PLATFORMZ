#!/usr/bin/env python3
"""A room whose last player walks out DURING the countdown comes back.

The bug: the wind-down clock that returns a finished room to its lobby was
stamped on the PLAYING -> GAMEOVER edge. A match can reach GAMEOVER on the very
first tick it is live - the count expires, and the end condition ("no humans
left alive") is already true because whoever was in the room left while it was
counting. The previous phase is then COUNTDOWN, not PLAYING, so the edge was
never seen: gameOverStamped stayed false, so nothing ever freed the world, so
the room sat in GAMEOVER for the rest of the process's life. The directory
showed it as ENDING forever and no one could play in it again.

So this probe is not really about leaving. It is about a match that begins and
ends inside one tick, which is the only way to reach that edge from anywhere but
PLAYING - and leaving during the count is how a player produces one.

What it canNOT check is the client half of the same race (main.cpp): a LEAVE
that crosses the room's `countdown` packet used to throw the leaver onto the
countdown screen for a room it no longer held, where no further packet was ever
coming and no key or timeout could get out. That is UI, not wire.

    cd server && ./gameserver &
    python3 test/probe_leaverace.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room, wait_until

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1


def room_phase(client, code):
    """The directory's opinion of one room, which is the thing that got stuck.

    Asked through the LIST rather than a state packet on purpose: once the room
    is empty there is nobody left in it to be sent one, and "what does everyone
    else see" is the actual symptom.
    """
    client.matchlists.clear()
    client.send({"type": "list", "cur": 0})
    deadline = time.time() + 4.0
    while time.time() < deadline:
        time.sleep(0.1)
        if client.matchlists:
            for r in client.matchlists[-1].get("m", []):
                if r.get("c") == code:
                    return r
            return {}
    return {}


eye = C("OBSERVER")            # never takes a seat; just reads the directory
eye.hello()
host = C("QUITTER")
host.hello()
time.sleep(1.0)

code = host_room(host, "LEAVE RACE")
check(bool(code), f"hosted a room to walk out of ({code!r})")

print("start it, then leave while it is still counting")
host.send(dict({"type": "start"}, **OPTS))
check(wait_until(lambda: host.phase == "countdown", timeout=5.0),
      f"the room is counting down (phase {host.phase!r})")
# Out, mid-count. The room now holds nobody at all, so when the count expires
# the match goes live and ends on the same tick.
host.send({"type": "leave"})
check(wait_until(lambda: host.slot is None, timeout=5.0),
      "we are out of it (unseated, no slot)")

print("the match runs its count out and ends, with nobody in it")
# COUNTDOWN_SECONDS is 5; one list a second is E1's budget, so this is ~8 asks.
check(wait_until(lambda: room_phase(eye, code).get("ph") == "gameover", timeout=12.0),
      "the empty match reached GAMEOVER rather than counting forever")

print("...and then winds down, which is the regression")
# GAMEOVER_LOBBY_SECONDS is 10. Before the fix this never happened at all: the
# room stayed in GAMEOVER until the process died. The generous timeout is so a
# slow (or sanitized) build reads as slow rather than as this bug coming back.
# Keep the row the poll succeeded on: E1 allows one list a second, so asking a
# second time for the same answer just gets dropped and reads as a missing room.
row = {}
def back_in_lobby():
    global row
    r = room_phase(eye, code)
    if r.get("ph") == "lobby":
        row = r
        return True
    return False
check(wait_until(back_in_lobby, timeout=25.0),
      "the room returned to its lobby instead of ending permanently")
check(row.get("j") is True, f"and is joinable again (j={row.get('j')!r})")

print("it is a real lobby, not just a label: somebody can play in it")
nxt = C("NEWCOMER")
nxt.hello()
time.sleep(0.8)
check(join_room(nxt, code), "a fresh client got a seat in the recovered room")
nxt.send(dict({"type": "start"}, **OPTS))
check(wait_until(lambda: nxt.phase in ("countdown", "playing"), timeout=6.0),
      f"and the room starts a second match (phase {nxt.phase!r})")

for c in (eye, host, nxt):
    c.drop(goodbye=True)

print("FAIL" if fails else "PASS", f"({fails} failures)")
sys.exit(1 if fails else 0)
