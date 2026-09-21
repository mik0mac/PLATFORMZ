#!/usr/bin/env python3
"""D3: the server-issued identity token, end to end.

`identity_test.cpp` proves the token's crypto in isolation. This proves the
POLICY around it, which is where the mistakes would be: that a client with no
token is issued one, that presenting it back is recognised (and does NOT churn a
new one), and above all that a bad token is never a reason to refuse a join.

That last one is the whole reason to test this over a socket. A token is a
convenience; a player holding a stale one after the operator rotated the secret
is still a player, and the failure mode to guard against is a server that decides
otherwise and locks them out of the game to protect a leaderboard row.

    cd server && ./gameserver &
    python3 test/probe_identity.py
"""
import sys, os, time, json, socket
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe
from probe import C, handshake_landed, enc

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

HEX = "0123456789abcdef"

print("a client with no token is issued one")
a = C("NEWCOMER")
a.hello()
time.sleep(1.2)
check(handshake_landed(a), "through the handshake (identity is issued before any seat)")
check(len(a.identities) == 1, f"exactly one identity was issued: {a.identities}")
tok = a.identities[0] if a.identities else ""
check(len(tok) == 64 and all(c in HEX for c in tok),
      f"the token is 64 hex chars: {tok!r}")

print("two clients get different identities")
b = C("OTHER")
b.hello()
time.sleep(1.2)
check(len(b.identities) == 1, "the second client was issued one too")
check(bool(b.identities) and b.identities[0] != tok,
      "...and it is a different token")
# The id is the token's first half - two players must never share one, because
# that is precisely the collision D3 exists to stop (two people typing MIKE).
check(bool(b.identities) and b.identities[0][:32] != tok[:32],
      "...carrying a different identity, not just a different signature")
b.drop(goodbye=True)

print("presenting a valid token is recognised, and does not churn a new one")
c = C("RETURNER")
c.token = tok                      # the same client, a session later
c.hello()
time.sleep(1.2)
check(handshake_landed(c), "through the handshake while presenting a token")
check(c.identities == [],
      f"NO new token was issued - the old one verified: {c.identities}")
c.drop(goodbye=True)

print("a bad token is replaced, never refused")
for label, bad in [
    ("garbage",            "z" * 64),
    ("right shape, wrong signature", tok[:32] + "0" * 32),
    ("too short",          tok[:32]),
    ("someone else's id under our tag", "f" * 32 + tok[32:]),
]:
    d = C("BADTOKEN")
    d.token = bad
    d.hello()
    time.sleep(1.2)
    check(handshake_landed(d), f"{label}: still let in (never hard-fail)")
    check(len(d.identities) == 1, f"{label}: and was issued a fresh token")
    check(bool(d.identities) and d.identities[0] != bad,
          f"{label}: which is not the one it presented")
    d.drop(goodbye=True)
    time.sleep(0.2)

print("an unproven address gets no token at all")
# E1's cookie comes first: a hello with no cookie is answered with a challenge
# and nothing else. An identity handed out before the address is proved would be
# a free credential for anyone who can spoof a source address.
raw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
raw.connect((probe.HOST, probe.PORT))
raw.settimeout(0.8)
m = {"type": "hello", "name": "SPOOF"}
if probe.KEY: m["key"] = probe.KEY
raw.send(enc(m))
seen = []
try:
    while True:
        d = raw.recv(65536)
        if d and d[0] == 0x7B:
            seen.append(json.loads(d.decode("utf-8", "replace")).get("type"))
except (socket.timeout, OSError, ValueError):
    pass
check(seen == ["challenge"],
      f"the first reply is a challenge and nothing else: {seen}")
raw.close()

a.drop(goodbye=True)

print()
if fails:
    print(f"probe_identity: {fails} CHECK(S) FAILED")
    sys.exit(1)
print("probe_identity: all checks passed")
