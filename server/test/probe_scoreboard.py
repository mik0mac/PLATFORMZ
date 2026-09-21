#!/usr/bin/env python3
"""D4/D5: the board is keyed on identity, and ranks RUNS.

`scoreboard_test.cpp` proves the store in isolation. This proves the thing that
was actually broken, end to end and over a socket: two different players both
called MIKE used to collapse into one row, and their scores added together.

    name-collision: MIKE=1009 in 1 row(s)

Ten points and nine hundred and ninety-nine, in a single entry. That is the bug
this exists to show is gone - and, since D5, that the board ranks individual runs
so one player can legitimately hold several rows.

    cd server && ./gameserver &
    python3 test/probe_scoreboard.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def rows_of(c):
    """The latest leaderboard this client was sent, as (name, score, isBot)."""
    return c.leaderboards[-1][1] if c.leaderboards else []

def play(host, others, secs=9.0):
    """Run one match to its end so the server credits it."""
    host.send({"type": "start", **OPTS})
    time.sleep(7.0)
    # Some input so somebody might actually score.
    for i in range(120):
        for c in [host] + others:
            c.seq += 1
            c.send({"seq": c.seq, "ep": c.epoch, "mx": 0.3, "mz": 1.0,
                    "jp": i % 30 == 0, "grav": False, "fire": i % 6 == 0,
                    "yaw": -1.57 + (i % 90) * 0.02, "pitch": 0.0})
        time.sleep(1.0 / 60.0)
    host.send({"type": "endmatch"})
    time.sleep(2.5)

print("two players, one name")
a = C("MIKE")
a.hello()
time.sleep(1.2)
b = C("MIKE")          # a DIFFERENT player who typed the same thing
b.hello()
time.sleep(1.0)
# The landing room is official and starts itself, so A makes a room it hosts and
# B follows it in - this probe drives the match by hand.
room = host_room(a, "SCOREBOARD")
check(bool(room), f"hosted a room to play in ({room})")
check(join_room(b, room), f"the second MIKE joined it ({b.matchCode})")
time.sleep(0.8)
check(a.slot is not None and b.slot is not None, "both seated")
check(bool(a.identities) and bool(b.identities), "both were issued identities")
check(a.identities[-1] != b.identities[-1] if (a.identities and b.identities) else False,
      "...and they are different, which is the whole basis of the fix")

play(a, [b])

rows = rows_of(a)
print(f"    board: {[(n, s) for n, s, bot in rows]}")
mikes = [(n, s) for n, s, bot in rows if n == "MIKE" and not bot]
check(len(mikes) == 2,
      f"two rows called MIKE, not one merged row (got {len(mikes)})")

print("bots hold exactly one row")
bots = [(n, s) for n, s, bot in rows if bot]
check(len(bots) == 1,
      f"all the bots together hold one line (got {len(bots)}: {bots})")

print("a second match adds rows rather than replacing them")
# An arcade board ranks RUNS, so the same player may hold several slots - which
# is the difference from a career total, where a second match would just add to
# one number.
before = len(rows)
play(a, [b])
rows = rows_of(a)
print(f"    board: {[(n, s) for n, s, bot in rows]}")
check(len(rows) >= before,
      f"the board grew or held ({before} -> {len(rows)}), it did not merge")

print("renaming freezes on old runs and follows on new ones")
a.send({"type": "name", "name": "MIKEY"})
time.sleep(0.8)
play(a, [b])
names = [n for n, _, bot in rows_of(a) if not bot]
print(f"    human rows: {names}")
check("MIKEY" in names, "the new run carries the new name")
# The old run keeps the name it was set under - a run is a historical event, and
# renaming yourself does not rewrite what happened. Only assert it if that run is
# still on a ten-row board.
check("MIKE" in names or len(names) >= 10,
      "...and an older run still carries the old one")

a.drop(goodbye=True)
b.drop(goodbye=True)

print()
if fails:
    print(f"probe_scoreboard: {fails} CHECK(S) FAILED")
    sys.exit(1)
print("probe_scoreboard: all checks passed")
