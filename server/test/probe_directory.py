#!/usr/bin/env python3
"""B1: the directory verbs - listing rooms and making them.

Covers listing, creating, paging and refusals. Actually moving between rooms is
exercised by probe_multimatch.py.

    cd server && ./gameserver &
    python3 test/probe_directory.py
"""
import sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

a = C("BROWSER")
a.hello()
time.sleep(1.0)
check(a.slot is not None, "joined the default room")

print("list")
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
check(len(a.matchlists) >= 1, "a match list came back")
lst = a.matchlists[-1] if a.matchlists else {}
rows = lst.get("m", [])
check(lst.get("total", 0) >= 1, f"at least the default room is listed (total={lst.get('total')})")
check(all(k in rows[0] for k in ("c", "n", "ph", "p", "max", "j")) if rows else False,
      "rows carry code/name/phase/players/max/joinable")
check(lst.get("next", -1) == -1, "one page is enough for one room (next=-1)")

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

print("create - private stays hidden")
before = lst.get("total", 0)
a.send({"type": "create", "n": "SECRET", "pre": "DEFAULT", "priv": True, "code": "abcd"})
time.sleep(0.8)
a.send({"type": "list", "cur": 0})
time.sleep(0.6)
lst = a.matchlists[-1]
check(lst.get("total", 0) == before, "a private room does NOT appear in the public list")

print("paging + capacity")
for i in range(20):                       # cap is 12; this must start refusing
    a.send({"type": "create", "n": f"ROOM{i}", "pre": "DEFAULT", "priv": False, "code": ""})
    time.sleep(0.12)
time.sleep(0.8)
check("server_full" in a.joinfails, "creation refused once at capacity")
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
