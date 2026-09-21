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
for i in range(20):                       # cap is 12; this must start refusing
    a.send({"type": "create", "n": f"ROOM{i}", "pre": "DEFAULT", "priv": False, "code": ""})
    time.sleep(0.12)
time.sleep(0.8)
check("server_full" in a.joinfails, "creation refused once at capacity")
# This probe runs with the per-address budget turned OFF, so every refusal here
# is the registry genuinely being out of rooms. Asserting the OTHER reason is
# absent is what keeps the two apart: they used to share one token, which is how
# "you already have three rooms" came to read as "SERVER IS AT CAPACITY" with a
# third of the registry free.
check("too_many_rooms" not in a.joinfails,
      f"...as capacity, not as a room budget that is switched off here: {set(a.joinfails)}")
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
p0 = a.matchlists[-1]
check(len(p0.get("m", [])) <= 8, f"page is capped at 8 rows (got {len(p0.get('m', []))})")
if p0.get("next", -1) > 0:
    a.send({"type": "list", "cur": p0["next"]})
    time.sleep(0.6)
    p1 = a.matchlists[-1]
    check(p1.get("cur") == p0["next"], "second page starts where the first ended")
    codes0 = {r["c"] for r in p0["m"]}
    codes1 = {r["c"] for r in p1["m"]}
    check(not (codes0 & codes1), "pages do not repeat a room")
else:
    check(False, "expected more than one page after filling the server")

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
