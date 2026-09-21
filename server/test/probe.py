#!/usr/bin/env python3
"""Headless PLATFORMZ protocol client (UDP) - drives a whole match lifecycle.

Written to prove the A1 Match refactor changed nothing observable: run it against
two server builds and diff the transcripts. Useful beyond that as the only way to
exercise the server without launching GUI clients.

    cd server && ./gameserver &
    python3 test/probe.py

Prints one line per client per step: slot, phase, match epoch, roster size. Every
host-only rule is checked by having the NON-host try it first (steps 2 and 6);
those must be ignored.

Two things this got wrong the first time, both worth knowing before you extend it:
  - The server parses inbound JSON by literal string search, so `json.dumps`
    defaults ("type": "hello", with a space) never match. Use compact separators.
  - The lobby reaps a silent UDP client after 3s (UDP_CLIENT_TIMEOUT_LOBBY), so a
    client that only speaks when acting gets dropped mid-test. Hence the ping
    thread - the real client does the same at 1/sec.
"""
import socket, struct, json, time, threading, os

# Target. Defaults to localhost, but A4's whole point is measuring the server
# WITHOUT the load generator competing for the same CPU - so these have to be
# drivable from another machine.
#   PLATFORMZ_HOST=203.0.113.9 python3 test/probe_load.py --clients 8
HOST = os.environ.get("PLATFORMZ_HOST", "127.0.0.1")
PORT = int(os.environ.get("PLATFORMZ_PORT", "9000"))
KEY  = os.environ.get("PLATFORMZ_KEY", "")   # if the server runs with a join gate

def set_target(host, port=None):
    """Point every client created after this at `host`."""
    global HOST, PORT
    HOST = host
    if port: PORT = int(port)
# Tags per netbin.h. WELCOME is 0x0A since the welcome grew the room's code
# and kind - values are never recycled there, so it went past the high-water
# mark rather than taking 0x03. STATE is 0x0B since the options block grew the
# maxbots + minhumans bytes; it skipped 0x0A because WELCOME holds it.
STATE, WELCOME, CHUNK, FULL = 0x0B, 0x0A, 0x03, 0x06
PHASES = {0: "lobby", 1: "countdown", 2: "playing", 3: "gameover"}
# Wire order of mapSizeOrder (constants.h). Append only, same as there.
MAP_SIZES = ["SMALL", "MEDIUM", "LARGE", "XL"]
def enc(o): return json.dumps(o, separators=(",", ":")).encode()

class C:
    # `cid` is the client's install id from its local profile (D1). The server
    # uses it to hand a reconnecting player back the slot they dropped out of.
    # `match` names the room to land in, which a reconnect must state or it goes
    # back to the default one.
    def __init__(self, name, cid="", match=""):
        self.cid, self.wantMatch = cid, match
        # E1's UDP handshake cookie, once the server has issued one. See hello().
        self.cookie = ""
        self.challenges = 0        # how many times we were asked to prove our address
        # D3's identity token: what we PRESENT (set before hello to play a
        # returning client) and every one the server ISSUED us, in order. A real
        # client keeps the latest in its profile; a probe keeps the list, because
        # "how many were issued" is the observable that says whether the server
        # recognised us.
        self.token = ""
        self.identities = []
        self.name, self.slot, self.seq, self.epoch = name, None, 0, 0
        self.phase, self.nplayers, self.alive = "(none)", 0, True
        # Which room the server put us in, and how it is run - straight off the
        # welcome, not the code we asked for.
        self.matchCode, self.matchKind = "", ""
        self.countdown = 0.0
        # The two rules with no OPTIONS slider, echoed in every state packet
        # like the rest of the bundle. Per-preset, so never assumed.
        self.maxBots   = 0
        self.minHumans = 0
        # The arena the LOBBY is advertising (from the state packet's option
        # flags) and the one actually generated (from the welcome's halfSize).
        # They are different questions: the first is the pending choice everyone
        # can see before start, the second is what got built.
        self.mapSize = ""
        self.half = 0.0
        # Leaderboard arrivals, and the phase we believed we were in at the time.
        # The server sends the table BEFORE the state packet announcing GAMEOVER,
        # so a correct client must handle it while still in "playing".
        self.leaderboards = []     # list of (phase_when_received, [(name, score, isBot)])
        self.personalBest = None   # (name, score) from the last leaderboard, or None
        self.matchlists   = []     # directory replies: dicts as sent
        self.joinfails    = []     # refusal reasons, in order
        self.unseated     = 0      # how many times the server said "no slot for you"
        self.created      = []     # codes of rooms we made
        self.players      = {}     # name -> {hp, score, alive, bot} from the last state
        # The same rows keyed by SLOT. Names are not unique - a reconnecting
        # player and the body they left behind are both called the same thing,
        # and the name-keyed dict silently keeps only the last one. Anything
        # reasoning about a particular body has to use this.
        self.slots        = {}     # slot index -> the same dict
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.connect((HOST, PORT)); self.s.settimeout(0.2)
        self.parts = {}
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._ping, daemon=True).start()
    def send(self, o): self.s.send(enc(o))
    def hello(self):
        m = {"type": "hello", "name": self.name}
        if KEY: m["key"] = KEY      # the server's join gate wants it in the hello
        if self.cid:       m["cid"]   = self.cid
        if self.wantMatch: m["match"] = self.wantMatch
        # E1's handshake cookie. The first hello of a session has none, so the
        # server replies with a challenge instead of a welcome and _read re-sends
        # this immediately with the cookie in it. Every caller therefore sees the
        # same thing it always did (a slot appears a round trip later), which is
        # why no probe but probe_cookie.py had to change.
        if self.cookie:    m["c"]     = self.cookie
        if self.token:     m["tok"]   = self.token
        self.send(m)

    def drop(self, goodbye=False):
        """Leave.

        goodbye=False goes dark the way a crash or a lost network does: stop
        pinging, close the socket, say nothing. The server only notices after
        UDP_CLIENT_TIMEOUT (10 s mid-match).

        goodbye=True is a player quitting deliberately, which is what the real
        client sends on teardown. The slot is freed immediately - so a test that
        needs a vacated slot gets one in a beat instead of leaving the body
        unattended in a live firefight for ten seconds first."""
        if goodbye:
            try: self.send({"type": "goodbye"})
            except OSError: pass
            time.sleep(0.3)          # let it arrive before the socket goes
        self.alive = False
        try: self.s.close()
        except OSError: pass
    def _ping(self):
        while self.alive:
            time.sleep(0.5)
            try: self.send({"type": "ping"})
            except OSError: return
    def _read(self):
        while self.alive:
            try: d = self.s.recv(65536)
            except (socket.timeout, OSError): continue
            if not d: continue
            tag = d[0]
            if tag == CHUNK:
                gen, idx, cnt = d[1], d[2], d[3]
                self.parts.setdefault(gen, {})[idx] = d[4:]
                if len(self.parts[gen]) != cnt: continue
                d = b"".join(self.parts[gen][i] for i in range(cnt))
                self.parts.clear(); tag = d[0]
            if tag == WELCOME:
                # u8 tag, i32 slot, u32 tick, u8 codeLen + code, u8 kind, then
                # the static world.
                self.slot = struct.unpack_from("<i", d, 1)[0]
                try:
                    clen = d[9]
                    self.matchCode = d[10:10 + clen].decode("utf-8", "replace")
                    self.matchKind = "official" if d[10 + clen] else "custom"
                    self.half = struct.unpack_from("<f", d, 11 + clen)[0]
                except (IndexError, struct.error):
                    pass
            elif tag == STATE:
                # header: u8 tag, u32 tick, u32 lastSeq  -> body starts at 9
                # body: u8 phase, f32 countdown, u32 epoch, u8 nplayers-opt,
                #       u8 maxbots, u8 minhumans, 7*f32, u8 fburn, u8 fregen,
                #       u8 flags, u8 rosterCount
                # (maxbots/minhumans at 19-20 are what STATE_BIN_VERSION 0x0B
                # added to the options block; every offset after them shifted.)
                self.phase = PHASES.get(d[9], "?")
                # f32 at 10: the pre-match countdown, and in LOBBY the official
                # room's auto-start timer (0 unless armed).
                self.countdown = struct.unpack_from("<f", d, 10)[0]
                self.epoch = struct.unpack_from("<I", d, 14)[0]
                # The two rules with no slider, straight after the roster size.
                self.maxBots   = d[19]
                self.minHumans = d[20]
                # Option flags at 51; bits 16/32 are the map index (see
                # mapSizeOrder in constants.h). Roster count follows at 52.
                self.mapSize = MAP_SIZES[(d[51] >> 4) & 0x3]
                self.nplayers = d[52]
                # Decode the roster so tests can assert on a player's actual
                # state. Layout per buildStateBodyBinary: u32 id, 3x qpos(i16),
                # 3x qvel(i16), yaw+pitch(u16), u8 hp, u8 fuel, u8 ammo,
                # u8 flash, u8 spectate, u16 score, u8 flags, u8 oob, then a
                # length-prefixed name.
                self.players = {}
                self.slots   = {}
                off = 53
                try:
                    for _ in range(self.nplayers):
                        pid   = struct.unpack_from("<I", d, off)[0]; off += 4
                        off  += 6 + 6 + 4                      # pos, vel, yaw, pitch
                        hp    = d[off]; off += 1
                        off  += 1 + 1 + 1 + 1                  # fuel, ammo, flash, spectate
                        score = struct.unpack_from("<H", d, off)[0]; off += 2
                        flags = d[off]; off += 1
                        off  += 1                              # oob timer
                        nlen  = d[off]; off += 1
                        name  = d[off:off + nlen].decode("utf-8", "replace"); off += nlen
                        row = {
                            "id": pid, "hp": hp, "score": score,
                            "alive": bool(flags & 1), "bot": bool(flags & 2),
                            # Bit 4: the server is showing this slot. Off for a
                            # slot the room's maxBots left EMPTY - no body, but
                            # still in the roster and still joinable.
                            "active": bool(flags & 4),
                            "host": bool(flags & 32),
                            "name": name,
                        }
                        self.players[name] = row
                        self.slots[pid - 1] = row      # pid is slot + 1 on the wire
                except (IndexError, struct.error):
                    pass                                       # truncated/chunked frame
            elif tag == FULL:
                self.phase = "full"
            elif tag == 0x7B:                      # '{' - a JSON message
                try:
                    j = json.loads(d.decode("utf-8", "replace"))
                except ValueError:
                    continue
                t = j.get("type")
                if t == "identity":
                    # Store it AND present it from now on, which is what the real
                    # client does - so a reconnect inside one probe run looks like
                    # a returning player rather than a new one.
                    tok = j.get("tok", "")
                    if tok:
                        self.identities.append(tok)
                        self.token = tok
                elif t == "challenge":
                    # Return-routability check (E1): echo the cookie straight
                    # back. Only when it CHANGES, or a server that kept
                    # rejecting us would put this thread in a tight hello loop.
                    self.challenges += 1
                    c = j.get("c", "")
                    if c and c != self.cookie:
                        self.cookie = c
                        self.hello()
                elif t == "leaderboard":
                    # (name, score, isBot). The bot flag arrived with D4, when the
                    # table was re-keyed on identity and bots - which have none -
                    # got their own class rather than being dropped.
                    rows = [(e.get("n", ""), e.get("s", 0), e.get("b", False))
                            for e in j.get("lb", [])]
                    self.leaderboards.append((self.phase, rows))
                    # D5: our own best run, pinned under the board. The server
                    # omits it when there is nothing to pin (no runs, or it is
                    # already up there), so None is a meaningful value here.
                    b = j.get("best")
                    self.personalBest = (b.get("n", ""), b.get("s", 0)) if b else None
                elif t == "matchlist":
                    self.matchlists.append(j)
                elif t == "created":
                    self.created.append(j.get("m", ""))
                elif t == "unseated":
                    # Connected, holding no slot. The seatless counterpart of the
                    # welcome - counted rather than flagged, because a client can
                    # be parked more than once (join refused, leave, ...).
                    #
                    # It also UNDOES a welcome: slot and room come from a welcome
                    # and this says we have neither any more. Without clearing
                    # them a probe that leaves a room still reads the room it
                    # left, because no further welcome ever arrives to correct it.
                    self.unseated += 1
                    self.slot = None
                    self.matchCode = ""
                elif t == "joinfail":
                    self.joinfails.append(j.get("why", "?"))

def step(label, cs, secs=1.2):
    time.sleep(secs)
    for c in cs:
        print(f"{label:22s} {c.name}: slot={c.slot} phase={c.phase} "
              f"epoch={c.epoch} roster={c.nplayers}")

# The bundle a host client sends. Mirrors MatchOptions{} defaults, so a probe
# that starts a match gets the same room an untouched OPTIONS modal would - and,
# like the real client, sends every key: an absent one resets to the compile-time
# default rather than keeping whatever the room's preset asked for.
OPTS = {"half": 120.0, "plat": 128, "roid": 18, "nplayers": 4, "diff": 0.2,
        "maxbots": 7, "minhumans": 2,
        "welast": 0.5, "pelast": 0.33, "boost": 1.0, "rspeed": 1.0,
        "xradius": 1.0, "jthrust": 1.0, "fburn": 5, "fregen": 40,
        "walls": True, "phys": False, "ff": True, "coast": True}

# The room a connection lands in when it names none is an OFFICIAL room: locked,
# no host, no START button - it starts itself once enough humans are present. A
# probe that wants to DRIVE a match therefore has to make a room it hosts.
#
# Public rather than invite-only, so the other clients can join by code alone (a
# private room demands its own code back as the join code). Each probe gets its
# own fresh server, so nothing else is ever looking at this listing.
def host_room(client, name="PROBE ROOM", preset="DEFAULT", timeout=5.0):
    """Create a CUSTOM room, land in it, and return its code ("" on failure)."""
    client.send({"type": "create", "n": name, "pre": preset, "priv": False, "code": ""})
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.1)
        if client.created and client.matchCode == client.created[-1]:
            return client.matchCode
    return ""


def take_any_room(client, timeout=6.0):
    """Get a seat ANYWHERE - the probe's QUICK MATCH.

    hello() does not land you in a room any more: you arrive in the directory
    holding nothing and choose (C6b). Probes that need a seat but do not care
    which one say so here, instead of relying on the server to pick for them.
    """
    client.send({"type": "quick"})
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.1)
        if client.slot is not None:
            return client.matchCode
    return ""


def handshake_landed(client):
    """Is this client through the door, seated or not?

    Either answer proves the handshake completed: a welcome (we hold a slot) or
    an `unseated` (we are connected and hold nothing). Before C6a only the first
    existed, so probes asserted on a slot to mean "connected" - which stopped
    being the same question once arriving without a room became normal.
    """
    return client.slot is not None or client.unseated > 0


def join_room(client, code, timeout=5.0):
    """Move an already-helloed client into `code`. True once it is actually there."""
    client.send({"type": "join", "m": code, "code": ""})
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.1)
        if client.matchCode == code:
            return True
    return False


# Guarded so probe_idle.py (and anything else) can import C/OPTS without
# running this whole scenario as a side effect of the import.
def main():
    a = C("ALPHA")
    time.sleep(0.3)
    b = C("BRAVO")
    a.hello(); time.sleep(0.3); b.hello()
    step("1 joined (lobby)", [a, b])

    b.send({"type": "start", **OPTS});  step("2 non-host start", [a, b])
    a.send({"type": "start", **OPTS});  step("3 host start", [a, b], 1.0)
    step("4 countdown->playing", [a, b], 5.0)

    for i in range(60):
        for c in (a, b):
            c.seq += 1
            c.send({"seq": c.seq, "ep": c.epoch, "mx": 0.0, "mz": 1.0, "jp": False,
                    "grav": False, "fire": i == 5, "yaw": -1.57, "pitch": 0.0})
        time.sleep(0.016)
    step("5 after input", [a, b], 1.0)

    b.send({"type": "endmatch"}); step("6 non-host endmatch", [a, b])
    a.send({"type": "endmatch"}); step("7 host endmatch", [a, b])
    a.send({"type": "start", **OPTS}); step("8 restart", [a, b], 7.0)
    b.alive = False; b.send({"type": "goodbye"}); step("9 bravo left", [a], 1.5)


if __name__ == "__main__":
    main()
