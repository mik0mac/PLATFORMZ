#!/usr/bin/env python3
"""E1: the UDP handshake cookie, and the abuse budgets behind it.

Three separate claims, each of which would be invisible from the game:

  1. An unknown endpoint gets a cookie and NOTHING else. This is the
     anti-spoofing part: a forged source address never receives the cookie (it
     goes to the address that was forged), so it can never reach the code that
     hands out a slot and a ~3 KB welcome.
  2. The cookie is bound to the address it was minted for. If it weren't, an
     attacker would ask for one from their own address and then spend it on a
     thousand spoofed ones.
  3. The budgets: one match list per second per connection, five bad join codes
     per minute.

Deliberately talks raw sockets rather than probe.C for the handshake parts -
C answers challenges automatically, which is exactly the behaviour under test.

    cd server && ./gameserver &
    python3 test/probe_cookie.py
"""
import sys, os, time, json, socket
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe
from probe import C, enc

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def raw():
    """A bare UDP socket to the server - no auto-challenge-answering."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((probe.HOST, probe.PORT))
    s.settimeout(0.8)
    return s

def hello_bytes(cookie="", name="PROBE"):
    m = {"type": "hello", "name": name}
    if probe.KEY: m["key"] = probe.KEY
    if cookie:    m["c"] = cookie
    return enc(m)

def exchange(s, payload):
    """Send one datagram, return (first reply bytes or None, reply length)."""
    s.send(payload)
    try:
        d = s.recv(65536)
    except socket.timeout:
        return None, 0
    return d, len(d)

def exchange_binary(s, payload):
    """Send one datagram, return the first BINARY reply (or None, 0).

    Deliberately not "the first reply": a successful hello draws several packets
    - an identity token (D3), the welcome, a leaderboard - and UDP promises
    nothing about which lands first. Asserting on packet [0] made this probe fail
    the moment D3 added one, for a reason that was never a bug.
    """
    s.send(payload)
    deadline = time.time() + 1.5
    while time.time() < deadline:
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        if d and d[0] != 0x7B:      # not '{' - a binary packet
            return d, len(d)
    return None, 0

def as_json(d):
    if not d or d[0] != 0x7B: return None      # not '{' - a binary packet
    try: return json.loads(d.decode("utf-8", "replace"))
    except ValueError: return None

print("challenge: a stranger gets a cookie, not a welcome")
a = raw()
req = hello_bytes()
reply, n = exchange(a, req)
j = as_json(reply)
check(j is not None and j.get("type") == "challenge",
      f"first hello is answered with a challenge (got {j.get('type') if j else reply[:1]})")
cookie = j.get("c", "") if j else ""
check(len(cookie) == 24 and all(ch in "0123456789abcdef" for ch in cookie),
      f"the cookie is 24 hex chars: {cookie!r}")
check(n <= 64, f"the whole reply is {n} bytes - nothing large left the box")

print("wrong cookie is refused, not welcomed")
bad, _ = exchange(a, hello_bytes("0" * 24))
jb = as_json(bad)
check(jb is not None and jb.get("type") == "challenge",
      "a made-up cookie gets challenged again rather than seated")

print("echoing the cookie completes the handshake")
welcome, wn = exchange_binary(a, hello_bytes(cookie))
check(welcome is not None and welcome[0] != 0x7B,
      "the cookie buys a binary welcome")
# Reported, not asserted. The welcome measured here is a LOBBY welcome and is
# tiny (~21 B) because the world is not generated until a match starts - the
# ~3 KB one this whole feature exists to stop reflecting is a LARGE/XL welcome
# from a room mid-match, which is not worth building a probe around. The number
# that IS pinned down is the one above: whatever the welcome grows to, an
# unproven address gets 51 bytes and no more.
print(f"  note  challenge {n}B for a {len(req)}B hello = {n/len(req):.1f}x; "
      f"this room's lobby welcome is {wn}B, a mid-match LARGE one ~3 KB")

print("the cookie is bound to the address it was minted for")
b = raw()   # same machine, different source PORT - a different endpoint
stolen, _ = exchange(b, hello_bytes(cookie))
js = as_json(stolen)
check(js is not None and js.get("type") == "challenge",
      "another endpoint cannot spend our cookie")
b.close()

print("a wrong join key is still answered with silence")
if probe.KEY:
    c = raw()
    quiet, _ = exchange(c, enc({"type": "hello", "name": "X", "key": probe.KEY + "-wrong"}))
    check(quiet is None, "bad key gets no reply at all - not even a challenge")
    c.close()
else:
    print("  SKIP server has no join key set (PLATFORMZ_KEY)")

print("an unregistered endpoint cannot use the directory")
d = raw()
listed, _ = exchange(d, enc({"type": "list", "cur": 0}))
check(listed is None, "`list` from a stranger is dropped unread")
created, _ = exchange(d, enc({"type": "create", "n": "GHOST", "pre": "DEFAULT",
                              "priv": False, "code": ""}))
check(created is None, "`create` from a stranger is dropped unread")
d.close()
# Say goodbye rather than just closing: `a` is holding a real slot in the default
# room now, and the probes below need that room to have space. A silent close
# would leave it occupied for the lobby reap timeout.
a.send(enc({"type": "goodbye"}))
time.sleep(0.3)
a.close()

print("list budget: a small burst, then one reply per second per connection")
lister = C("LISTER")
lister.hello()
time.sleep(1.2)
check(lister.slot is not None, "the probe client got seated (it answers challenges for us)")
check(lister.challenges >= 1, f"...after being challenged {lister.challenges}x")
lister.matchlists.clear()
for _ in range(25):
    lister.send({"type": "list", "cur": 0})
    time.sleep(0.02)          # ~0.5s of requests, as fast as a script would
time.sleep(0.6)
burst = len(lister.matchlists)
# The bucket holds 3 and refills at 1/s, so half a second of hammering drains it
# and earns roughly one more - never the 25 the caller asked for.
check(1 <= burst <= 5, f"25 requests in half a second produced {burst} replies")
time.sleep(1.1)
lister.send({"type": "list", "cur": 0})
time.sleep(0.5)
check(len(lister.matchlists) == burst + 1, "a request a second later is answered normally")

print("bad join codes: five a minute, then refused")
guesser = C("GUESSER")
guesser.hello()
time.sleep(1.2)
check(guesser.slot is not None, "the guesser got seated")
for i in range(9):
    guesser.send({"type": "join", "m": f"ZZ{i:02d}", "code": ""})
    time.sleep(0.08)
time.sleep(0.6)
reasons = guesser.joinfails
check(reasons.count("notfound") == 5,
      f"exactly 5 guesses were answered: {reasons}")
check(reasons.count("rate_limited") == 4,
      "the rest were refused as rate-limited, so the 4-char code space is not walkable")

# Once the budget is spent it gates EVERY attempt, not just the wrong ones -
# whether a code is a guess is only knowable after the lookup that answers the
# guess. This is the cost of the defence and is worth pinning down, because the
# tempting "let good codes through" version would answer every guess and stop
# being a limit at all.
guesser.send({"type": "list", "cur": 0})
time.sleep(0.6)
rooms = guesser.matchlists[-1].get("m", []) if guesser.matchlists else []
check(bool(rooms), "the list came back, so there is a real room to aim at")
real = rooms[0]["c"] if rooms else ""
if real:
    guesser.joinfails.clear()
    guesser.send({"type": "join", "m": real, "code": ""})
    time.sleep(0.8)
    check(guesser.joinfails == ["rate_limited"],
          f"a real code is refused too while the budget is spent: {guesser.joinfails}")

    # ...and the limit really is per connection, not per server or per room: a
    # fresh client walks straight into the same room the guesser was refused.
    clean = C("CLEAN")
    clean.hello()
    time.sleep(1.2)
    clean.send({"type": "join", "m": real, "code": ""})
    time.sleep(0.8)
    check(clean.matchCode == real and not clean.joinfails,
          f"a fresh connection joins {real} unaffected (got {clean.matchCode!r})")
    clean.drop(goodbye=True)

lister.drop(goodbye=True)
guesser.drop(goodbye=True)

print()
if fails:
    print(f"probe_cookie: {fails} CHECK(S) FAILED")
    sys.exit(1)
print("probe_cookie: all checks passed")
