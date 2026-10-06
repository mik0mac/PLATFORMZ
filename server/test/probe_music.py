#!/usr/bin/env python3
"""The room picks the music, so everyone in it hears the same cue (#174).

Each room keeps its own jukeboxes (jukebox.h's MakeJukeboxes, one per screen) and
sends the current MusicId in every state packet. Checked here: two clients in one
room are sent the same cue in every phase, the lobby sends none, and the gameplay
track moves on match after match until the room has played its whole list.

MusicId values, per constants.h - append only, like the rest of the wire.

    cd server && ./gameserver &
    python3 test/probe_music.py
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import C, OPTS, host_room, join_room, wait_until

MUSIC_COUNTDOWN, MUSIC_GAMEOVER, MUSIC_NONE = 1, 5, 6
GAMEPLAY = {2, 3, 4}  # MUSIC_GAMEPLAY, MUSIC_PLACEHOLDER1, MUSIC_PLACEHOLDER2

fails = 0
def check(ok, what):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {what}")
    if not ok: fails += 1

host = C("HOST"); host.hello(); time.sleep(1.0)
peer = C("PEER"); peer.hello(); time.sleep(1.0)
room = host_room(host, "JUKEBOX")
check(bool(room), f"hosted a room ({room})")
check(join_room(peer, room), f"peer joined it ({peer.matchCode})")
time.sleep(0.8)

print("the lobby sends no cue: the client keeps its own title track there")
check(host.music == MUSIC_NONE and peer.music == MUSIC_NONE,
      f"lobby music is none (host {host.music}, peer {peer.music})")

def both_in(phase):
    return wait_until(lambda: host.phase == phase and peer.phase == phase, timeout=10.0)

played = []
for n in range(1, len(GAMEPLAY) + 1):
    print(f"match {n}")
    host.send(dict({"type": "start"}, **OPTS))
    check(both_in("countdown"), f"both counting down")
    check(host.music == peer.music == MUSIC_COUNTDOWN,
          f"same countdown cue (host {host.music}, peer {peer.music})")
    check(both_in("playing"), f"both playing")
    time.sleep(0.3)
    check(host.music == peer.music and host.music in GAMEPLAY,
          f"same gameplay cue (host {host.music}, peer {peer.music})")
    played.append(host.music)
    host.send({"type": "endmatch"})
    check(both_in("gameover"), f"both at game over")
    check(host.music == peer.music == MUSIC_GAMEOVER,
          f"same game-over cue (host {host.music}, peer {peer.music})")

print("the room walks its whole gameplay list, one track a match")
check(set(played) == GAMEPLAY, f"{len(GAMEPLAY)} matches played {len(GAMEPLAY)} different tracks ({played})")

for c in (host, peer):
    c.drop(goodbye=True)

print("FAIL" if fails else "PASS", f"({fails} failures)")
sys.exit(1 if fails else 0)
