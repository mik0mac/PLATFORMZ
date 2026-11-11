#!/usr/bin/env python3
"""A room that is not simulating sends a fraction of its ticks.

The server used to broadcast every room's state on every tick, whatever the
room was doing - a lobby with one person in it cost the same 60 packets a
second as a firefight. Now only a room whose world is moving sends every tick;
a lobby, a countdown and a finished match send one tick in
IDLE_BROADCAST_EVERY_N_TICKS (constants.h), which is 10 a second.

What must NOT slow down, and is checked here because it is what a player would
feel: a change of phase still arrives at once (it is what flips every client's
screen), and a live match is still sent at the full rate.

Thresholds are wide on purpose. This measures packets against a wall clock on
whatever machine CI gave us; the claim is "about ten, not sixty" and "about
sixty, not ten", not a number to the packet.

    cd server && ./gameserver &
    python3 test/probe_sendrate.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, wait_until

IDLE_HZ, LIVE_HZ = 10.0, 60.0

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

def rate(c, secs):
    n0, t0 = c.states, time.time()
    time.sleep(secs)
    return (c.states - n0) / (time.time() - t0)

host = C("HOST"); host.hello(); time.sleep(0.5)
room = host_room(host, "SEND RATE")
check(bool(room), f"hosted a room (code={room})")
time.sleep(0.5)

print("in the lobby")
r = rate(host, 3.0)
check(IDLE_HZ * 0.5 < r < IDLE_HZ * 2.0, f"about {IDLE_HZ:.0f} packets a second, not {LIVE_HZ:.0f} (got {r:.1f})")

print("a rules change still shows up promptly")
opts = dict(OPTS); opts["nplayers"] = 4; opts["maxbots"] = 3
host.send({"type": "options", **opts})
check(wait_until(lambda: host.maxBots == 3, 1.0, step=0.02), f"the echo arrived within a second (maxBots={host.maxBots})")

print("the countdown")
t0 = time.time()
host.send({"type": "start", **opts})
check(wait_until(lambda: host.phase == "countdown", 2.0, step=0.01), f"began (phase={host.phase})")
# The flip is sent on its own tick rather than waiting for the next idle one.
# Not provable to the millisecond from here, but a flip that waited would be up
# to 100 ms late; half a second catches a regression to "never sent promptly".
check(time.time() - t0 < 0.5, f"and was told at once ({(time.time() - t0) * 1000:.0f} ms after START)")
r = rate(host, 2.5)
check(IDLE_HZ * 0.5 < r < IDLE_HZ * 2.0, f"counts down at about {IDLE_HZ:.0f} a second (got {r:.1f})")

print("the match")
check(wait_until(lambda: host.phase == "playing", 8.0, step=0.01), f"went live (phase={host.phase})")
r = rate(host, 3.0)
if host.phase == "playing":
    check(r > LIVE_HZ * 0.7, f"is sent at the full rate (got {r:.1f} a second)")
else:
    print(f"  NOTE the match ended inside the 3s sample (phase={host.phase}); live rate not checked this run")

print("after the host ends it")
host.send({"type": "endmatch"})
check(wait_until(lambda: host.phase == "gameover", 2.0, step=0.01), f"game over (phase={host.phase})")
# The world keeps simulating for GAMEOVER_SIM_SECONDS (7) so the last moments
# play out on screen, then stops. Sample after that.
time.sleep(8.0)
r = rate(host, 2.5)
check(r < IDLE_HZ * 2.0, f"a finished match settles to the idle rate (got {r:.1f} a second)")

host.alive = False
host.send({"type": "goodbye"})
print()
print("FAILURES" if fails else "idle rooms send a fraction of their ticks")
sys.exit(1 if fails else 0)
