# Play the PLATFORMZ web client via GitHub

Run the whole thing from GitHub — no local machine needed to host:

- **Server** runs on a GitHub Actions runner (`.github/workflows/gameserver.yml`)
  and is exposed to the internet by a **cloudflared tunnel** that prints a public
  `wss://…trycloudflare.com` URL.
- **Web client** (the WASM build in `web/`) is served by **GitHub Pages**.
- You connect the Pages page to the tunnel with a `?server=` query string.

> The server supports **2 player slots**. The tunnel URL is new every run and the
> job auto-stops after 6 hours (`timeout-minutes: 360`).

---

## One-time setup (skip if already done)

- **Step 0a.** Make sure your latest commits are pushed to GitHub
  (`git push`) — Actions builds/runs whatever is on the branch, and the manual
  "Run workflow" button only appears for workflows on the **default branch**.
- **Step 0b.** Enable GitHub Pages: repo **Settings → Pages → Build and
  deployment → Source: "Deploy from a branch" → Branch: `main`, Folder: `/ (root)`
  → Save**. After it publishes, your client URL is:
  `https://mik0mac.github.io/PLATFORMZ/web/platformz.html`

---

## Start it up

- **Step 1.** On GitHub, open the **Actions** tab.
- **Step 2.** In the left sidebar click **"Run Game Server"**, then the
  **"Run workflow"** button (top right) → **Run workflow**.
- **Step 3.** Click into the new run, then the **`serve`** job, to watch its log.
- **Step 4.** Wait ~30–60s for the **"Start server and tunnel"** step to print a
  ready-to-use **PLAY** link (the tunnel URL is already baked into `?server=`):
  ```
  ============================================
  TUNNEL URL: https://random-words.trycloudflare.com
  WebSocket:  wss://random-words.trycloudflare.com

  PLAY (no install - opens in a browser):
    https://mik0mac.github.io/PLATFORMZ/web/platformz.html?server=wss://random-words.trycloudflare.com
  ============================================
  ```
- **Step 5.** **Copy the PLAY link.**

---

## Connect and play

No install needed — the link loads the WASM client from GitHub Pages and runs it
in the browser. Anyone with the link can play; only the server runs on GitHub.

- **Step 6.** Open the **PLAY link** from Step 5 in a browser.
- **Step 7.** Press a key on the title screen to start, then **click the black
  canvas** to capture the mouse (pointer lock) and unlock audio.
- **Step 8.** Confirm you're connected: the server log (still live in the Actions
  run) climbs `players 0 → 1`. If it stays on "CONNECTING TO SERVER…", re-check you
  copied the **whole** PLAY link including the `?server=wss://…` part.
- **Step 9.** **Second player:** open the **same** PLAY link in another browser,
  window, or device. The log should reach `players 2`.

---

## Controls
WASD move · mouse look · left-click fire rocket · Space jetpack (up) · hold
Left Shift for stronger (earth) gravity · Esc toggle cursor capture.

---

## Stop the server
- **Step 10.** Back in the Actions run, click **"Cancel workflow"** (top right) when
  you're done — otherwise it runs until the 6-hour timeout.

---

## Quick fixes
| Symptom | Fix |
| --- | --- |
| No "Run workflow" button | Workflow isn't on the default branch — `git push` it to `main`. |
| Page loads but "CONNECTING TO SERVER…" forever | Stale link. Each run makes a **new** tunnel — use the PLAY link from the **current** run's log (Step 5). |
| 404 for the client page | Pages not enabled/published yet, or wrong path — confirm Step 0b and use the `/web/platformz.html` path. |
| Blank canvas / no audio | Click the canvas once (pointer lock + audio need a user gesture). |

## See also
- `docs/multiplayer-testing.md` — local/LAN testing (native + browser) without GitHub.
- `.github/workflows/gameserver.yml` — the server + tunnel job.
