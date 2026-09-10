---
name: run-platformz
description: Launch and drive the native PLATFORMZ client on macOS - real clicks and key chords against the game window, screenshots you can read, and a local gameserver. Use when asked to run the app, screenshot it, or confirm a UI change works for real rather than only in tests.
---

# Running and driving PLATFORMZ

The game is a raylib/GLFW window. There is no accessibility tree and no DOM, so
driving it means **posting real HID events at screen coordinates and reading
screenshots**. Everything below is verified working, including the two dead ends
that cost the most time.

## 0. Permissions (check first, they are the usual blocker)

```bash
clang -O1 -framework ApplicationServices \
  -o "$SCRATCH/uidriver" .claude/skills/run-platformz/tools/uidriver.c
"$SCRATCH/uidriver" trusted        # 1 = can click/type, 0 = cannot
```

Two separate macOS grants, both for the terminal running Claude Code (usually
**Visual Studio Code**), in System Settings -> Privacy & Security:

| Grant | Without it |
|---|---|
| **Screen Recording** | `screencapture` returns the wallpaper and menu bar with **no windows** - looks like the app failed to open |
| **Accessibility** | `trusted` prints 0; clicks and keys go nowhere |

macOS prompts only once ever. If it was declined before, it must be added by
hand - triggering `AXIsProcessTrustedWithOptions` will not re-prompt. Ask the
user; do not burn turns retrying.

## 1. Launch

```bash
make -C server && make                       # server + native client
lsof -ti :9000 | xargs -r kill               # free the port first
(cd server && exec ./gameserver > "$SCRATCH/gs.log" 2>&1 &)
sleep 2
(exec ./platformz udp://localhost:9000 > "$SCRATCH/client.log" 2>&1 &)
sleep 7                                      # window + UDP handshake
tail -1 "$SCRATCH/client.log"                # want: "Joined as player slot 0"
```

`./platformz local` is offline. **Bare `./platformz` connects to the live
production server** (`secrets.mk` bakes the host in) - always pass a URL locally.

> **Quit `dist/PLATFORMZ.app` first if it is running.** It is an older packaged
> build, its window looks almost identical, and both processes are called
> `platformz` - so `tell application "platformz" to activate` raises the wrong
> one and you screenshot a stale UI. `pgrep -lf PLATFORMZ.app` to check.

A freshly launched app is already frontmost; prefer that over `activate`.

## 2. Screenshot and the coordinate mapping

```bash
screencapture -x -o "$SCRATCH/s.png"
sips -Z 1200 "$SCRATCH/s.png" --out "$SCRATCH/s_view.png"   # readable size
```

Read `s_view.png`. Then convert what you measured on it into the screen points
`uidriver click` wants - **never assume the factor, it depends on the display**:

```bash
PX=$(sips -g pixelWidth "$SCRATCH/s.png" | awk '/pixelWidth/{print $2}')
PT=$(osascript -e 'tell application "Finder" to get bounds of window of desktop' | awk -F', ' '{print $3}')
VIEW=1200
echo "scale=4; $PT / $VIEW" | bc     # multiply your measured x,y by this
```

On a 2940px-wide capture with a 1470pt desktop and a 1200px view, the factor is
**1.225**. Zoom a region to read small text: `sips -c <h> <w> --cropOffset <top>
<left>` on the **native** png (offsets are in capture pixels, so scale up by
`PX/VIEW`).

## 2b. Better: derive coordinates from the source, not the screenshot

Measuring a screenshot by eye is the main source of wasted turns - a click that
lands 20px off is indistinguishable from a widget that ignores clicks, and you
will blame the widget. The window's content area is **1000x700 unscaled points**,
which is exactly the game's own coordinate space, so game coords map to screen
points 1:1 once you know the origin:

```bash
POS=$(osascript -e 'tell application "System Events" to tell process "platformz" to get position of window 1')
OX=$(echo $POS | cut -d, -f1); OY=$(echo $POS | cut -d, -f2 | tr -d ' ')
# content origin = (OX, OY + 28)   <- 28pt title bar
```

Then read the widget's rect straight out of `screens.h` / `ui.h` and add:

```
screen_x = OX + rect.x + rect.width/2
screen_y = OY + 28 + rect.y + rect.height/2
```

e.g. the CUSTOM screen's INVITE ONLY toggle is `{470, 368, 100, 26}`, so with an
origin of (235, 113) it is at (755, 494). Use screenshots to confirm the result,
not to find the target.

**All three widget kinds respond to `uidriver click`** - `UiButton`, `UiToggle`
and `UiSlider`. They share the same `hovered && IsMouseButtonPressed` hit test,
and the 90ms the driver holds the button down is ~5 frames, enough for
`UiSlider`'s press-and-hold. If a control appears not to respond, suspect the
coordinates first and the game's own state second (a slider on a screen whose
values the server echoes back will snap straight back - that is the game, not the
driver).

## 3. Click and type

```bash
"$SCRATCH/uidriver" click 734 627        # screen POINTS
"$SCRATCH/uidriver" key 46               # M - end match
"$SCRATCH/uidriver" chord 55 9 cmd       # Cmd+V (paste)
```

**Do not use `osascript`/System Events for input.** Two traps, both of which
make working code look broken:

- `keystroke "v"` posts a unicode **character**. `GetCharPressed()` sees it,
  `IsKeyPressed()` never does - so a paste chord or hotkey appears dead while
  plain typing works. This is the single most misleading failure here.
- `click at {x,y}` fails with `-25211` regardless of Accessibility.

Also: a chord must press the **modifier key itself** first. `CGEventSetFlags` on
the letter alone leaves `IsKeyDown(KEY_LEFT_SUPER)` false. `uidriver chord`
already does this correctly.

**Always prove focus before blaming a key.** Click the field, send a plain
letter, screenshot, and only then send the chord. A text field with no focus
swallows everything silently.

## 4. Reach a screen without clicking

Cheaper and far less brittle than a click path, and it works with no
Accessibility grant at all:

```bash
./platformz udp://localhost:9000 --match ABCD    # straight into that room's LOBBY
./platformz local                                # offline, title screen
./platformz bench 120 128 18 4                   # skip the menus into a match
```

Drive the *other* side with the protocol probes - they need no UI:

```python
import sys, os, time
sys.path.insert(0, os.path.abspath('server/test'))
from probe import C
a = C("FRIEND"); a.hello(); time.sleep(1.0)
a.send({"type":"create","n":"ROOM","pre":"DEFAULT","priv":True,"code":""})
time.sleep(120)                 # stay connected or the room gets reaped in 30s
```

Room codes come out of the server log:

```bash
grep -o "Match [A-Z0-9]* created (custom, invite-only)" "$SCRATCH/gs.log" | tail -1
grep -o "official room [A-Z0-9]*" "$SCRATCH/gs.log" | tail -1
```

## 5. Assert on the server, not just the picture

The strongest checks pair a screenshot with what the server did or did not see.
This is how "LOCAL MATCH secretly started an online match" was caught:

```bash
BEFORE=$(wc -l < "$SCRATCH/gs.log")
"$SCRATCH/uidriver" click 634 539                 # MEDIUM on the LOCAL screen
sleep 8
tail -n +$((BEFORE+1)) "$SCRATCH/gs.log" | grep -v "^tick" | head
# a local match must produce ZERO "countdown started" lines
```

## 6. Clean up

```bash
pkill -f "platformz udp"; pkill -f hold.py
lsof -ti :9000 | xargs -r kill
```

Leaving a client running holds a slot; leaving a server running makes the next
run bind-fail and silently talk to the old one.

## Current title-screen coordinates

Measured on a 1200px-wide view, multiply by the factor from §2. **These move
whenever `screens.h` changes - re-measure from a screenshot rather than trusting
them.**

| Target | view x,y | points (×1.225) |
|---|---|---|
| QUICK MATCH | 599, 375 | 734, 459 |
| FIND A MATCH | 599, 420 | 734, 515 |
| CUSTOM MATCH | 599, 466 | 734, 571 |
| LOCAL MATCH | 599, 512 | 734, 627 |
| LOCAL: MEDIUM | 518, 440 | 634, 539 |
| BROWSE: CODE field | 677, 546 | 829, 669 |
| LOBBY: COPY INVITE | 750, 225 | 919, 276 |
