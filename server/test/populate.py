#!/usr/bin/env python3
"""Fill a running server with rooms so the browser has something to show.

This is a TOY, not a test: it exists so you can look at the match browser with
more than five empty official rooms in it - scroll it, watch the ordering change
as rooms fill, check a long list on a phone-sized window. Nothing here asserts
anything.

    cd server && ./gameserver &
    python3 test/populate.py                 # fill to capacity, varied
    python3 test/populate.py --rooms 4       # just a few
    python3 test/populate.py --boring        # no variety, just a long list

IT MUST KEEP RUNNING. A room with nobody in it is reaped after 30 s, so the
connections that made these rooms have to stay up or the list empties itself
about half a minute after you look away. Leave this in its own terminal and
Ctrl-C when you are done - that drops every client with a `goodbye`, so the rooms
go immediately rather than lingering for the reap.

THE PER-ADDRESS ROOM BUDGET WILL STOP YOU AT THREE. Every client here comes from
one address, and E2 allows three rooms per address with one back every two
minutes. Start the server with the budget off:

    cd server && PLATFORMZ_MAX_ROOMS_PER_ADDR=0 ./gameserver

The script detects that refusal and says so rather than quietly making 3 rooms.

Points at 127.0.0.1:9000 unless PLATFORMZ_HOST / PLATFORMZ_PORT / PLATFORMZ_KEY
say otherwise, same as the probes.
"""
import sys, os, time, argparse, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room

# This script's whole output is progress and a summary, and it then blocks
# forever holding the rooms open. Python block-buffers stdout when it is not a
# terminal, so piping it to a file or a log would show NOTHING at all until
# Ctrl-C - the summary would sit in the buffer behind the sleep.
try: sys.stdout.reconfigure(line_buffering=True)
except AttributeError: pass          # pre-3.7; a terminal is line-buffered anyway

# Names that look like rooms people made, because a list of ROOM 00..ROOM 30 is
# a worse test of a browser than a list of real-looking ones: you cannot tell at
# a glance whether it re-ordered, and long names are what overflow a row.
NAMES = [
    "FRIDAY NIGHT", "NO ROCKETS", "BEGINNERS WELCOME", "THE USUAL",
    "ASTEROID DERBY", "LOW GRAVITY LOUNGE", "SPEEDRUN PRACTICE", "CHAOS HOUR",
    "MIKE'S MATCH", "AFTER WORK", "TOURNAMENT QUALIFIER", "JUST MESSING ABOUT",
    "PRO LOBBY", "ZERO G", "THE LONG GAME", "ROOKIES ONLY", "GRUDGE MATCH",
    "LUNCHTIME", "SUNDAY LEAGUE", "ONE MORE ROUND", "THE PIT", "FINAL BOSS",
    "QUIET ONE", "WALL BOUNCERS", "FUEL CRISIS", "OPEN HOUSE",
]
PRESETS = ["DEFAULT", "CLASSIC HYPED", "SPAMMERS DELIGHT", "MAYHEM", "VOID"]

ap = argparse.ArgumentParser(description="Fill a server with rooms to look at.")
ap.add_argument("--rooms", type=int, default=0,
                help="how many custom rooms (default: until the server refuses)")
ap.add_argument("--boring", action="store_true",
                help="no variety: every room public, DEFAULT, and holding only "
                     "its host - just a long list to scroll")
ap.add_argument("--no-play", action="store_true",
                help="do not start any match, so every room stays in its lobby")
ap.add_argument("--seed", type=int, default=None,
                help="repeat an exact layout")
args = ap.parse_args()
rng = random.Random(args.seed)

clients = []          # every connection, so Ctrl-C can put them all down
rooms   = []          # (code, name, preset, players, private)

def connect(name):
    c = C(name)
    c.hello()
    time.sleep(0.45)          # the UDP handshake is two round trips (E1)
    clients.append(c)
    return c

def bail(why):
    print(f"\n{why}")
    teardown()
    sys.exit(1)

def teardown():
    if not clients: return
    print(f"closing {len(clients)} connections...")
    for c in clients:
        try: c.drop(goodbye=True)
        except Exception: pass

print(f"populating {os.environ.get('PLATFORMZ_HOST','127.0.0.1')}:"
      f"{os.environ.get('PLATFORMZ_PORT','9000')}")

try:
    # --- the rooms themselves -------------------------------------------------
    # No target count by default: create until the server says stop, so this
    # works at whatever MATCH_MAX_CONCURRENT is set to without being told.
    want  = args.rooms if args.rooms > 0 else 999
    names = NAMES[:]
    rng.shuffle(names)
    while len(rooms) < want:
        host = connect(f"HOST{len(rooms)}")
        nm   = names[len(rooms) % len(names)]
        if len(rooms) >= len(names): nm += f" {len(rooms)//len(names)+1}"
        pre  = "DEFAULT" if args.boring else rng.choice(PRESETS)
        # Roughly one room in six hidden from the browser, so there is always a
        # gap between "rooms on the server" and "rooms you can see".
        priv = False if args.boring else (rng.random() < 0.17)
        host.send({"type": "create", "n": nm, "pre": pre, "priv": priv, "code": ""})
        deadline = time.time() + 4.0
        code = ""
        while time.time() < deadline:
            time.sleep(0.1)
            if host.created and host.matchCode == host.created[-1]:
                code = host.matchCode; break
            if host.joinfails: break
        if code:
            rooms.append([code, nm, pre, 1, priv])
            continue

        why = host.joinfails[-1] if host.joinfails else "no answer"
        if why == "too_many_rooms":
            bail("The server's PER-ADDRESS room budget stopped us at "
                 f"{len(rooms)} rooms.\n"
                 "Every client here shares one address, and the default is 3 "
                 "(one back every 2 min).\nRestart the server with the budget "
                 "off:\n\n    cd server && PLATFORMZ_MAX_ROOMS_PER_ADDR=0 "
                 "./gameserver\n")
        if why == "server_full":
            print(f"server is full at {len(rooms)} custom rooms "
                  "(the rest of the cap is the pinned official rooms)")
            break
        bail(f"create refused: {why}")

    if not rooms: bail("could not make a single room")

    # --- people in them ------------------------------------------------------
    # The browser sorts by how close a room is to being a GAME, so a list of
    # rooms that all hold one player demonstrates nothing. Spread the occupancy
    # and the ordering becomes visible - and wrong ordering becomes obvious.
    # Note a custom room can never show 0/8 here: its host has to stay in it or
    # the room is reaped. Only the pinned official rooms are ever truly empty.
    if not args.boring:
        for r in rooms:
            extra = rng.choice([0, 0, 1, 1, 2, 3, 5])
            for k in range(extra):
                g = connect(f"P{r[0]}{k}")
                if join_room(g, r[0]): r[3] += 1

    # --- and a couple of matches actually running ----------------------------
    # So the phase band is visible too: a running match sorts below every lobby
    # you could still join, however full it is.
    if not args.boring and not args.no_play:
        for r in rooms:
            if r[3] >= 2 and rng.random() < 0.35:
                host = next((c for c in clients if c.matchCode == r[0]), None)
                if host:
                    host.send({"type": "start", **OPTS})
                    time.sleep(0.4)

    # --- what you should be looking at ---------------------------------------
    time.sleep(1.5)
    public = [r for r in rooms if not r[4]]
    print()
    print(f"{len(rooms)} custom rooms ({len(public)} public, "
          f"{len(rooms)-len(public)} hidden) + the pinned official ones")
    print(f"{len(clients)} connections held open\n")
    print("  CODE  PLAYERS  PRESET             NAME")
    for code, nm, pre, n, priv in sorted(rooms, key=lambda r: -r[3]):
        print(f"  {code}  {n:>7}  {pre:<18} {nm}{'   (hidden)' if priv else ''}")
    print("\nExpected top of the browser: fullest joinable lobbies first, then")
    print("matches already playing, then the empty official rooms in preset order.")
    print("\nCtrl-C to drop everything.")

    while True: time.sleep(3600)

except KeyboardInterrupt:
    print()
    teardown()
    print("done - the rooms are gone")
