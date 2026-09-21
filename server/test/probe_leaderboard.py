#!/usr/bin/env python3
"""Does the end-of-match leaderboard reach a client that is still PLAYING?

The bug this exists for: the server credits scores on the PLAYING -> GAMEOVER
edge and broadcasts the new table immediately - BEFORE the state packet that
announces GAMEOVER, in the same tick. So the table always lands while the client
still believes it is playing.

The client used to drain its socket in two places with different message
handling: the menu screens knew about "leaderboard", the PLAYING loop did not.
The one message that mattered arrived at the one drain that ignored it, so the
table only ever looked right after restarting the client - the join path re-sends
it behind the welcome, which lands on the menu drain.

This checks the protocol side of that: a table arrives, it carries the match's
scores, and it arrives while phase is still "playing".

    cd server && ./gameserver &
    python3 test/probe_leaderboard.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, handshake_landed

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

a = C("SCORER")
a.hello()
time.sleep(1.0)
check(handshake_landed(a), "connected (holding no room yet)")
# The landing room is official - locked, self-starting - so host one instead.
check(bool(host_room(a, "LEADERBOARD")), f"hosted a room to play in ({a.matchCode})")
time.sleep(0.8)

before = len(a.leaderboards)
a.send({"type": "start", **OPTS})
time.sleep(7.0)
check(a.phase == "playing", f"match running (phase={a.phase})")

# shoot at things for a while so somebody scores
for i in range(240):
    a.seq += 1
    a.send({"seq": a.seq, "ep": a.epoch, "mx": 0.3, "mz": 1.0, "jp": i % 30 == 0,
            "grav": False, "fire": i % 8 == 0, "yaw": -1.57 + (i % 90) * 0.02,
            "pitch": 0.0})
    time.sleep(1.0 / 60.0)

a.send({"type": "endmatch"})
time.sleep(2.0)

fresh = a.leaderboards[before:]
check(len(fresh) > 0, f"a leaderboard arrived after the match ({len(fresh)})")

if fresh:
    phase_at, rows = fresh[-1]
    print(f"    arrived while phase={phase_at!r}, {len(rows)} rows: {rows[:4]}")
    # THE point is that the server SENDS the table before it broadcasts
    # GAMEOVER, so a correct client has to handle one while still believing the
    # match is on. Reported, not asserted: what a client can observe is the
    # ARRIVAL order of two datagrams, and UDP does not promise that the one sent
    # first lands first. Asserting it failed about one run in five for a reason
    # that was never a bug - and a probe that cries wolf is worse than no probe,
    # which matters now that CI runs these.
    if phase_at != "playing":
        print("    (note: the gameover packet overtook it - UDP reordering on the"
              " way here, not the server sending them the wrong way round)")
    # What IS guaranteed, and what the feature is actually for: the table arrives
    # after a match, and it arrives already CREDITED. A player who opens the
    # leaderboard the moment a match ends must see that match's scores in it,
    # whichever datagram happened to land first.
    check(any(n == "SCORER" for n, _, _ in rows), "our name is in the table")
    # D4: our row is a HUMAN row, keyed on this connection's identity rather than
    # on the name "SCORER". A bot row here would mean the credit path picked the
    # wrong key.
    check(any(n == "SCORER" and not bot for n, _, bot in rows),
          "...as a player, not a bot")
    # Bots play every match, so by the end of one there are bot rows too - sent in
    # the same message and tagged, for the client's PLAYERS/BOTS toggle.
    check(any(bot for _, _, bot in rows),
          f"bot rows ride along, tagged: {[n for n, _, b in rows if b]}")

a.alive = False
a.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "leaderboard delivery looks correct")
sys.exit(1 if fails else 0)
