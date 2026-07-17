# Vultr server CLI cheatsheet

The commands you actually need on the PLATFORMZ box, plus a plain-language tour
of the Linux folder layout at the bottom. Replace `SERVER_IP` with the VPS's
public IPv4.

The box serves **platformz.space** over HTTPS via **Caddy** (not nginx — nginx was
stopped and disabled during the HTTPS upgrade on 2026-07-16). The gameserver runs
on :9000 for both WebSocket and UDP.

Everything here assumes you log in as `mik0mac` (see `docs/ssh-hardening.md`),
so admin commands are prefixed with `sudo`. Full deploy instructions live in
`docs/deploy-vultr.md` — this is the day-to-day reference.

## Getting in and out

```bash
ssh mik0mac@SERVER_IP     # log in (key-only, no password)
exit                      # log out (or Ctrl-D)
```

## Watching the gameserver log  ← the one you asked about

The server writes to systemd's journal, not a log file. To watch it live:

```bash
sudo journalctl -u platformz -f
```

`-u platformz` = only this service, `-f` = follow (keep printing new lines).
Ctrl-C to stop watching — that does **not** stop the server.

Other views of the same log:

```bash
sudo journalctl -u platformz -n 50          # last 50 lines, then quit
sudo journalctl -u platformz --since today  # today only ("1 hour ago" also works)
sudo journalctl -u platformz | grep -i key  # search the whole log
```

## Controlling the gameserver

```bash
sudo systemctl status platformz     # is it running? shows recent log lines too
sudo systemctl restart platformz    # restart (after a rebuild or config change)
sudo systemctl stop platformz       # stop
sudo systemctl start platformz      # start
sudo systemctl enable platformz     # start automatically on boot (already set)
sudo systemctl daemon-reload        # required after editing the .service file
```

The unit file lives at `/etc/systemd/system/platformz.service`.

## Redeploying after a code change

```bash
cd /opt/PLATFORMZ
sudo git pull                                  # pull your pushed commits
sudo make -C server                            # rebuild the gameserver
sudo systemctl restart platformz               # swap in the new binary
sudo journalctl -u platformz -f                # confirm it came back up
```

Web client only (after `make web` on your Mac, committed and pushed):

```bash
cd /opt/PLATFORMZ && sudo git pull
sudo cp /opt/PLATFORMZ/web/platformz.* /var/www/platformz/   # Caddy's web root
```

No Caddy restart needed — `file_server` reads the files off disk per request.
Hard-refresh the browser (Cmd-Shift-R) to get past the cached wasm.

## The web server (Caddy)

```bash
sudo systemctl status caddy --no-pager    # is it running?
sudo systemctl reload caddy               # after editing /etc/caddy/Caddyfile
sudo systemctl restart caddy              # heavier; reload is usually enough
sudo journalctl -u caddy -f               # its log, same pattern as the gameserver
sudo caddy validate --config /etc/caddy/Caddyfile   # syntax-check before reloading
```

Config is `/etc/caddy/Caddyfile`; web root is `/var/www/platformz`; TLS certs renew
themselves. **Always drive Caddy through `systemctl`** — never `caddy start` or
`caddy run` by hand (see the troubleshooting note below for why).

nginx is stopped and disabled on this box. If it ever comes back it will steal :80
from Caddy and break TLS renewal:

```bash
sudo systemctl is-enabled nginx    # want: disabled
```

## Is the site actually up?

```bash
curl -I https://platformz.space/platformz.html    # want: 200, no TLS warning
curl -I http://platformz.space/platformz.html     # want: 308 redirect to https
sudo caddy list-certificates                      # cert + expiry (renews automatically)
```

A page that loads only proves `file_server` works. To prove the whole path —
including the `/ws` proxy hop into the gameserver — open the invite link in a
browser and watch `sudo journalctl -u platformz -f`: `players` should climb as you
join. Invite links carry the join key from `/etc/platformz.env`:

```
https://platformz.space/platformz.html?key=THEKEY
```

The page forwards `?key=` onto the WebSocket URL itself, and Caddy passes the query
string through the proxy — so nothing extra is needed. Native players are
unaffected by Caddy entirely (`udp://platformz.space:9000?key=THEKEY` goes straight
to the gameserver), which makes native a useful way to tell "the web server is
broken" apart from "the gameserver is broken".

## Firewall

```bash
sudo ufw status                 # list the open ports
sudo ufw allow 9000/udp         # open a port
sudo ufw delete allow 9000/udp  # close it again
```

Remember there are two firewalls: `ufw` on the box, and (if you attached one)
the Vultr cloud firewall in the control panel. A port must be open in both.

## Is it actually listening?

```bash
sudo ss -tulnp | grep 9000              # what's bound to port 9000 (tcp + udp)
sudo ss -tulnp | grep -E ':(80|443)'    # what's serving the web
```

Expect `gameserver` on 9000 (both udp and tcp) and `caddy` on 80 + 443. Check a
port you *know* is busy in the same output — if `9000` is missing too, the command
isn't telling you what you think it is, and "nothing on :80" proves nothing.

## Troubleshooting a service that won't start

Hard-won on 2026-07-16, when Caddy had been dead for three days. Worth reading
before you debug any systemd service here.

**`status` on a failed unit is a snapshot from when it died, not from now.** It
replays the log from the moment of death and keeps showing it indefinitely. A
failed unit never retries on its own. So a port conflict from three days ago reads
exactly like a live one — we chased a "port :80 in use" error long after the thing
using :80 was gone. Check the timestamp on the log lines and confirm against
current reality (`ss`) before believing the error.

**`caddy start` is a trap.** It launches a *detached background daemon* that
outlives your terminal, and without `--config` it starts with an empty config: no
sites, no :80, no :443 — just the admin API on :2019. It looks like success and
serves nothing. Worse, it then blocks the real systemd unit from starting, which
fails with:

```
listen tcp 127.0.0.1:2019: bind: address already in use
```

**Port 2019 in that error means a second Caddy is already running**, not a config
problem. Find and kill the stray:

```bash
ps aux | grep '[c]addy'        # a "caddy run --pingback ..." line = someone ran `caddy start`
sudo caddy stop                # shut it down via its admin API
sudo ss -tulnp | grep 2019     # want: nothing
sudo systemctl start caddy     # now the real one can start
```

`--pingback` in the process list is the tell — nobody types that flag; `caddy start`
adds it. Once 2019 is clear, `systemctl start caddy` works.

**A failed cert is not a failed start.** Caddy starts first and fetches
certificates in the background, so a Let's Encrypt problem leaves the service
`active (running)` with no cert — it does *not* refuse to start. If the unit won't
start at all, look for a config or port problem, not DNS.

**Don't loop on restarts when TLS is involved.** Repeated failed ACME attempts
burn Let's Encrypt rate limit, and you can end up locked out of issuing a
certificate for hours. Read the log, fix the cause, start once.

## Files and moving around

```bash
pwd                     # where am I?
ls -la                  # list everything here, including hidden files
cd /opt/PLATFORMZ       # go to a folder ("cd" alone → your home folder)
cd ..                   # up one level
cat file.txt            # dump a file to the screen
less file.txt           # page through a big file (q to quit, / to search)
nano file.txt           # simple editor (Ctrl-O save, Ctrl-X exit)
cp a b                  # copy
mv a b                  # move/rename
rm a                    # delete
mkdir -p /var/www/foo   # make a folder (and any parents)
du -sh /opt/PLATFORMZ   # how big is this folder?
df -h                   # free disk space per drive
```

## Copying files between your Mac and the box

Run these **on your Mac**, not on the server:

```bash
scp file.txt mik0mac@SERVER_IP:/tmp/          # Mac → server
scp mik0mac@SERVER_IP:/tmp/file.txt .         # server → Mac
```

## Health and housekeeping

```bash
htop                        # live CPU/memory/process view (q to quit)
                            # not installed? sudo apt install -y htop
uptime                      # how long since the last reboot, plus load
free -h                     # memory used/free
sudo apt update && sudo apt upgrade -y   # security updates
sudo reboot                 # restart the box (systemd brings platformz back)
```

## Who's knocking on the door

```bash
sudo tail -f /var/log/auth.log            # live SSH login attempts
sudo grep 'Failed password' /var/log/auth.log | wc -l   # count bot attempts
```

---

# The Linux folder structure

Linux has **one** tree starting at `/` (the "root" of everything). There are no
drive letters like `C:` — extra disks get attached to a folder somewhere inside
that one tree. Every path starts from `/`.

**The folder you land in when you log in:** `/home/mik0mac` — your home folder,
also written `~`. Typing `cd` with nothing after it always brings you back here.
It's yours to write to without `sudo`, and it's the right place for scratch
files. Note that nothing about PLATFORMZ lives here — the code is in `/opt`.

(If you ever log in as root instead, root's home is `/root`, not `/home/root`.)

## The folders worth knowing

| Folder | What's in it |
| --- | --- |
| `/home/mik0mac` | Your home folder. Where you land, and `~` is shorthand for it. |
| `/opt` | Optional/third-party software installed by hand — **`/opt/PLATFORMZ` is your git clone**. |
| `/etc` | All configuration files. `platformz.service`, the Caddyfile, SSH settings, `/etc/platformz.env`. Editing anything here needs `sudo`. |
| `/var` | Data that changes as the box runs: **`/var/www/platformz`** (the web files Caddy serves), `/var/log` (log files like `auth.log`), the systemd journal. |
| `/tmp` | Scratch space, wiped on reboot. Anyone can write here. |
| `/usr/bin`, `/bin` | The programs themselves — `ls`, `git`, `make` are files in here. |
| `/root` | The root user's home folder. |

## Reading a path

`/opt/PLATFORMZ/server/gameserver` = start at the root of the tree → into `opt`
→ into `PLATFORMZ` → into `server` → the file `gameserver`. A path starting with
`/` is absolute (same meaning from anywhere). One that doesn't — `server/gameserver`
— is relative to whatever folder you're currently standing in.

## Permissions in one paragraph

Every file has an owner and a set of read/write/execute flags — that's what
`ls -la` is showing you in the `-rw-r--r--` column. Most of the system is owned
by `root`, so changing anything in `/etc`, `/opt`, or `/var` needs `sudo`
("do this as root"). Your home folder is the exception: it's yours already, so
no `sudo` needed there. If a command fails with "Permission denied", that's
almost always the answer — try it again with `sudo` in front.

## See also
- `docs/deploy-vultr.md` — the full deploy/build walkthrough.
- `docs/ssh-hardening.md` — key-only login setup.
