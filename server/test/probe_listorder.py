#!/usr/bin/env python3
"""The browser's ORDER, and the snapshot that keeps it still while you read it.

The match list used to be sorted by room code - an arbitrary key, but a constant
one, which is why nothing had to think about it. It is now sorted by how close
each room is to being a game, which is a LIVE key, so both halves of that need a
test: the order itself, and the fact that it does not move under the player.

THE ORDER, outermost key first:

  1. BAND. Joinable lobbies first, then joinable matches already in progress,
     then rooms you cannot enter at all. A 7/8 room that is already PLAYING is a
     worse place to land than a 2/8 lobby, so fullness must NOT be allowed to
     lift it above one.
  2. FEWEST FREE SPOTS. The anti-fragmentation rule: the second player to arrive
     should land on the first player's room, not beside it. Free spots rather
     than head count, because rosters differ per preset.
  3. PRESET RANK, the typical-to-niche ramp in options.h.
  4. CODE, so the order is total and cannot shuffle between two requests.

Every EMPTY room is clamped to the same free-spot key, which is what makes a
freshly booted server list in exactly preset order - the property this probe
checks first, because it is the one a player sees every single time.

THE SNAPSHOT. A `list` with cursor 0 takes a fresh one; every later page is a
slice of that same frozen vector. Without it, page 1 would be cut from a list
re-sorted since page 0 was sent, and a room could appear on both pages or on
neither. That invariant used to come free from sorting on a key that never
changed.

    cd server && ./gameserver &
    python3 test/probe_listorder.py
"""
import sys, os, re, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room, handshake_landed

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1


def preset_order():
    """The preset keys, in the order options.h declares them.

    Read from the source rather than hardcoded: this probe asserts that the
    browser agrees with that table, and a copy of the table here would agree
    with itself forever while the two drifted apart.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    src  = os.path.join(here, "..", "..", "options.h")
    with open(src) as f:
        return re.findall(r'^\s*\{"([^"]+)",\s*MakePreset\(', f.read(), re.M)


def fresh_list(client, cur=0, wait=1.1):
    """Ask for a page and return its rows. The wait respects E1's one-a-second."""
    client.matchlists.clear()
    client.send({"type": "list", "cur": cur})
    deadline = time.time() + 4.0
    while time.time() < deadline:
        time.sleep(0.1)
        if client.matchlists:
            time.sleep(0.1)
            return client.matchlists[-1]
    return {}


def codes(rows):  return [r.get("c") for r in rows]
def names(rows):  return [r.get("n") for r in rows]
def where(rows, code):
    for i, r in enumerate(rows):
        if r.get("c") == code: return i
    return -1


eye = C("OBSERVER")
eye.hello()
time.sleep(1.0)
check(handshake_landed(eye), "an observer is connected, holding no room")
check(eye.slot is None, "...so it is not in any of the rooms it is about to rank")

print("a freshly booted server lists in preset order")
# The load-bearing property. Every room is empty at boot, so the free-spot key
# ties across all of them and the preset ramp is the entire answer - which is how
# "a controllable, consistent order at every restart" is delivered without a
# second mechanism to enforce it.
lst  = fresh_list(eye)
rows = lst.get("m", [])
check(bool(rows), f"the boot rooms are listed ({lst.get('total')} total)")
check(all(r.get("p", 0) == 0 for r in rows),
      f"every room is empty, so nothing but the ramp can be ordering them: {[r.get('p') for r in rows]}")
want = preset_order()
got  = [r.get("pre") for r in rows]
check(got == want, f"listed in options.h order\n       want {want}\n       got  {got}")
# Kept for the paging test far below, which needs to know which rooms are EMPTY
# without asking for a list it would then be paging through.
boot_codes = codes(rows)

print("one player outranks every empty room, however niche the preset")
# The last preset on the ramp, so preset rank alone would put it dead last. It
# has a player in it, so it must be first: occupancy outranks the ramp, and the
# ramp is only ever a tie-break.
last_preset = want[-1]
b = C("FIRST MOVER")
b.hello()
time.sleep(0.7)
lonely = host_room(b, "LONELY", preset=last_preset)
check(bool(lonely), f"a custom {last_preset} room exists with one player in it ({lonely})")
rows = fresh_list(eye).get("m", [])
check(where(rows, lonely) == 0,
      f"a 1-player {last_preset} room leads the list: {names(rows)}")

print("...and among rooms with a player, fewest free spots wins")
# FREE SPOTS, NOT HEAD COUNT, and the fixture has to be written in those terms.
#
# This section used to put one player in each room, call that a tie, and then add
# a second body to the niche room and assert that 2 players beat 1. Both claims
# were accidents of a bug: every LOBBY had eight slots whatever its preset said,
# so equal head counts really did mean equal free spots. Rooms are sized to their
# own numPlayers now, so a 1-player 6-slot room (5 free) is genuinely a better
# offer than a 1-player 8-slot one (7 free) - it is closer to being a game, which
# is the entire thing this key ranks. Ranking by bodies would put a 5/8 room above
# a 3/4 room, which is the failure the key exists to avoid.
#
# So: fill the niche room until the two TIE on free spots (the ramp must then
# decide), then add one more body (it must then lead). Driven off the listing
# rather than off numbers written here, so re-tuning a preset's roster size can
# never silently invert what this is asserting.
c = C("SECOND HOST")
c.hello()
time.sleep(0.7)
popular = host_room(c, "POPULAR", preset=want[0])
check(bool(popular), f"a second custom room, preset {want[0]} ({popular})")


def free_spots(rows, code):
    for r in rows:
        if r.get("c") == code:
            return r.get("max", 0) - r.get("p", 0)
    return None


rows = fresh_list(eye).get("m", [])
fp, fl = free_spots(rows, popular), free_spots(rows, lonely)
check(fp is not None and fl is not None,
      f"both rooms are listed (popular={fp} free, {last_preset}={fl} free)")

# Bodies into the niche room until it ties. Each one has to be a separate
# connection: a room ranks on the people in it.
joiners = []
while fl is not None and fp is not None and fl > fp:
    j = C(f"FILLER JOINER {len(joiners)}")
    j.hello()
    time.sleep(0.7)
    if not join_room(j, lonely):
        break
    joiners.append(j)
    rows = fresh_list(eye).get("m", [])
    fl = free_spots(rows, lonely)

check(fl == fp, f"the two rooms now tie on free spots ({fl} each, "
                f"{len(joiners)} joiner(s) into {last_preset})")
check(where(rows, popular) == 0 and where(rows, lonely) == 1,
      f"tied on free spots, the ramp decides: {names(rows)[:2]}")

# Break the tie. The niche room is now one seat closer to a game than the
# better-ranked preset, so it must climb over it - fullness first, ramp only
# ever a tie-break.
d = C("JOINER")
d.hello()
time.sleep(0.7)
check(join_room(d, lonely), f"one more player joins the {last_preset} room")
rows = fresh_list(eye).get("m", [])
check(free_spots(rows, lonely) < free_spots(rows, popular),
      f"...leaving it fewer free spots ({free_spots(rows, lonely)} "
      f"vs {free_spots(rows, popular)})")
check(where(rows, lonely) == 0 and where(rows, popular) == 1,
      f"fewer free spots outranks a better preset: {names(rows)[:2]}")

print("a match already in progress sorts BELOW a lobby still filling")
# The phase band, and the one case where fullness must lose. The room that starts
# is the one that just climbed to the top on free spots, so on occupancy alone it
# would stay there. Dropping into a match somebody else is most of the way
# through is a worse offer than a lobby about to begin, so the band outranks the
# count.
b.send({"type": "start", **OPTS})
playing = False
for _ in range(20):
    rows = fresh_list(eye).get("m", [])
    row  = [r for r in rows if r.get("c") == lonely]
    if row and row[0].get("ph") == "playing":
        playing = True
        break
check(playing, f"the fuller room is now playing: {[ (r.get('n'), r.get('ph')) for r in rows ]}")
if playing:
    check(where(rows, popular) < where(rows, lonely),
          f"an emptier LOBBY outranks the fuller MATCH: {[(r.get('n'), r.get('ph'), r.get('p')) for r in rows]}")

print("full and ending rooms sort last")
# Not "hidden" - a row that vanishes reads as a bug, and the browser draws an
# inert reason on it instead. But it must never sit above something you can
# actually join.
rows = fresh_list(eye).get("m", [])
joinable_idx  = [i for i, r in enumerate(rows) if r.get("j")]
closed_idx    = [i for i, r in enumerate(rows) if not r.get("j")]
check(not closed_idx or not joinable_idx or max(joinable_idx) < min(closed_idx),
      f"every joinable room comes first: {[(r.get('n'), r.get('j')) for r in rows]}")

print("paging is a snapshot, not a second look")
# THE REGRESSION TEST FOR THE LIVE SORT. Fill past one page, take page 0, then
# CHANGE THE OCCUPANCY that the order is derived from, and only then ask for page
# 1. Re-deriving the order at that point would cut page 1 from a different list -
# showing a room on both pages, or on neither.
hosts = []
for i in range(6):
    h = C(f"FILLER{i}")
    h.hello()
    time.sleep(0.5)
    if host_room(h, f"FILLER {i}"):
        hosts.append(h)
check(len(hosts) >= 4, f"enough rooms to need a second page ({len(hosts)} extra)")

p0 = fresh_list(eye, 0)
check(p0.get("next", -1) > 0, f"page 0 says there is more (next={p0.get('next')})")

# THE CHURN, and it has to cross the page boundary to prove anything. Moving a
# room around WITHIN page 0 changes nothing about where page 1 starts, so the
# first cut of this test passed against a server that re-sorted on every request
# - it was measuring nothing. What bites is a room from page 1 climbing ONTO page
# 0: re-derive the order now and index 8 of the new list is a room page 0 already
# showed, so the player sees it twice.
#
# The empty official rooms are exactly that room. They have no players, so they
# sort below every custom room a filler just created, which puts them on the far
# side of the boundary - and one player is all it takes to lift one to the top.
stranded = [c for c in boot_codes if c not in set(codes(p0.get("m", [])))]
check(bool(stranded), f"an empty room is sitting on page 1 to push around: {stranded}")
if stranded:
    late = C("LATE")
    late.hello()
    time.sleep(0.7)
    check(join_room(late, stranded[0]),
          f"it gains a player, which would re-rank it to the very top ({stranded[0]})")
    # Hold a moment so the count is genuinely visible to the next sort.
    time.sleep(0.8)

p1 = fresh_list(eye, p0.get("next", 0))
c0, c1 = set(codes(p0.get("m", []))), set(codes(p1.get("m", [])))
check(not (c0 & c1), f"no room appears on both pages: {sorted(c0 & c1)}")
check(len(c0 | c1) == p0.get("total", -1),
      f"and none was skipped: {len(c0 | c1)} rooms across the pages, total says {p0.get('total')}")
check(p1.get("total") == p0.get("total"),
      f"the page count is the snapshot's, not a fresh one ({p0.get('total')} -> {p1.get('total')})")

print("cursor 0 is what takes a new one")
# The server half of the REFRESH button. The client stopped polling when the
# order went live, so this is the ONLY thing that re-reads the world.
p0b  = fresh_list(eye, 0)
rows = p0b.get("m", [])
if stranded:
    # The same room the churn above deliberately left OFF the snapshot page 1 was
    # cut from. A new snapshot has to see it where it now belongs.
    check(where(rows, stranded[0]) >= 0,
          f"the room that gained a player has climbed onto page 0: {names(rows)}")

for cl in [eye, b, c, d] + hosts:
    cl.alive = False
    cl.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "the browser's order and its snapshot both hold")
sys.exit(1 if fails else 0)
