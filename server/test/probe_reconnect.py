#!/usr/bin/env python3
"""D2: reconnect into your own slot.

A mid-match leaver's body is held open for MID_MATCH_LEAVE_GRACE_SEC, keeping its
position, health and score. Until now nothing could prove "I am that player", so
a dropped player came back as a new slot while their old body drifted off and got
eliminated. The client's install id (D1) is what ties them back together.

What this checks, in order of how easy each is to get wrong:
  1. the same id gets the SAME slot back, with its score and health intact
     (routing a reconnect through TakeOverSlot would hand it back wiped)
  2. a DIFFERENT id does not get that slot, even though it is sitting vacant
  3. no id at all does not get it either - an empty string must not match the
     empty slotOwner of an untouched slot
  4. once that body is gone - the grace expired, or it was shot while nobody was
     flying it - the same id gets a fresh body rather than resuming a corpse

    cd server && ./gameserver &
    python3 test/probe_reconnect.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

def body(c, slot, timeout=3.0):
    """The body in `slot` as `c` currently sees it, waiting for a readable view.

    By SLOT, never by name. A player who dropped and the client that came back
    are both called "HOST", and C.players is keyed by name - so the returning
    client silently overwrote the very body under test, and the probe read the
    wrong row while reporting a server bug.

    The poll is for a second reason: C.slots is rebuilt from scratch on every
    state packet, and a truncated or chunked frame leaves it empty. Sampling one
    instant is a coin toss.
    """
    end = time.time() + timeout
    while time.time() < end:
        e = c.slots.get(slot)
        if e: return e
        time.sleep(0.1)
    return {}

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

# The server frees a quiet UDP slot after UDP_CLIENT_TIMEOUT (10 s mid-match) and
# then holds the body for MID_MATCH_LEAVE_GRACE_SEC (15 s). So a reconnect has to
# land in that second window: after the reap, before the eviction.
REAP_SEC  = 10.0
GRACE_SEC = 15.0

CID_A = "aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa"
CID_B = "bbbbbbbb-2222-4222-8222-bbbbbbbbbbbb"

print("setting up a live match")
# Two humans: with one, the match ends the moment they drop (aliveHumans == 0)
# and there is no match left to reconnect INTO.
host  = C("HOST", cid=CID_A)
host.hello()
time.sleep(0.4)
buddy = C("BUDDY", cid=CID_B)
buddy.hello()
time.sleep(1.0)

# The held body must survive the whole grace window unattended for any of this to
# be testable, and the defaults shred it: the first version came back at 4 hp and
# then died outright, which turned "did the stranger take the held slot?" into a
# coin toss. So: a big empty arena, no asteroids, and bots too dim to aim. None
# of that changes the code path under test - it stops the scenery from killing
# the subject mid-experiment.
opts = dict(OPTS)
opts["nplayers"] = 8
opts["ff"]      = False     # bots do not shoot the drifting body
opts["diff"]    = 0.0       # ...and are not trying very hard anyway
opts["roid"]    = 0         # nothing to collide with
opts["half"]    = 240.0     # room to drift without hitting a wall
opts["plat"]    = 32
host.send({"type": "start", **opts})
time.sleep(7.5)
check(host.phase == "playing", f"match running (phase={host.phase})")
room = host.matchCode
check(bool(room), f"room code known ({room})")

host_slot    = host.slot
before       = body(buddy, host_slot) if host_slot is not None else {}
before_hp    = before.get("hp")
before_score = before.get("score")
check(host_slot is not None, f"host seated (slot {host_slot})")
check(before_hp is not None, f"host body visible to others (hp={before_hp})")

# ---------------------------------------------------------------------------
# Everything below happens inside ONE grace window. Sequencing the impostors and
# the rightful owner against the same held body is both faster and a stronger
# claim than testing them separately: the slot is demonstrably still there when
# each impostor is turned away, and still there for its owner afterwards.
#
# It also avoids the trap the first version fell into. Each check used its own
# 12 s window, the match ended part-way through the run, and at GAMEOVER a vacant
# slot gets botified and RENAMED - so the roster lookup stopped finding "HOST"
# and the failure looked like a reconnect bug rather than a stale scenario.
print("\nhost drops mid-match; the body is held open")
host.drop()
time.sleep(REAP_SEC + 2.0)      # past the UDP reap, well inside the 15 s grace

check(buddy.phase == "playing", f"match still running (phase={buddy.phase})")
held = body(buddy, host_slot)
check(held.get("alive") is True, "the dropped body is still alive, held open")
check(held.get("bot") is False,  "...and was not handed to a bot")

print("\nimpostors are turned away while the body is held")
stranger = C("STRANGER", cid="cccccccc-3333-4333-8333-cccccccccccc", match=room)
stranger.hello()
time.sleep(1.5)
check(stranger.slot is not None, "a different id was seated somewhere")
check(stranger.slot != host_slot,
      f"...but NOT into the held slot ({stranger.slot} != {host_slot})")

anon = C("ANON", match=room)     # no cid at all
anon.hello()
time.sleep(1.5)
check(anon.slot is not None, "a client with no id was seated somewhere")
check(anon.slot != host_slot,
      f"...an empty id did not match the empty slotOwner ({anon.slot} != {host_slot})")

print("\nthe rightful owner gets it back, body and score intact")
back = C("HOST", cid=CID_A, match=room)
back.hello()
time.sleep(1.5)
check(back.slot == host_slot,
      f"same id resumed the same slot ({back.slot} == {host_slot})")
check(back.matchCode == room, f"...in the same room ({back.matchCode})")

after = body(buddy, host_slot)
check(after.get("score") == before_score,
      f"score survived the reconnect ({after.get('score')} == {before_score})")
# The invariant is NOT "hp is unchanged" - the sim keeps running while the body
# drifts, so it can still take damage. It is "the body was never handed back
# fresh": TakeOverSlot resets health to PLAYER_STARTING_HEALTH, so a resumed body
# must never have GAINED health, and must never sit at full after being hurt.
check(after.get("hp", 0) <= before_hp,
      f"resumed body was not healed ({after.get('hp')} <= {before_hp})")
check(not (before_hp < 100 and after.get("hp") == 100),
      "resumed body was not reset to full health (that would be TakeOverSlot)")

# ---------------------------------------------------------------------------
print("\nonce the body is gone, there is nothing left to resume")
back.drop()
# Wait for the body to go, rather than sleeping a computed interval. Two things
# can end it and the contract is the same for both - HeldSlotFor requires the
# body to be BOTH inside its grace AND alive:
#   the grace expiring (~25 s: reap + grace, though tick slop puts it nearer 26,
#     which is why a fixed 27 s sleep sat on the edge and failed half the time)
#   or simply being shot while nobody is flying it, which unattended bodies
#     usually are, and which gets there sooner.
# Whichever happens, what must follow is a fresh body rather than a resumed one.
t0 = time.time()
while time.time() - t0 < REAP_SEC + GRACE_SEC + 15.0:
    if buddy.slots.get(host_slot, {}).get("alive") is False: break
    time.sleep(0.5)
print(f"   body evicted after {time.time() - t0:.1f}s")
expired = body(buddy, host_slot)
check(expired.get("alive") is False,
      "the abandoned body is gone (grace expired, or shot while unattended)")

late = C("HOST", cid=CID_A, match=room)
late.hello()
time.sleep(2.0)
check(late.slot is not None, "late returner was still seated")
# NOT "a different slot number". Getting slot 0 back is correct - it is the
# lowest free slot and nobody else wants it. What must differ is the BODY: an
# expired grace has to go through TakeOverSlot, which revives it at full health
# with a zeroed score, rather than resuming a corpse.
fresh = body(buddy, late.slot)
check(fresh.get("alive") is True,  "...revived, not seated into the corpse")
check(fresh.get("hp") == 100,      f"...with a fresh body (hp={fresh.get('hp')}, not resumed)")
check(fresh.get("score") == 0,     f"...and a zeroed score ({fresh.get('score')})")

for c in (host, buddy, stranger, anon, back, late):
    try: c.drop()
    except Exception: pass

print("\n" + ("FAILURES" if fails else "all reconnect checks passed"))
sys.exit(1 if fails else 0)
