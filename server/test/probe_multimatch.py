#!/usr/bin/env python3
"""A3: two players, two rooms, one server - and they must not see each other.

This is the point of the whole exercise. Everything before it was foundations
that changed nothing a player could observe.

    cd server && ./gameserver &
    python3 test/probe_multimatch.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def wait(secs=1.2): time.sleep(secs)

print("both clients land in the default room")
a, b = C("ALPHA"), C("BRAVO")
a.hello(); wait(0.4); b.hello(); wait()
check(a.slot is not None and b.slot is not None, f"joined (slots {a.slot}, {b.slot})")
check(a.slot != b.slot, "different slots in the same room")

print("ALPHA makes a room - and is put in it")
# PRIVATE on purpose - but only so the next check can prove a private room stays
# out of the public list. Governance no longer follows visibility: a room a player
# creates is CUSTOM either way, so ALPHA hosts it and its START works regardless
# (probe_official covers that). Boot's resident OFFICIAL room is the locked,
# self-starting one.
a.send({"type": "create", "n": "ALPHA HOUSE", "pre": "DEFAULT", "priv": True, "code": "letmein"})
wait(1.5)
check(len(a.created) == 1, f"server returned the new room's code: {a.created}")
newRoom = a.created[-1] if a.created else None
check(a.slot == 0, f"ALPHA was placed in it (slot={a.slot})")
check(not a.joinfails, f"no refusal ({a.joinfails})")

print("the private room is hidden from everyone else")
b.send({"type": "list", "cur": 0})
wait()
listed = [r.get("n") for r in (b.matchlists[-1].get("m", []) if b.matchlists else [])]
check("ALPHA HOUSE" not in listed, f"BRAVO cannot see it: {listed}")

print("they are now in different rooms")
a.send({"type": "start", **OPTS})     # only ALPHA's room starts
wait(7.0)
check(a.phase == "playing", f"ALPHA is playing (phase={a.phase})")
check(b.phase == "lobby",  f"BRAVO is still in the lobby (phase={b.phase})")
check(a.epoch != b.epoch or b.epoch == 0, "separate match epochs")

print("BRAVO's room is unaffected by ALPHA's match ending")
a.send({"type": "endmatch"})
wait(1.5)
check(a.phase == "gameover", f"ALPHA's match ended (phase={a.phase})")
check(b.phase == "lobby",    f"BRAVO untouched (phase={b.phase})")

print("leaving comes home")
a.send({"type": "leave"})
wait(1.5)
check(a.phase in ("lobby", "gameover"), f"ALPHA is back in the default room (phase={a.phase})")

print("refusals are specific")
n = len(a.joinfails)
a.send({"type": "join", "m": "ZZZZ", "code": ""})
wait(0.8)
check(len(a.joinfails) > n and a.joinfails[-1] == "notfound",
      f"unknown room -> notfound ({a.joinfails[-1:]})")

print("an invite-only room is opened by its own code, and nothing else")
# The creator supplies no password - the client cannot, it does not learn the
# room's code until after `create` returns - so the server uses the room's OWN
# code. Without that the room was gated by the EMPTY string: the code you were
# handed was refused as badcode, and the one thing that DID open the room was
# sending no password at all.
b.created.clear()
b.send({"type": "create", "n": "SECRET", "pre": "DEFAULT", "priv": True, "code": ""})
wait(1.5)
secret = b.created[-1] if b.created else ""
check(bool(secret), f"created an invite-only room: {secret}")
# The creator has to end up IN it. They send no password, so the server has to
# use the room's effective one - handing theirs back refused them entry to the
# room it had just built for them, and CREATE looked like it did nothing.
check(b.matchCode == secret, f"the creator is in their own room ({b.matchCode})")

n = len(a.joinfails)
a.send({"type": "join", "m": secret, "code": ""})
wait(0.8)
check(len(a.joinfails) > n, f"an empty password is refused ({a.joinfails[-1:]})")

n = len(a.joinfails)
a.send({"type": "join", "m": secret, "code": "wrong"})
wait(0.8)
check(len(a.joinfails) > n, f"a wrong one is refused ({a.joinfails[-1:]})")

a.send({"type": "join", "m": secret, "code": secret})
wait(1.5)
check(a.matchCode == secret, f"the code itself opens it (in {a.matchCode})")

for c in (a, b):
    c.alive = False
    c.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "multi-match routing works")
sys.exit(1 if fails else 0)
