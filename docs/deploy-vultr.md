# Deploy PLATFORMZ on a Vultr VPS (IP-only)

How PLATFORMZ is hosted. Runs the authoritative server on a Vultr VPS with a
stable public IP, reachable over **both** WebSocket (TCP) and UDP, and serves the
browser client from the same box over plain HTTP.

(There was once a throwaway alternative — a server on a GitHub Actions runner
behind a cloudflared tunnel, with the client on GitHub Pages. It was retired: the
tunnel URL changed every run, the job died after a few hours, and it had none of
the join key, identity secret or persistence this has. Nothing references it now.)

This guide is **IP-only** (no domain, no TLS) — the quickest thing that works while
testing. The [HTTPS upgrade](#https-upgrade-later) at the bottom is the "proper"
persistent setup for later.

## What goes where

- **On the VPS:** the headless **game server** (`gameserver`) and the **web client
  files** (`web/platformz.*`). Nothing else needs uploading — the one file the
  server writes, the cumulative scoreboard, it creates itself under
  `/var/lib/platformz` (step 6).
- **Not on the VPS:** the native desktop `platformz` — each player builds/runs that
  on their own Mac and points it at the VPS IP. Only the browser client is hosted.
- **Ports:** the server listens on **9000** for both TCP/WebSocket (browser + native
  `ws://`) and UDP (native `udp://`). The web page is served on **80**.

> Below, `SERVER_IP` = the VPS's public **IPv4** address — use it everywhere
> (baked-in client address, `?server=`, native `udp://`/`ws://`). The server is
> IPv4-only (it binds `0.0.0.0` for TCP and `udp::v4()` for UDP), so the box's IPv6
> address won't connect; ignore it.

---

## 1. Create the Vultr instance

Vultr control panel → **Deploy → Cloud Compute – Shared CPU**:
- **OS:** Ubuntu 24.04 LTS
- **Plan:** smallest (1 vCPU / 1 GB, ~$5–6/mo) is plenty for a headless server
- **Region:** closest to your players (latency)
- Add your SSH key, deploy, note the **public IP**.

## 2. Install build tools

```bash
ssh root@SERVER_IP
apt update && apt upgrade -y
apt install -y build-essential libboost-dev git nginx
```
`build-essential` → `g++`/`make`; `libboost-dev` provides the Boost **headers** the
server needs (Boost.System is header-only since Boost 1.69, so there's nothing to
link — no `libboost-system-dev` required); `nginx` serves the web page. **No raylib
needed** — the server compiles against `server/raylib_server_stub.h` (math types only).

## 3. Get the code onto the box

Everything the box needs is tracked in git — the server source **and** the built
browser client (`web/platformz.*` is committed) — while the heavyweight local-only
stuff (`audio-src/` masters, `WireframeTests/`, binaries) is gitignored and never
makes the trip. So deploying is just a clone. Commit + push first
(`git add -A && git commit && git push -u origin <your-branch>`), then on the box:
```bash
git clone -b <your-branch> https://github.com/mik0mac/PLATFORMZ.git /opt/PLATFORMZ
```

> A plain `git clone` pulls `main`. If your latest work is on a feature branch,
> clone that branch, or the server will be built from stale code. Uncommitted work
> never deploys — if something is missing on the box, commit it and `git pull`.
> (Don't be tempted to rsync the working tree up instead: it drags along ~140 MB
> of audio masters and prototypes that the server has no use for.)

## 4. Build the server

```bash
cd /opt/PLATFORMZ/server
make
./gameserver   # smoke test: "PLATFORMZ server | port 9000 (TCP/WebSocket + UDP) | 60 Hz"
```
Ctrl-C once it starts.

## 5. Open the firewall

Two layers — do both if you attach a Vultr cloud firewall.

**OS-level (ufw), on the box:**
```bash
ufw allow 22/tcp        # SSH — FIRST, or you lock yourself out
ufw allow 80/tcp        # web page
ufw allow 9000/tcp      # WebSocket (browser + native ws://)
ufw allow 9000/udp      # native udp://
ufw --force enable
```
**Vultr cloud firewall (control panel):** a fresh instance has none attached, so
ports are already open. *If* you attach one, add matching inbound rules: TCP 22, 80,
9000 and **UDP 9000** — the UDP rule is easy to forget, and without it native
`udp://` silently fails.

## 6. Keep the server running (systemd)

Survives reboots, restarts on crash:
```bash
cat >/etc/systemd/system/platformz.service <<'EOF'
[Unit]
Description=PLATFORMZ game server
After=network.target

[Service]
DynamicUser=yes
StateDirectory=platformz
Environment=PLATFORMZ_SCORES=/var/lib/platformz/scores
WorkingDirectory=/opt/PLATFORMZ/server
ExecStart=/opt/PLATFORMZ/server/gameserver
Restart=always

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now platformz
systemctl status platformz     # "active (running)"
journalctl -u platformz -f     # live log; players climb as people join
```

`DynamicUser=yes` runs the server as a throwaway unprivileged user instead of
root: port 9000 doesn't need privileges, so this costs nothing and a compromised
server can't touch the box. Both other lines exist because that user is
unprivileged and has no home:

- **`StateDirectory=platformz`** creates `/var/lib/platformz` owned by the
  transient user before the service starts, and keeps it across restarts. The
  server writes its cumulative scoreboard there. Without it the scoreboard has
  nowhere to go — the default path is relative to the root-owned
  `WorkingDirectory`, so every write fails and all-time scores silently reset on
  each restart. Do **not** fix that by `chown`ing the repo instead: that puts
  live game data inside a git checkout, where a redeploy or `git clean` can take
  it, and hands the service write access to its own source.
- **`EnvironmentFile=/etc/platformz.env`** (added below, for the join key) still
  works: systemd reads the root-only file itself before dropping privileges.

After a match ends, confirm the scoreboard landed:

```bash
ls -l /var/lib/platformz/scores
journalctl -u platformz | grep Scoreboard   # names loaded, and from where
```

### Optional: require a join key (recommended once the IP is public)

Port 9000 answers anyone by default — scanners can claim slots, and the
lowest-slot stranger becomes host. Setting `PLATFORMZ_KEY` in the server's
environment closes the door: joins without the key get **no reply at all**
(to a scanner the port looks dead). The key lives only on the box — never in
the repo.

```bash
install -m 600 /dev/null /etc/platformz.env
echo 'PLATFORMZ_KEY=pick-something-url-safe' > /etc/platformz.env
```

Add one line to the `[Service]` section of the unit above, then restart:

```
EnvironmentFile=/etc/platformz.env
```

```bash
systemctl daemon-reload && systemctl restart platformz
journalctl -u platformz | tail   # should show "Join key: REQUIRED"
```

How players present the key (it always travels inside the invitation):

- **Browser friends:** put it in the link you send —
  `https://yourdomain.com/platformz.html?key=pick-something-url-safe`. The
  page forwards it onto the WebSocket URL automatically.
- **Native handout builds:** bake it next to the host via the gitignored
  `secrets.mk` (see the note at the top of the repo `Makefile`), then
  `make` — the app connects with the key without the player seeing anything.
- **Terminal/testing:** append it to the URL arg:
  `./platformz "udp://yourdomain.com:9000?key=pick-something-url-safe"`.

Keep the key URL-safe (letters, digits, dashes). To rotate it: change the
file, restart the service, send fresh links. This is a friends-and-family
gate, not real security — anyone holding an invite can share it.

### Inviting someone to a specific room

The key gets people onto the **server**. `?match=CODE` gets them into a
particular **room** on it, which is what an invite-only room needs — it is hidden
from FIND A MATCH, so its 4-character code is the only way in.

The lobby shows the code and has a **COPY INVITE** button. In a browser that
copies the whole link, key and all; natively it copies the code, since there is
no link to hand out.

- **Browser:** `https://yourdomain.com/platformz.html?key=KEY&match=CODE`
- **Native, baked-in server:** `./platformz --match CODE`
- **Native, explicit server:** `./platformz "udp://yourdomain.com:9000?key=KEY&match=CODE"`

All three join on connect instead of idling in whatever room the server parked
you in. A code that has been reaped (rooms are destroyed 30 s after emptying)
comes back as a refusal on the browser screen, not a hang.

### The key and the room code are different things — keep them that way

They look alike (a short string in a URL) and they are not remotely the same
gate. Conflating them is the mistake this section exists to prevent.

| | `PLATFORMZ_KEY` | A room's 4-char code |
|---|---|---|
| Answers | "may you speak to this process at all" | "may you enter this room" |
| Checked at | WS upgrade / UDP hello, before anything else | `join`, after you are already connected |
| On failure | **no reply at all** — the port looks dead | a `joinfail` you can read on screen |
| Scope | the whole server | one room, for as long as it exists |
| Who sets it | the operator, once, in `/etc/platformz.env` | minted by the server when a private room is made |

Both can be on at once, and that is the interesting part: **there is no
configuration fork between "friends only" and "public".**

- **Friends-only (today).** Set `PLATFORMZ_KEY`. Only people holding it reach the
  server at all, so the match browser is only ever seen by people you invited.
  Room codes still work inside that, for a private room within the group.
- **Public.** Unset `PLATFORMZ_KEY` and restart. Anyone can connect and browse;
  private rooms are hidden from the list and their codes become the real
  invite mechanism. Nothing else changes — same binary, same rooms, same client.

Going public is a one-line change, which is exactly why the abuse limits below
are not optional.

### Abuse limits, and the two knobs that tune them

The server enforces these with no configuration at all; they are listed so a
refusal in the log is recognisable rather than mysterious.

| Limit | Default | Refusal |
|---|---|---|
| Concurrent rooms | 12 (`MATCH_MAX_CONCURRENT`) | `server_full` on create |
| Rooms one **address** may mint | 3, one back every 2 min | `server_full` on create |
| Room moves (join / quick / leave) | 5 in hand, 1/s | `rate_limited` |
| Wrong room codes | 5 per minute per connection | `rate_limited` |
| Match-list replies | 3 in hand, 1/s | *silently dropped* |
| UDP handshake | must echo a cookie (E1) | a challenge, and nothing else |
| Live matches at once | off (= the room cap) | the start is **held**, not refused |

Two are tunable, both for real scenarios rather than for tinkering:

```bash
# Your players share a NAT - a LAN party, an office, a household - so they all
# look like one address to the room-creation budget. Raise it, or 0 to disable.
PLATFORMZ_MAX_ROOMS_PER_ADDR=8

# The transfer graph, not the tick time, is this box's ceiling: a full match
# costs ~310 KB/s. Cap how many may run at once. A start that exceeds it is
# HELD - the room waits in its lobby and begins when a live match ends - so
# nobody's button press is lost and the client needs no new error to explain.
PLATFORMZ_MAX_ACTIVE=6
```

Both go in `/etc/platformz.env` beside the key. `GET /status` reports
`maxMatches`, `maxActive` and the live counts, so you can check what a running
server actually thinks its limits are.

**Nothing here ever hangs up on a player.** A full room does not end a
connection: you stay connected with no slot, the browser shows that room as 8/8,
and you pick another — or wait, and your client takes the next seat that frees up
on its own. That was a real bug once (the server sent a "full" packet and dropped
the socket, which over UDP is indistinguishable from the server being gone) and
it is the thing most worth re-checking if connection behaviour ever looks odd.

## 7. Serve the web client over HTTP

The `web/` files are already built (`make web` output). Point nginx's default site
at them:
```bash
rm -f /var/www/html/index.nginx-debian.html
cp /opt/PLATFORMZ/web/platformz.* /var/www/html/
systemctl restart nginx
```
Page is now at `http://SERVER_IP/platformz.html`. It loads over **http** and
auto-connects to `ws://<same-host>:9000`, so no `?server=` is needed and there's no
mixed-content block.

## Health check: `GET /status`

The game port answers a plain HTTP GET as well as WebSocket upgrades, so you can
check a running server without an SSH session:

```bash
curl http://SERVER_IP:9000/status
```
```json
{"uptime":8412,"matches":3,"active":1,"players":5,"maxMatches":12,
 "maxActive":12,"maxPlayers":8,"stateTag":9,"welcomeTag":10,
 "egressBytes":91442310}
```

| Field | Means |
|---|---|
| `uptime` | seconds since boot — answers *"did it restart?"* |
| `matches` | rooms that exist right now |
| `active` | how many of those are in countdown or playing — **the number that costs bandwidth** |
| `players` | connected humans across every room |
| `maxMatches` / `maxActive` | the caps this binary is running with, including any `PLATFORMZ_MAX_ACTIVE` override |
| `maxPlayers` | slots per room |
| `stateTag` / `welcomeTag` | the protocol versions the running binary actually speaks |
| `egressBytes` | bytes this process has put on a socket since boot |

`stateTag`/`welcomeTag` are the quickest answer to *"is this the build I
deployed?"*, which has caught a stale server here before — CI now asserts them
against `netbin.h` on every push, but the endpoint is how you check the box.

`egressBytes` is the one to watch over time. Divide the delta by the elapsed
seconds and compare against the ~325 KB/s a full match costs
([`perf-measurements.md`](perf-measurements.md)); against a 2 TB/month plan the
budget is about **770 KB/s sustained**, so `active` is the number that decides
whether you are inside it:

```bash
# rough KB/s over a minute, from the outside
a=$(curl -s "http://SERVER_IP:9000/status" | grep -o '"egressBytes":[0-9]*' | cut -d: -f2)
sleep 60
b=$(curl -s "http://SERVER_IP:9000/status" | grep -o '"egressBytes":[0-9]*' | cut -d: -f2)
echo $(( (b - a) / 60 / 1024 )) KB/s
```

**It respects the join key.** With `PLATFORMZ_KEY` set the endpoint is as silent
as everything else, so a scanner still sees a dead port; pass `?key=...` to reach
it. Nothing from the request is echoed back into the response.

## 8. Connect and play

- **Browser:** open `http://SERVER_IP/platformz.html`. Press a key on the title
  screen, then **click the canvas** (pointer lock + audio need a gesture).
- **Native (each player, on their Mac):**
  ```bash
  ./platformz udp://SERVER_IP:9000     # lower latency
  ./platformz ws://SERVER_IP:9000      # or WebSocket
  ```
- Watch `journalctl -u platformz -f` — `players` climbs as people join. Browser and
  native players share one room, on either transport.

### What you are looking at: several rooms, one server

The server holds up to **12 rooms**, each a whole match with its own world,
roster and options, all ticked on one 60 Hz beat. Two exist from boot:

```
Match registry: default room 7CGD (cap 12)
Match registry: official room A6SS preset=DEFAULT (locked, auto-starts at 2 players)
```

- The **default room** is where a connection with no opinion lands, and is never
  reaped.
- An **official room** has locked options and starts itself once two humans are
  in it — so somebody arriving alone on a quiet server still gets a game. QUICK
  MATCH aims here, and the server mints another when they are all busy.
- Players make their own rooms with CUSTOM MATCH. Those are **custom**: the
  creator owns the options and the START button. Private ones are hidden from the
  browser, so their 4-character code is the only way in.
- An empty room is destroyed after a grace — 30 s from a lobby, 60 s if it was
  mid-match, so a room-wide network blip does not tear down a live game. The
  default and official rooms are pinned and never reaped.

The heartbeat lists them every ten seconds, which is usually all the monitoring
you need:

```
tick 3600  matches 4 (2 active)  players 7  worst 0.31ms
    7CGD  lobby    slots 1/8  asteroids 0
    A6SS  playing  slots 4/8  asteroids 18
```

`worst` is the slowest tick that second across **every** room. On the $6 box a
full match costs ~0.6 ms against a 10 ms budget, so this stays small long after
bandwidth has become the real limit — see the capacity note below.

### How many matches this box can actually carry

Measured, not estimated ([`perf-measurements.md`](perf-measurements.md)): a full
8-player match costs **~325 KB/s** and about **0.6 ms** of tick.

**CPU is not the constraint. Transfer is, by roughly 7×.** Twelve simultaneously
full matches would be ~10 TB/month against the $6 plan's 2 TB, while the tick
budget would still be nearly empty. So:

- **12 rooms is fine** — most sit in a lobby costing almost nothing.
- **~6 simultaneously *playing* matches** is what 2 TB/month actually pays for.

`PLATFORMZ_MAX_ACTIVE=6` caps that, and a start beyond the cap is **held** — the
room waits in its lobby and begins when a live match ends, so nobody's button
press is lost. It ships **off**, because the right number depends on your plan
and on what else shares it. Watch `active` and `egressBytes` on `/status` for a
week before choosing one.

---

## Hand out a native binary with the address baked in

So recipients just run `./platformz` — no URL to type. The address is a **build-time**
setting (`PLATFORMZ_DEFAULT_SERVER_HOST` in `constants.h`, default empty), baked in via
the `dist` Makefile target; a plain `make` stays a dev build (unless `secrets.mk`
defines the host — see below).

| build command | `./platformz` (no arg) | `./platformz local` | `./platformz <url>` |
| --- | --- | --- | --- |
| `make` (dev) | local single-player | single-player | networked to `<url>` |
| `make dist HOST=SERVER_IP` (release) | auto-connect (UDP, pivots to WS) | single-player | networked to `<url>` (no pivot) |

**Build the distribution binary** (swap in your Vultr IP):
```bash
make dist HOST=203.0.113.10
```
This forces a full rebuild and passes `-DPLATFORMZ_DEFAULT_SERVER_HOST="203.0.113.10"`
to `g++` under the hood (see the `dist` target in the `Makefile` if you want the raw
`EXTRA_CXXFLAGS` form instead). Equivalent alternative if you'd rather not deal with
the Makefile target at all: edit the `PLATFORMZ_DEFAULT_SERVER_HOST` default in
`constants.h` to `"203.0.113.10"`, then a plain `make`. (Port defaults to 9000;
override with `make dist HOST=203.0.113.10 PORT=9000` if you moved it.)

If you also have a `secrets.mk` defining `PLATFORMZ_DEFAULT_SERVER_KEY` (see the
note at the top of the repo `Makefile`), `make dist` bakes that in automatically
alongside the host/port — no extra flags needed. The same goes for
`PLATFORMZ_DEFAULT_SERVER_HOST`: with the host in `secrets.mk`, a plain `make dist`
(no `HOST=`) is all you need; passing `HOST=` on the command line overrides it.

**What the baked binary does:** connects over **UDP** first, and if the UDP handshake
gets no reply within ~3s (a network blocking UDP), it logs `falling back to
WebSocket` and switches to `ws://SERVER_IP:9000` — so it works even where UDP is
filtered. `./platformz local` is the single-player escape hatch.

> The native binary is **macOS-only** (Homebrew build), so this is for handing to
> other Mac users. Everyone else uses the browser client (Step 7). Rebuild per code
> change like any native build.

`make dist` produces a bare `./platformz` for your own use. To hand something to
someone else, use `make dist-pack` — see the next section.

---

## Hand out a signed, notarized `PLATFORMZ.app`

`make dist-pack` produces `dist/PLATFORMZ-mac-arm64.zip`: a real `.app` bundle,
signed with Developer ID, notarized by Apple and stapled. The recipient downloads
it, double-clicks, and plays. No "unidentified developer" warning, no right-click →
Open, no `xattr` incantation.

**Requirements:** Apple Silicon, **macOS 13.0 (Ventura) or later**.

### One-time setup

**1. Build raylib from source.** Homebrew's raylib bottle is compiled with a
**15.0** deployment target, which would lock the handout to Sequoia. Build it
against 13.0 instead — into its own directory, *not* `~/raylib` (that's the
Emscripten/WebGL1 build `make web` uses):

```bash
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git ~/raylib-macos13
cmake -S ~/raylib-macos13 -B ~/raylib-macos13/build-mac \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF
cmake --build ~/raylib-macos13/build-mac -j8
otool -l ~/raylib-macos13/build-mac/raylib/libraylib.a | grep -m1 -A3 LC_BUILD_VERSION
#   expect: minos 13.0
```

(`CMAKE_POLICY_VERSION_MINIMUM` is needed because Homebrew's cmake is 4.x, which
rejects the older policy versions raylib's bundled GLFW declares.)

**2. Store notarization credentials.** This needs a secret typed in, so it can't
live in the repo. Generate an app-specific password at
<https://account.apple.com> → Sign-In and Security → App-Specific Passwords, then:

```bash
xcrun notarytool store-credentials "platformz-notary"
#   Apple ID: mike@michaelmacallister.com
#   Team ID:  9WM486296X
#   Password: <the app-specific password>
```

Don't pass `--password` on the command line — it would land in your shell history.
Verify with `xcrun notarytool history --keychain-profile platformz-notary`.

**3. Keep Dropbox off the build output.** The repo lives in Dropbox, and Dropbox
rewriting file metadata between `codesign` and `stapler staple` will silently
invalidate the signature. Run once:

```bash
xattr -w com.dropbox.ignored 1 dist
```

### The targets

| command | does | needs credentials |
| --- | --- | --- |
| `make app` | unsigned `dist/PLATFORMZ.app` | no |
| `make pack-unsigned` | zips it as `…-UNSIGNED-…` for local testing | no |
| `make sign` | Developer ID + hardened runtime + secure timestamp | Developer ID cert |
| `make notarize` | submits to Apple, waits, staples the ticket | notarytool profile |
| `make dist-pack` | the whole chain → `dist/PLATFORMZ-mac-arm64.zip` | both |

Notarization hits Apple's servers and usually takes 1–5 minutes. If it's rejected,
`xcrun notarytool log <submission-id> --keychain-profile platformz-notary` gives the
exact reason — rejections are almost always mechanical (missing hardened runtime,
missing timestamp), not a judgement about the code.

Server host and join key bake in from `secrets.mk` exactly as they do for `make dist`.

### Proving it actually works

Local checks (`spctl`, `stapler validate`) pass on **your** machine even for an
unsigned build, because your Mac trusts your own certificate. The only test that
counts uses a real quarantine flag, which only a browser, Mail, Messages or AirDrop
applies — **not** `curl`, `scp`, or a USB stick:

1. Download the zip through Safari/Chrome on a **different Mac** (second best: a
   freshly created user account, which defeats the per-user Gatekeeper cache).
2. Confirm the flag is there: `xattr -p com.apple.quarantine ~/Downloads/PLATFORMZ-mac-arm64.zip`
3. Unzip and double-click. Expect **at most one** "downloaded from the Internet,
   are you sure you want to open it?" dialog **with an Open button** — that one is
   normal for any downloaded app, first launch only. The failure dialog is the one
   with *no* Open button.
4. Turn Wi-Fi off before first launch and repeat. Stapling is what makes this work
   offline; without it, first launch needs a round-trip to Apple that can fail.

### Caveats

- **Apple Silicon only.** Intel Macs get "not supported on this type of Mac". A
  universal build is possible (`-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` plus
  `-arch` flags on our objects) but nobody has needed it yet.
- **The Developer ID certificate expires 2027-02-01.** Builds notarized before then
  keep launching forever — `--timestamp` embeds a countersignature and Gatekeeper
  judges the cert as of that moment. What expires is the ability to sign *new*
  builds. Renew in the Developer portal.
- **Notarization needs an active paid Developer Program membership** at submit time.
  Already-stapled builds keep working if it lapses.
- **Console output disappears** when launched from Finder. Anyone who needs
  connection logs runs `PLATFORMZ.app/Contents/MacOS/platformz` from Terminal —
  which is also the only way to pass `local` or an explicit server URL, since a
  double-clicked `.app` takes no arguments.

---

## What the server keeps on disk

Worth stating plainly, because this doc used to say the opposite. Earlier versions
asserted the server "reads no files, writes no files" — that was true once, and
the scoreboard made it false. **Two things on the box are not in git and cannot be
rebuilt from it.**

| Path | What it is | If you lose it |
|---|---|---|
| `/var/lib/platformz/scores` | The all-time scoreboard: one `<score>\t<id>\t<name>` line per row, flushed a few seconds after each match end and on shutdown | Every player's cumulative score is gone. The server starts a fresh board and logs that it loaded nothing; nothing else breaks |
| `/etc/platformz.env` | `PLATFORMZ_KEY`, the join gate, and `PLATFORMZ_IDENTITY_SECRET`, the key behind every tag the server issues | Existing invite links and baked handout builds stop working, because the key they carry no longer matches — reissue links and rebuild handouts. Losing the identity secret costs one round of UDP handshakes now, and (once D3 lands) everyone's remembered identity |

Everything else — the binary, the web bundle, the systemd unit, the Caddy config
— is either in the repo or reproducible from the steps above.

The scoreboard is written atomically (to `scores.tmp`, then renamed), so an
interrupted save or a crash mid-write cannot leave a half-file; you either get the
previous board or the new one. That protects against corruption, **not** against
the disk going away.

### Backing them up

Small, plain text, and rarely changing — so this is a one-liner, not a strategy:

```bash
# on the box, as root (/etc/platformz.env is root-only by design).
# -C / keeps the paths relative inside the archive, so tar does not warn about
# stripping leading slashes and the restore below lands them back where they were.
tar czf "/root/platformz-state-$(date +%F).tgz" -C / \
    var/lib/platformz/scores etc/platformz.env

# restore
tar xzf platformz-state-YYYY-MM-DD.tgz -C /
systemctl restart platformz
```

Then copy the archive **off the instance** — a backup that only exists on the box
is not a backup of the box. Run it before a redeploy that changes the scoreboard
format, and before destroying or resizing the instance.

### The identity secret

`PLATFORMZ_IDENTITY_SECRET` is the key behind every tag the server issues: E1's
UDP handshake cookie and D3's identity token. It lives in `/etc/platformz.env`
beside the join key:

```bash
# on the box, as root. 32 random bytes, hex, no shell-special characters.
printf 'PLATFORMZ_IDENTITY_SECRET=%s\n' "$(openssl rand -hex 32)" >> /etc/platformz.env
systemctl restart platformz
journalctl -u platformz | grep -i 'handshake cookie\|IDENTITY_SECRET'
```

A good boot prints `UDP handshake cookie: ON` and nothing else. If it also prints
`WARNING: PLATFORMZ_IDENTITY_SECRET unset`, the server minted a random one for
this boot and the line above did not take.

**It must survive restarts.** A per-boot secret is harmless for E1 — clients
whose cookie went stale across the restart just take one extra handshake round
trip. For D3 it is not: every identity token in every player's profile stops
verifying, so each returning player is issued a fresh one and looks like somebody
new. Nothing breaks — a client with a stale token is re-issued and plays exactly
as before — but continuity is lost, and once the scoreboard is keyed on identity
(D4) that means everybody's history resets on every deploy. It is also why it
belongs in the backup above.

**Rotating it deliberately** does the same thing on purpose: every player becomes
new. That is the reset switch if you ever need one, and the reason not to touch
the line otherwise.

Unlike `PLATFORMZ_KEY`, this one is **never** shared with players: it does not
ride in invite links or handout builds, and nothing a client sends should ever
contain it. Losing it costs a restart's worth of handshakes and (later) everyone's
remembered identity; leaking it lets anyone mint cookies, which puts the
reflection hole back.

### One-time: the scoreboard format changed (D4)

The table used to be keyed on **display name**, so two players who both typed
`MIKE` shared a row and anyone could claim someone else's by typing their name.
It is keyed on the server-issued identity now, and the file grew a column:

```
before   <score>\t<name>
after    <score>\t<id>\t<name>
```

**Nothing is required.** Old lines have two fields, so the loader skips them and
says so once at boot, and the first match to credit rewrites the file wholesale in
the new format. The old rows go away on their own.

Deleting it just skips that, and keeps the boot log quiet in the meantime:

```bash
systemctl stop platformz
rm -f /var/lib/platformz/scores /var/lib/platformz/scores.tmp
systemctl start platformz
journalctl -u platformz | grep scoreboard   # "no file at ... - starting empty"
```

**Stop the server first.** A running server holds the table in memory and rewrites
the file at the next match end, so deleting it underneath a live process achieves
nothing. (`mv` instead of `rm` if you want to keep the old numbers to look at —
nothing reads them either way.)

Leave the file alone and you will see this on every restart until a match
credits, which is the only cost:

```
[scoreboard] ignored 102 pre-D4 line(s) from /var/lib/platformz/scores - ...
```

A row belongs to an identity, and identities come from
`PLATFORMZ_IDENTITY_SECRET`. **Rotate that and every player becomes a new row** —
see [The identity secret](#the-identity-secret) above.

## Redeploying after code changes

- **Server:** commit + push, then on the box: `cd /opt/PLATFORMZ && git pull` →
  `make -C server` → `systemctl restart platformz`. Then confirm the deploy
  actually took: `journalctl -u platformz -n 20` prints a `Protocol:` line with
  the wire tags and quantizer ranges the running binary was built with, and those
  must match `netbin.h` in your clients' build. A mismatch means clients get
  SERVER VERSION MISMATCH instead of connecting.
- **Web:** rebuild on your Mac (`make web RAYLIB_WEB_DIR=$HOME/raylib`), commit +
  push the regenerated `web/platformz.*` (they're tracked), then on the box:
  `git pull` and copy them to **whichever directory is actually serving the page**:
  - after the HTTPS upgrade (Caddy — the normal state of a public box):
    `cp /opt/PLATFORMZ/web/platformz.* /var/www/platformz/`
  - on the plain-HTTP setup from step 7:
    `cp /opt/PLATFORMZ/web/platformz.* /var/www/html/`

  This step used to name only `/var/www/html/` — right for plain HTTP, but
  **not** where Caddy serves from (see "HTTPS upgrade" below). On an HTTPS box
  the copy succeeded, changed nothing anyone could see, and the site kept serving
  a month-old build: one that predated the lobby entirely and, after the
  welcome-version bump, showed every browser visitor SERVER VERSION MISMATCH.
  Copying to the wrong directory raises no error, so **check the copy landed**:

  ```bash
  # the wasm the site serves must be the one you just built - same hash
  curl -s https://yourdomain.com/platformz.wasm | shasum
  shasum /opt/PLATFORMZ/web/platformz.wasm
  ```
- **The box holds state a `git pull` will not restore** — see "What the server
  keeps on disk" above. `git clean` inside `/opt/PLATFORMZ` is safe today only
  because the scoreboard lives in `/var/lib/platformz`, which is exactly why the
  systemd unit puts it there.

## Caveats while on plain IP/HTTP

- **Pointer lock / "Not secure".** Browsers flag http as not secure, and a few
  tighten pointer-lock on insecure origins. If look-controls or audio misbehave in
  someone's browser, that's the cue to do the HTTPS upgrade — not a code bug.
- **The GitHub Pages client won't work with this server.** Pages is https, and an
  https page is blocked from talking to `ws://` (it needs `wss://`). Use the
  VPS-hosted `http://SERVER_IP/platformz.html`, not the Pages link.

## HTTPS upgrade (later)

Point an A record at `SERVER_IP`, then install **Caddy** — it auto-provisions
Let's Encrypt TLS, serves `web/` over https, and reverse-proxies the WebSocket.
The web client's built-in default matches this layout: on an https page it
connects to `wss://<the page's host>/ws` (see `main.cpp`), so the proxy must
route `/ws` to the game server. Plain-http/LAN pages keep the old
`ws://host:9000` default — nothing changes for local testing.

```
apt install -y caddy
systemctl stop nginx && systemctl disable nginx   # Caddy takes over :80/:443
ufw allow 443/tcp                                 # step 5 never opened https
```

(If a Vultr cloud firewall is attached, add an inbound TCP 443 rule there too.)

The apt package ships an example `/etc/caddy/Caddyfile` (comment header plus a
`:80` site block) — **replace the whole file**, don't add to it. This is the
complete config, nothing else needed; certificates are automatic. Swap in your
domain and run:

```bash
cat >/etc/caddy/Caddyfile <<'EOF'
yourdomain.com {
    encode zstd gzip          # the ~13 MB .wasm/.data payload is the whole first load
    root * /var/www/platformz
    file_server

    reverse_proxy /ws localhost:9000
}
EOF
```

Deploy the web build to `/var/www/platformz`
(`mkdir -p /var/www/platformz && cp /opt/PLATFORMZ/web/platformz.* /var/www/platformz/`),
then `systemctl reload caddy`. Checks:

- `https://yourdomain.com/platformz.html` loads and auto-connects (no
  `?server=` needed — the default is scheme-aware).
- Native handout builds can bake the domain instead of an IP
  (`PLATFORMZ_DEFAULT_SERVER_HOST="yourdomain.com"`) — both transports resolve
  hostnames. `udp://yourdomain.com:9000` bypasses the proxy entirely, as
  before, and the ws:// fallback pivot also connects direct to :9000 (the
  proxy only matters for browsers, which need wss on a secure page).
- Your GitHub Pages client works again via `?server=wss://yourdomain.com/ws`.

Remember the web bundle must be rebuilt (`make web`) whenever `main.cpp`
changes — the scheme-aware default is baked into the wasm.

## See also
- `docs/multiplayer-testing.md` — local/LAN testing (native + browser).
- `server/Makefile` — the Linux server build (Boost + stub, no raylib).
