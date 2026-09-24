#!/usr/bin/env python3
"""B1: the directory verbs - listing rooms and making them.

Covers listing, creating, paging and refusals. Actually moving between rooms is
exercised by probe_multimatch.py.

    cd server && ./gameserver &
    python3 test/probe_directory.py
"""
import sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, handshake_landed

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

a = C("BROWSER")
a.hello()
time.sleep(1.0)
# Connecting no longer puts you anywhere (C6b) - the directory is the point of
# this probe, and you now browse it from exactly that state.
check(handshake_landed(a), "connected")
check(a.slot is None, "...holding no room, which is what the browser is for")

print("list")
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
check(len(a.matchlists) >= 1, "a match list came back")
lst = a.matchlists[-1] if a.matchlists else {}
rows = lst.get("m", [])
check(lst.get("total", 0) >= 1, f"the official rooms are listed (total={lst.get('total')})")
check(all(k in rows[0] for k in ("c", "n", "ph", "p", "max", "j", "k", "map")) if rows else False,
      "rows carry code/name/phase/players/max/joinable/kind/map")
check(rows[0].get("map") in ("SMALL", "MEDIUM", "LARGE", "XL") if rows else False,
      f"...and the map is a real preset name: {rows[0].get('map') if rows else None}")
check(lst.get("next", -1) == -1, "the boot rooms fit one page (next=-1)")

print("official rooms are resident")
official = [r for r in rows if r.get("k") == "official"]
check(len(official) >= 1, f"an official room exists at boot: {[r['n'] for r in official]}")
check(all(r.get("j") for r in official), "and it is joinable, so the browser is never empty")

print("create - public")
before = lst.get("total", 0)
a.send({"type": "create", "n": "PUBLIC ROOM", "pre": "DEFAULT", "priv": False, "code": ""})
time.sleep(0.8)
check(len(a.created) == 1, f"create returns the room's code: {a.created}")
# `create` replies with the code and puts you in the room; it does NOT return a
# list any more, so ask for one.
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
lst = a.matchlists[-1]
check(lst.get("total", 0) == before + 1, f"list grew to {lst.get('total')}")
names = [r.get("n") for r in lst.get("m", [])]
check("PUBLIC ROOM" in names, f"the new room is listed: {names}")
# The whole point of #107. Before the split, "public" derived optionsLocked and
# autoStart, so a public room was ALWAYS hostless - a public host-run room could
# not be expressed at all.
mine = [r for r in lst.get("m", []) if r.get("n") == "PUBLIC ROOM"]
check(bool(mine) and mine[0].get("k") == "custom",
      f"a player-created PUBLIC room is custom, not official: {mine}")

print("create - private stays hidden")
before = lst.get("total", 0)
a.send({"type": "create", "n": "SECRET", "pre": "DEFAULT", "priv": True, "code": "abcd"})
time.sleep(0.8)
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
lst = a.matchlists[-1]
check(lst.get("total", 0) == before, "a private room does NOT appear in the public list")

print("quick match goes to the FIRST official preset, not the lowest room code")
# The tiebreak. Every official room is empty at boot, so this is decided entirely
# by the preset ramp in options.h - typical gameplay first, niche last - and
# DEFAULT is the front door.
#
# This used to assert on official[0], the first row of a listing SORTED BY CODE.
# With one preset that was the same room by definition; with five it agreed only
# when DEFAULT happened to draw the lowest of five random codes, so it was a
# 1-in-5 flake that would have read as "quick match is broken".
defaults = [r for r in official if r.get("pre") == "DEFAULT"]
check(bool(defaults), f"an official DEFAULT room exists: {[r.get('pre') for r in official]}")
offCode = defaults[0]["c"] if defaults else ""
lowest  = min((r["c"] for r in official), default="")
b = C("QUICKER")
b.hello()
time.sleep(1.0)
b.send({"type": "quick"})
time.sleep(0.9)
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
row = [r for r in a.matchlists[-1].get("m", []) if r.get("c") == offCode]
# Not "some room got a player" - the resident OFFICIAL room specifically, while a
# joinable public custom room ("PUBLIC ROOM") was also sitting there to be picked.
check(bool(row) and row[0].get("p", 0) >= 1,
      f"quick landed in the official DEFAULT room {offCode}: {row}")
# Worth saying out loud when the codes happen to line up, because on those runs
# the check above would also pass under the OLD sort-by-code behaviour and so
# proves nothing about the ramp.
if offCode == lowest:
    print(f"    (note: DEFAULT also drew the lowest code this run, {lowest} -"
          f" the ramp is not distinguished from code order here)")
b.alive = False
b.send({"type": "goodbye"})
time.sleep(0.3)

print("paging + capacity")
# FILL UNTIL REFUSED, rather than a fixed number of attempts. This used to send
# exactly 20 creates with the comment "cap is 12; this must start refusing",
# which quietly made MATCH_MAX_CONCURRENT untouchable: raise it past 20 and the
# probe fails on a server that is behaving perfectly, reporting a capacity bug
# where there is only a stale constant in a test. The cap is meant to go up when
# the box it runs on does, so nothing here may assume its value.
made = 0
for i in range(400):
    a.send({"type": "create", "n": f"ROOM{i}", "pre": "DEFAULT", "priv": False, "code": ""})
    time.sleep(0.05)
    if "server_full" in a.joinfails: break
    made += 1
time.sleep(0.8)
check("server_full" in a.joinfails,
      f"creation refused once at capacity (after {made} rooms)")
# This probe runs with the per-address budget turned OFF, so every refusal here
# is the registry genuinely being out of rooms. Asserting the OTHER reason is
# absent is what keeps the two apart: they used to share one token, which is how
# "you already have three rooms" came to read as "SERVER IS AT CAPACITY" with a
# third of the registry free.
check("too_many_rooms" not in a.joinfails,
      f"...as capacity, not as a room budget that is switched off here: {set(a.joinfails)}")

# WALK EVERY PAGE, however many there are. This is the assembly the real client
# performs to show one scrollable list, so it has to hold at whatever size the
# registry is - not merely for the two pages a 12-room cap happens to produce.
pages, seen, dupes, cur = 0, [], set(), 0
total = None
while True:
    a.matchlists.clear()
    a.send({"type": "list", "cur": cur})
    deadline = time.time() + 4.0
    while time.time() < deadline and not a.matchlists: time.sleep(0.1)
    if not a.matchlists:
        check(False, f"page at cur={cur} never came back")
        break
    p = a.matchlists[-1]
    pages += 1
    if total is None: total = p.get("total", -1)
    rows = p.get("m", [])
    check(len(rows) <= 8, f"page {pages} is capped at 8 rows (got {len(rows)})") if pages == 1 else None
    for r in rows:
        if r["c"] in seen: dupes.add(r["c"])
        seen.append(r["c"])
    if p.get("next", -1) < 0: break
    check(p["next"] == cur + len(rows), "a page starts where the last one ended")
    cur = p["next"]
    time.sleep(1.1)          # E1 allows a burst then one list a second

check(pages > 1, f"filling the server took more than one page ({pages})")
check(not dupes, f"no room appears on two pages: {sorted(dupes)}")
check(len(seen) == total,
      f"every room was reachable by paging: walked {len(seen)}, server says {total}")

print("refusals are answered, not left to time out")
n = len(a.joinfails)
a.send({"type": "join", "m": "ZZZZ", "code": ""})
time.sleep(0.5)
check(len(a.joinfails) > n, "join to an unknown room gets a refusal")

a.alive = False
a.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "directory verbs look correct")
sys.exit(1 if fails else 0)
