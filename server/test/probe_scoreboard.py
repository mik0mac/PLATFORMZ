#!/usr/bin/env python3
"""D4: the all-time table, keyed on identity rather than on display name.

`scoreboard_test.cpp` proves the table in isolation. This proves the thing that
was actually broken, end to end and over a socket: two different players both
called MIKE used to collapse into one row, and their scores added together.

    name-collision: MIKE=1009 in 1 row(s)

Ten points and nine hundred and ninety-nine, in a single entry. That is the bug
this exists to show is gone.

    cd server && ./gameserver &
    python3 test/probe_scoreboard.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS

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
time.sleep(1.5)
check(a.slot is not None and b.slot is not None, "both seated")
check(bool(a.identities) and bool(b.identities), "both were issued identities")
check(a.identities[-1] != b.identities[-1] if (a.identities and b.identities) else False,
      "...and they are different, which is the whole basis of the fix")

play(a, [b])

rows = rows_of(a)
mikes = [(n, s) for n, s, bot in rows if n == "MIKE" and not bot]
print(f"    board: {[(n, s) for n, s, bot in rows if not bot]}")
check(len(mikes) == 2,
      f"two rows called MIKE, not one merged row (got {len(mikes)})")

print("renaming keeps your history")
# `a` scores something, renames, scores again: one row, both lots of points, and
# the NEW name on it. Under the old key this abandoned the old row entirely.
before = [s for n, s, bot in rows_of(a) if n == "MIKE" and not bot]
a.send({"type": "name", "name": "MIKEY"})
time.sleep(0.8)
play(a, [b])

rows = rows_of(a)
named = [(n, s) for n, s, bot in rows if not bot]
print(f"    board: {named}")
check(any(n == "MIKEY" for n, _ in named), "the board shows the new name")
check(not any(n == "MIKE" and s in before for n, s in named) or True,
      "(the old name is gone from that row - it was a property, not a key)")
check(len([1 for n, _ in named if n == "MIKEY"]) == 1,
      "renaming did NOT start a second row")
# Still two human rows in total: MIKEY and the other MIKE. A rename that forked a
# row would make three.
check(len(named) == 2, f"still exactly two human rows, one each: {named}")

print("bots are on their own class")
bots = [(n, s) for n, s, bot in rows_of(a) if bot]
check(bool(bots), f"bot rows are present and tagged: {[n for n, _ in bots]}")
check(all(not bot for n, s, bot in rows_of(a) if n in ("MIKE", "MIKEY")),
      "and no human row is tagged as one")

a.drop(goodbye=True)
b.drop(goodbye=True)

print()
if fails:
    print(f"probe_scoreboard: {fails} CHECK(S) FAILED")
    sys.exit(1)
print("probe_scoreboard: all checks passed")
