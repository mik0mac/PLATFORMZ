# Deploy PLATFORMZ on a Vultr VPS (IP-only)

A persistent, self-hosted alternative to the GitHub Actions + cloudflared tunnel
(`docs/play-web-via-github.md`). Runs the authoritative server on a Vultr VPS with
a stable public IP, reachable over **both** WebSocket (TCP) and UDP, and serves the
browser client from the same box over plain HTTP.

This guide is **IP-only** (no domain, no TLS) — the quickest thing that works while
testing. The [HTTPS upgrade](#https-upgrade-later) at the bottom is the "proper"
persistent setup for later.

## What goes where

- **On the VPS:** the headless **game server** (`gameserver`) and the **web client
  files** (`web/platformz.*`). The server reads no files at runtime (pure in-memory),
  so nothing else needs uploading.
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
root — the process is pure in-memory (reads no files, writes no files, and port
9000 doesn't need privileges), so it costs nothing and a compromised server
can't touch the box. The `EnvironmentFile` line below still works with it:
systemd reads the root-only key file itself before dropping privileges.

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

## 8. Connect and play

- **Browser:** open `http://SERVER_IP/platformz.html`. Press a key on the title
  screen, then **click the canvas** (pointer lock + audio need a gesture).
- **Native (each player, on their Mac):**
  ```bash
  ./platformz udp://SERVER_IP:9000     # lower latency
  ./platformz ws://SERVER_IP:9000      # or WebSocket
  ```
- Watch `journalctl -u platformz -f` — `players` climbs as people join. Browser and
  native players share one match.

---

## Hand out a native binary with the address baked in

So recipients just run `./platformz` — no URL to type. The address is a **build-time**
setting (`PLATFORMZ_DEFAULT_SERVER_HOST` in `constants.h`, default empty). The `-D`
flag is what makes it a **distribution** build; a plain `make` with no flag stays a
dev build.

| build command | `./platformz` (no arg) | `./platformz local` | `./platformz <url>` |
| --- | --- | --- | --- |
| `make` (dev, no `-D`) | local single-player | single-player | networked to `<url>` |
| `make EXTRA_CXXFLAGS='-DPLATFORMZ_DEFAULT_SERVER_HOST=\"SERVER_IP\"'` (release) | auto-connect (UDP, pivots to WS) | single-player | networked to `<url>` (no pivot) |

**Build the distribution binary** (swap in your Vultr IP; the `\"` escaping matters —
it's how the string survives `make` → shell → `g++`):
```bash
make EXTRA_CXXFLAGS='-DPLATFORMZ_DEFAULT_SERVER_HOST=\"203.0.113.10\"'
```
Equivalent alternative if you'd rather not deal with quoting: edit the
`PLATFORMZ_DEFAULT_SERVER_HOST` default in `constants.h` to `"203.0.113.10"`, then a
plain `make`. (Port defaults to 9000; override with
`-DPLATFORMZ_DEFAULT_SERVER_PORT=\"9000\"` if you moved it.)

**What the baked binary does:** connects over **UDP** first, and if the UDP handshake
gets no reply within ~3s (a network blocking UDP), it logs `falling back to
WebSocket` and switches to `ws://SERVER_IP:9000` — so it works even where UDP is
filtered. `./platformz local` is the single-player escape hatch.

> The native binary is **macOS-only** (Homebrew build), so this is for handing to
> other Mac users. Everyone else uses the browser client (Step 7). Rebuild per code
> change like any native build.

---

## Redeploying after code changes

- **Server:** commit + push, then on the box: `cd /opt/PLATFORMZ && git pull` →
  `make -C server` → `systemctl restart platformz`.
- **Web:** rebuild on your Mac (`make web RAYLIB_WEB_DIR=$HOME/raylib`), commit +
  push the regenerated `web/platformz.*` (they're tracked), then on the box:
  `git pull` and `cp /opt/PLATFORMZ/web/platformz.* /var/www/html/`.

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

`/etc/caddy/Caddyfile` (replace `yourdomain.com`; that's the whole config —
certificates are automatic):

```
yourdomain.com {
    encode zstd gzip          # the ~13 MB .wasm/.data payload is the whole first load
    root * /var/www/platformz
    file_server

    reverse_proxy /ws localhost:9000
}
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
- `docs/play-web-via-github.md` — the GitHub Actions + cloudflared tunnel setup.
- `docs/multiplayer-testing.md` — local/LAN testing (native + browser).
- `server/Makefile` — the Linux server build (Boost + stub, no raylib).
