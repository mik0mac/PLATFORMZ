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

> Below, `SERVER_IP` = the VPS's public IP.

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
apt install -y build-essential libboost-dev libboost-system-dev git nginx rsync
```
`build-essential` → `g++`/`make`; `libboost-system-dev` is what the server links
(`-lboost_system`); `nginx` serves the web page. **No raylib needed** — the server
compiles against `server/raylib_server_stub.h` (math types only).

## 3. Get the code onto the box

**Option A — deploys exactly what's on your Mac, including uncommitted work.** Run
*from your Mac* (quote the Dropbox path — it has a space):
```bash
rsync -avz --exclude '.git' \
  "/Users/michaelmacallister/Dropbox (Personal)/VS_CODE/PLATFORMZ/" \
  root@SERVER_IP:/opt/PLATFORMZ/
```

**Option B — clone (commit + push your branch first).** On your Mac:
`git add -A && git commit && git push -u origin <your-branch>`, then on the box:
```bash
git clone -b <your-branch> https://github.com/mik0mac/PLATFORMZ.git /opt/PLATFORMZ
```

> A plain `git clone` pulls `main`. If your latest work is on a feature branch,
> clone that branch (Option B) or use rsync (Option A), or the server will be built
> from stale code.

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

- **Server:** rsync/`git pull` the new code → `make -C /opt/PLATFORMZ/server` →
  `systemctl restart platformz`.
- **Web:** rebuild on your Mac (`make web RAYLIB_WEB_DIR=$HOME/raylib`), rsync the new
  `web/platformz.*` up, then `cp /opt/PLATFORMZ/web/platformz.* /var/www/html/`.

## Caveats while on plain IP/HTTP

- **Pointer lock / "Not secure".** Browsers flag http as not secure, and a few
  tighten pointer-lock on insecure origins. If look-controls or audio misbehave in
  someone's browser, that's the cue to do the HTTPS upgrade — not a code bug.
- **The GitHub Pages client won't work with this server.** Pages is https, and an
  https page is blocked from talking to `ws://` (it needs `wss://`). Use the
  VPS-hosted `http://SERVER_IP/platformz.html`, not the Pages link.

## HTTPS upgrade (later)

Point an A record at `SERVER_IP`, then install **Caddy** — it auto-provisions Let's
Encrypt TLS and reverse-proxies `wss://yourdomain/…` → `ws://localhost:9000` while
serving `web/` over https. Native `udp://yourdomain:9000` keeps working directly
(UDP bypasses the proxy). Your GitHub Pages client then works again via
`?server=wss://yourdomain`.

## See also
- `docs/play-web-via-github.md` — the GitHub Actions + cloudflared tunnel setup.
- `docs/multiplayer-testing.md` — local/LAN testing (native + browser).
- `server/Makefile` — the Linux server build (Boost + stub, no raylib).
