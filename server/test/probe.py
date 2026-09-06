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
STATE, WELCOME, CHUNK, FULL = 0x09, 0x02, 0x03, 0x06
PHASES = {0: "lobby", 1: "countdown", 2: "playing", 3: "gameover"}
def enc(o): return json.dumps(o, separators=(",", ":")).encode()

class C:
    def __init__(self, name):
        self.name, self.slot, self.seq, self.epoch = name, None, 0, 0
        self.phase, self.nplayers, self.alive = "(none)", 0, True
        # Leaderboard arrivals, and the phase we believed we were in at the time.
        # The server sends the table BEFORE the state packet announcing GAMEOVER,
        # so a correct client must handle it while still in "playing".
        self.leaderboards = []     # list of (phase_when_received, [(name, score)])
        self.matchlists   = []     # directory replies: dicts as sent
        self.joinfails    = []     # refusal reasons, in order
        self.created      = []     # codes of rooms we made
        self.players      = {}     # name -> {hp, score, alive, bot} from the last state
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.connect((HOST, PORT)); self.s.settimeout(0.2)
        self.parts = {}
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._ping, daemon=True).start()
    def send(self, o): self.s.send(enc(o))
    def hello(self):
        m = {"type": "hello", "name": self.name}
        if KEY: m["key"] = KEY      # the server's join gate wants it in the hello
        self.send(m)
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
                self.slot = struct.unpack_from("<i", d, 1)[0]
            elif tag == STATE:
                # header: u8 tag, u32 tick, u32 lastSeq  -> body starts at 9
                # body: u8 phase, f32 countdown, u32 epoch, u8 nplayers-opt,
                #       7*f32, u8 fburn, u8 fregen, u8 flags, u8 rosterCount
                self.phase = PHASES.get(d[9], "?")
                self.epoch = struct.unpack_from("<I", d, 14)[0]
                self.nplayers = d[50]
                # Decode the roster so tests can assert on a player's actual
                # state. Layout per buildStateBodyBinary: u32 id, 3x qpos(i16),
                # 3x qvel(i16), yaw+pitch(u16), u8 hp, u8 fuel, u8 ammo,
                # u8 flash, u8 spectate, u16 score, u8 flags, u8 oob, then a
                # length-prefixed name.
                self.players = {}
                off = 51
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
                        self.players[name] = {
                            "id": pid, "hp": hp, "score": score,
                            "alive": bool(flags & 1), "bot": bool(flags & 2),
                            "host": bool(flags & 32),
                        }
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
                if t == "leaderboard":
                    rows = [(e.get("n", ""), e.get("s", 0)) for e in j.get("lb", [])]
                    self.leaderboards.append((self.phase, rows))
                elif t == "matchlist":
                    self.matchlists.append(j)
                elif t == "created":
                    self.created.append(j.get("m", ""))
                elif t == "joinfail":
                    self.joinfails.append(j.get("why", "?"))

def step(label, cs, secs=1.2):
    time.sleep(secs)
    for c in cs:
        print(f"{label:22s} {c.name}: slot={c.slot} phase={c.phase} "
              f"epoch={c.epoch} roster={c.nplayers}")

OPTS = {"half": 120.0, "plat": 128, "roid": 18, "nplayers": 4, "diff": 0.2,
        "welast": 0.5, "pelast": 0.33, "boost": 1.0, "rspeed": 1.0,
        "xradius": 1.0, "jthrust": 1.0, "fburn": 5, "fregen": 40,
        "walls": True, "phys": False, "ff": True, "coast": True}

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
