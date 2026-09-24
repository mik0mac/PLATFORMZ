# Matchmaking — the directory protocol

What a PLATFORMZ server offers, how a client asks for it, and what comes back.
This is the reference for **both ends**: the client in `wire.h`, the server in
`server/server_main.cpp`. If they ever disagree, the code is right and this file
is wrong — but the whole point of writing it down is that a reader can tell.

For *running* a server see [`deploy-vultr.md`](deploy-vultr.md); for testing one
see [`multiplayer-testing.md`](multiplayer-testing.md); for why any of it is
shaped this way see [`matchmaking-plan.md`](matchmaking-plan.md).

---

## The model in one paragraph

A server holds up to **12 rooms**. Each room is a whole match — its own world,
roster, options and phase — and the sim ticks every one of them on a single 60 Hz
beat. A connection belongs to **at most one room at a time**, or to none at all.
Players find rooms through a directory (`list`), enter one (`join`/`quick`), or
make one (`create`). None of that exists on the wire as anything but JSON, on
both transports.

---

## What a room is

| Property | Values | Set by | Means |
|---|---|---|---|
| **code** | 4 chars, e.g. `7QK2` | the server, at creation | its name on the wire, and its invite |
| **kind** | `official` \| `custom` | the server (only it mints official) | **how it is governed** |
| **visibility** | public \| private | whoever created it | whether it appears in `list` |
| **phase** | `lobby` \| `countdown` \| `playing` \| `gameover` | the match | what it is doing right now |
| **preset** | `DEFAULT`, `CHAOS`, `SKIRMISH`, `ENDURANCE`, `VOID` | at creation | the rule set it started from |
| **map** | `SMALL` \| `MEDIUM` \| `LARGE` \| `XL` | its options | which arena |

**Presets** live in `options.h` as an ORDERED list, and the order is behaviour,
not style: it runs from typical gameplay to niche, and quick match walks it to
break ties (below). One pinned official room is created per entry at boot. The
name crosses the wire as a *string*, never an index, so adding one is a server
rebuild and nothing else — an old client naming a preset that no longer exists
gets `DEFAULT` rather than a failed join.

Two match options have no slider in the OPTIONS modal, because they are things a
preset's author sets rather than things a player dials: `maxBots` and
`minHumansToStart`. `maxBots` caps how many
unclaimed roster slots are filled with bots — a room is still `numPlayers` slots
and any of them can be taken by a human, so this does not change capacity; the
slots past the cap are simply **empty**. An empty slot has no body at all: it is
not drawn, not shootable, and not counted when the server asks whether one player
is left standing — but it is still in the roster and still joinable, including
mid-match. A client learns a slot is empty from the per-player `active` flag it
already receives, not from `maxBots` — but both rules ride the options block like
every other, so the lobby can say how many more players a room is waiting for and
a host's START echoes the room's own values back rather than resetting them.

**Kind and visibility are different questions,** and conflating them was a real
bug (#107). *Kind* says who is in charge: an **official** room has its options
locked and starts itself once its preset's `minHumansToStart` (default
`PUBLIC_MIN_PLAYERS`) arrive, so it promises a game
that begins; a **custom** room is run by the person who created it, who owns the
options and the START button. *Visibility* only says whether it is advertised. A
public custom room is an ordinary thing to want.

**Codes** are 4 characters from a 32-letter alphabet with no `O`, `0`, `I` or `1`
— so a code can be read aloud and typed back. Retired codes stay retired for the
life of the process, so a client holding a stale one can never land in a
different room that happens to have taken it.

An empty room is destroyed after a grace — **30 s** from a lobby, **60 s** if it
was mid-match, so a whole room losing its network for a moment does not tear down
a game whose bodies are still being held open for reconnects. The default and
official rooms are pinned and never reaped.

---

## The two gates, which are not the same gate

| | `PLATFORMZ_KEY` | a room's join code |
|---|---|---|
| Answers | "may you speak to this process at all" | "may you enter this room" |
| Checked at | WS upgrade / UDP hello, before anything else | `join`, once you are already connected |
| On failure | **no reply at all** — the port looks dead | a `joinfail` you can read on screen |
| Scope | the whole server | one room, while it exists |

Both can be on at once, and there is no configuration fork between "friends only"
and "public" — see the deploy doc. A **private** room is omitted from `list`
entirely, which is what makes its code a real capability rather than a formality.

---

## Connecting

**Connecting does not put you in a room.** Name one and you get it, or a refusal
saying why; name none and you hold none, which is the ordinary way to arrive. You
browse, you pick, and only then do you have a slot. `leave` returns you to that
state, and a refused `join` leaves you in it - there is no fallback room, because
a seat in a room nobody asked for is the thing this removes.

A client learns the handshake landed from a `welcome` if it got a seat, and from
an `unseated` if it did not. Before that second message existed, holding no room
was indistinguishable from a handshake that never arrived, and the only recovery
was to keep re-helloing. A **bare hello is not a request for a seat** - it is a
client saying it is still there - so re-sending one never seats you.

A connection with no room is a working connection: it can `list`, `join`,
`create` and `quick`, and it is sent the leaderboard, which is not a property of
a room. Over UDP it must still keep up its heartbeat - `UDP_CLIENT_TIMEOUT_LOBBY`
is 3 seconds and applies to everyone, seated or not.

Everything a client sends is JSON. Everything it receives is JSON **except** the
welcome and the per-tick state over UDP, which are binary (`netbin.h`) so a full
8-player tick fits one unfragmented datagram. The client dispatches on **byte 0**:
`{` is JSON, anything else is a binary tag.

### WebSocket

The key and the room ride the upgrade URL, because a WS connection claims its
slot during the handshake, before any message could arrive:

```
ws://host:9000/?key=KEY&match=CODE&cid=INSTALL-ID
```

TCP's own handshake has already proved the address, so there is nothing else to do.

### UDP — two round trips, not one

A source address in a datagram is a claim, not a fact. So an unknown endpoint's
first `hello` gets back **one thing**:

```
client → {"type":"hello","name":"…"}
server ← {"type":"challenge","c":"<24 hex>"}      51 bytes, and nothing else
client → {"type":"hello","name":"…","c":"<24 hex>"}
server ← welcome (binary)
```

The cookie is `truncate(HMAC(server secret, family‖addr‖port‖bucket), 96 bits)`
over 30-second buckets, with the previous bucket also accepted — so it is valid
for 30–60s. The server **stores nothing**: it recomputes the cookie from the
address it actually observes. A spoofer never receives the challenge (it went to
the address they forged), so they never get past it, and a flood of spoofed
hellos costs one HMAC and one small send each and allocates nothing.

A wrong `PLATFORMZ_KEY` gets **no challenge either** — silence on a bad key comes
first.

Clients keep the cookie and re-send it; when it goes stale the server simply
challenges again, which costs one more round trip and nothing else.

---

## Client → server

All JSON, both transports. Unknown fields are ignored; absent optional fields
take the server's default.

| Type | Fields | Notes |
|---|---|---|
| `hello` | `name`, `key`?, `cid`?, `match`?, `c`?, `tok`? | handshake and retry. Re-sent every 0.5 s until welcomed |
| `ping` | — | keepalive. UDP has no disconnect event, so silence is how the server notices you left |
| `goodbye` | — | leaving on purpose; frees the slot at once instead of after the idle timeout |
| `name` | `name` | display name for your slot |
| `list` | `cur` | one page of the directory, from that cursor |
| `create` | `n`, `pre`, `priv`, `code` | make a room and enter it |
| `join` | `m`, `code`? | enter a room by code |
| `quick` | — | fullest joinable official lobby, ties broken by preset order; or a new one |
| `leave` | — | back to the default room |
| `start` | the options bundle | host only; locked rooms ignore it |
| `options` | the options bundle | host only; echoed live to everyone |
| `endmatch` | — | host only |
| *(input)* | `seq`, `ep`, `mx`, `mz`, `jp`, `grav`, `fire`, `yaw`, `pitch` | **no `type`** — the 60 Hz packet, kept small |

**`quick` picks the fullest room first**, so players pack together instead of
scattering one each across empty lobbies. On a quiet server every official room
is equally empty, so the tie is the common case — and it breaks on the preset
order in `options.h`, which is why that list runs typical-first. Without it the
winner was whichever room sorted first by its randomly minted code, making a
stranger's first game a coin flip between the standard match and the one with no
walls. Official rooms only: dropping someone into a stranger's public custom room
hands their experience to a host who may never press START.

The **options bundle** (`writeOptionKeys` in `wire.h`) is the same set of keys on
`start` and `options`: `nplayers`, `diff`, `welast`, `pelast`, `boost`, `rspeed`,
`xradius`, `jthrust`, `fburn`, `fregen`, `walls`, `phys`, `ff`, `coast`, `map`.
Map size lives **here**, not on the START button, so a lobby can show everyone
which arena they are about to play before anyone presses anything.

`ep` is the match epoch, echoed from the newest state packet. The server drops
input stamped with any other, so an input still in flight from the previous match
cannot land on the new one's spawn state.

---

## Server → client

| Type | Shape | Notes |
|---|---|---|
| `challenge` | `c` | UDP only. Not a refusal — answer it and hello again |
| `identity` | `tok` | a token to store and present from now on. Sent when you had none, or yours no longer verifies |
| **welcome** | slot + room identity + static world | JSON over WS, binary tag `0x0C` over UDP |
| **state** | phase, countdown, epoch, options, roster | JSON over WS, binary tag `0x09` over UDP, 60 Hz |
| `matchlist` | `cur`, `next`, `total`, `m[]` | **public rooms only** |
| `created` | `m` | the code of the room you just made — the only place a private room's code is ever revealed |
| `joinfail` | `why` | see below |
| `leaderboard` | `lb[{n,s,b}]`, optional `best` | the best RUNS, already ranked. `b` marks the bot row. `best` is this client's own best run, pinned under the board — absent when they have none, and absent when it is already up there. This is the **ONLINE** tab of the client's HIGH SCORES modal; the LOCAL tab is a second board the client keeps itself (`local_scores.h`, fed only by offline matches) and never reaches the wire |
| *(chunk)* | tag `0x03` | transport framing, reassembled below the protocol |

**Room identity** in the welcome is `m`, `k`, `n`, `p` — code, kind, name, and
the preset it was seeded from. None of it is derivable client-side: quick match
picks the room, and connecting with no room named lands you in one you never
chose, so the code you think you asked for proves nothing. The preset travels as
its **key**, not its description: `options.h` is compiled into both ends, so the
client looks the sentence up locally rather than having it exist in two places.
The lobby heads the screen with `n` and describes the room from `p`.

A `matchlist` row is `{c, n, pre, k, map, ph, p, max, j}` — code, name, preset,
kind, map, phase, players, max players, joinable. **`j` is honest**: a room that
is full or already playing says so, which is what makes "pick another" advice
somebody can act on.

The list is **capped to one datagram** (~1160 bytes, 8 rows) and paged with
`cur`/`next`; `next < 0` means that was the last page. The page size is
deliberately *below* the room cap so the paging path runs from day one rather
than rotting until the cap is raised.

#### The order

Rooms come back **sorted by how close each one is to being a game**, outermost
key first:

1. **Band.** Joinable lobbies (`lobby`/`countdown`), then joinable matches
   already in progress, then rooms you cannot enter at all (full, or `gameover`).
   A 7/8 room that is already playing is a worse offer than a 2/8 lobby, so
   fullness is not allowed to lift it above one. Unjoinable rooms are still
   *listed* — the browser draws an inert reason on the row rather than making it
   vanish — but never above something you can actually join.
2. **Fewest free spots.** The anti-fragmentation rule: five players spread across
   five empty rooms is the failure state of a small-population game, so the
   second person to arrive should land on the first person's room rather than
   beside it. Free spots, **not** head count — rosters differ per preset, so
   ranking by players would put a 5/8 room above a 3/4 room that is one person
   from starting.
3. **Preset rank** — the typical-to-niche ramp in `options.h`, the same one QUICK
   MATCH walks.
4. **Code**, which makes the order *total*: without it, many rooms of one preset
   at one occupancy would have no defined order and could shuffle between two
   requests.

Every **empty** room is clamped to the same free-spot key, so empty rooms tie and
fall through to the preset ramp. That is what makes a freshly booted server —
where every room is empty — list in exactly preset order, every time, with no
separate mechanism keeping it there.

#### Paging is a snapshot

`cur = 0` means **take a fresh snapshot**; every other cursor is a slice of that
same frozen vector, contents included. This is not an optimisation. The order
above is derived from *live* occupancy, so re-deriving it for page 1 could cut
that page from a list sorted differently to the one page 0 came from — showing a
room on both pages, or on neither. Sorting by room code used to make that
invariant free.

A snapshot is held per connection (on the rate-limit record, which is already
keyed by connection id and already swept) and is at most `MATCH_MAX_CONCURRENT`
rows.

The client side of this splits a refresh into the two things it does, because
only one of them is safe to do while somebody is reaching for a row:

- **Contents** — how full a room is, its phase, whether it can be joined. A
  background walk updates each row **in place**, matched by code, every few
  seconds. Nothing moves. This is what stops a `gameover` room from still reading
  ENDING ten seconds after it became a joinable lobby again.
- **Order and membership** — which rooms exist and in what sequence. Only
  **REFRESH** changes those, because that is the moment the player is not
  mid-reach. A room the background walk can no longer find is greyed in place and
  labelled GONE rather than removed (removing it would shift every row below);
  rooms it finds that are not on screen are offered as a count beside the button
  rather than inserted.

The old client simply re-asked every two seconds and replaced the list. That was
harmless against a key that never changed, and became a way to make somebody
click the room next to the one they aimed at the moment the order went live.

**Paging never reaches the player.** The client asks for `cur = 0`, then follows
`next` to the end of the snapshot and concatenates, so the browser shows ONE
scrollable list of every public room rather than pages to click through. That is
only coherent because of the snapshot above — pages cut from a list re-sorted
between requests would assemble into one that never existed at any single moment.
Assembly is bounded by the registry cap, but it is also rate limited (a burst,
then one `list` a second, and an over-budget request is dropped in silence), so
the client re-asks for a page that does not come back and gives up after four
tries rather than spinning: a short list somebody can act on beats a spinner.

The REFRESH button goes inert for a second after the **last page** of a refresh
lands, not the first. A refresh costs one request per page while the budget
refills at one a second, so timing it from page 0 would let a multi-page
directory drain the bucket faster than it fills.

`probe_listorder.py` is the test for all of it.

### `joinfail` reasons

| `why` | Means |
|---|---|
| `notfound` | no such room, or it was reaped |
| `badcode` | wrong join code for a private room |
| `full` | that room has no free slot — you were put somewhere else, or left where you were |
| `server_full` | the registry has no free room at all |
| `too_many_rooms` | *you* are over your own room-creation budget. Distinct from `server_full` on purpose: that one is the whole server and clears in seconds as empty rooms are reaped, this one is your three rooms and comes back one every two minutes. They shared a token until it was pointed out that "SERVER IS AT CAPACITY" was being shown with a third of the registry free |
| `rate_limited` | too many attempts; wait |
| `inprogress` | **defined but never sent.** The client renders it ("MATCH ALREADY STARTED") and the enum carries it, but no server path emits it: a match in progress either has a free slot, in which case you join it, or it is `full` |

> **`notfound` and `badcode` are distinguishable today, and the comment above
> `MoveConnToMatch` says they are not.** Getting `badcode` back tells you a
> private room with that code *exists* — so the reply is an existence oracle over
> the 4-character code space, with E1's five-guesses-a-minute budget as the only
> brake. The space is ~1M wide so this is slow rather than serious, and a private
> room's code is also its password, which means "wrong code" and "no such room"
> are the same event to an honest player. Worth collapsing into one reason; not
> changed here, because a docs pass is the wrong place to quietly alter what a
> security-relevant path answers.

**A refusal never ends the connection.** If there is no seat anywhere you stay
connected holding no slot — you can still list, join and create, and your next
`hello` takes the first seat that frees up. That is the whole point: being hung
up on is the worst possible answer to "this room is full", because the one thing
you want next is the list of rooms that are not.

---

## Identity

Three different things get called "who you are", and they are not interchangeable.

| | What it is | Trust |
|---|---|---|
| **display name** | what you typed | none — two players can both be `MIKE` |
| **`cid`** | a UUID the client minted and keeps in its own profile | none — the player owns the file and can put anything in it |
| **identity token** | a random id the **server** minted and signed | the id is one this server issued, and the holder had it |

The token is `id ‖ HMAC(secret, id)`, 64 hex characters, and the server keeps **no
record of it** — it re-computes the tag and compares, the same trick as the
handshake cookie. So there is no user table, nothing to back up beyond the secret,
and nothing to leak.

```
client → hello with no "tok"
server ← {"type":"identity","tok":"<64 hex>"}      store it in the profile
client → hello with "tok" from then on            (or ?tok= on a WS upgrade URL)
```

A token that does not verify — made up, truncated, or signed before the operator
rotated the secret — is **replaced, never refused.** A stale token is a client to
re-issue, not a player to lock out.

**What it proves is continuity, not personhood.** It says "the same client as last
time". It does not say a human is who they claim: a player can copy their own
token to a second machine, or run several clients. That is enough for a
friends-and-family ranking and not enough for a competitive public one, which
would want real accounts. Do not let a later feature quietly assume otherwise.

### What keys the scoreboard

Rows are keyed on the identity, not on the display name — which is what stops two
players called `MIKE` sharing a row.

There are two tables in one file, told apart by a line-type column:

```
C  <score>  <matches>  <id>  <name>                             career totals
R  <score>  <when>  <map>  <kind>  <matchName>  <id>  <name>    one finished run
```

**R is what players see** — the best runs, so one player can hold several rows.
**C is recorded but not displayed**; a capped run list cannot be summed back into
a career total, so it has to be kept from the start or the history is gone.

**The name rule differs between them, deliberately.** An R row freezes the name as
it was: a run is a historical event, and renaming yourself does not rewrite what
happened that Tuesday. A C row follows renames, because a total belongs to a
person. They look inconsistent and are not.

Bots have no identity. On C each keeps its own row (`-GEOFF`); on R they share one
(`-BOT`) and hold a single line between them, because nine bot names at three rows
each would leave no room for humans on a board of ten. The `-` prefix makes both
work: a human id is 32 hex characters and can only begin `[0-9a-f]`, while `-` is
0x2D — disjoint by construction, one character to test, and bots sort first for
free.

```
    3	-GEOFF	GEOFF                              a bot's career row
    0	13f67c4c8f5b0f8d32d392dee979510a	MIKE    a player
    0	ec6582d8780b7943bd4d6c720fc007b8	MIKE    a different player, same name
```

`PLATFORMZ_SCORES_OFFICIAL_ONLY=1` limits the board to runs from official rooms.
It filters on **read**, so it is free in both directions and retroactive — custom
runs are hidden, never discarded.

The **id** (the token's first half) is what anything persistent should key on —The **id** (the token's first half) is what anything persistent should key on —
it is safe to log and to write to disk. The **token** is a bearer credential and
belongs only in the client's profile and on the wire. They are separate strings in
the code for exactly this reason.

---

## Limits

Enforced with no configuration. They are listed here so a refusal is
recognisable rather than mysterious.

| Limit | Default | Refused with |
|---|---|---|
| Rooms on the server | 12 | `server_full` |
| Rooms one **address** may mint | 3, one back every 2 min | `too_many_rooms` |
| Room moves (join / quick / leave) | 5 in hand, 1/s | `rate_limited` |
| Wrong room codes | 5 per minute per connection | `rate_limited` |
| `list` replies | 3 in hand, 1/s | *silently dropped* |
| UDP handshake | must echo a cookie | a `challenge` |
| Live matches at once | off by default | the start is **held**, not refused |

Two are tunable — `PLATFORMZ_MAX_ROOMS_PER_ADDR` and `PLATFORMZ_MAX_ACTIVE` — for
reasons the deploy doc explains.

The `list` limit is the one that drops silently rather than answering, because an
error reply is still a reply, and the whole reason the list is budgeted is that
it is the largest thing an authenticated client can ask for on demand.

Once the bad-code budget is spent it refuses **every** join, not only wrong ones.
It has to: whether a code is a guess is only knowable after the lookup that
answers the guess. Mistype a code five times and you wait out the minute.

---

## Where it lives

| Concern | File |
|---|---|
| Every client→server builder, every inbound parse | `wire.h` |
| Binary tags, chunking, quantisation | `netbin.h` |
| Directory verbs, budgets, seating | `server/server_main.cpp` |
| Which rooms exist, codes, reaping | `server/registry.h` |
| One room: world, roster, phase, tick | `server/match.h` |
| HMAC + constant-time compare, hex, randomness | `server/crypto.h` |
| Minting and verifying the identity token | `server/identity.h` |
| The token bucket every limit is built on | `server/bucket.h` |
| Browser + lobby screens | `screens.h` |

Changing the **binary** tags is a hard break with every deployed native client —
it latches `SERVER VERSION MISMATCH` and never recovers — so bump once, ship both
ends together, and never reuse a retired value. `server/test/ci_smoke.sh` checks
the running server's tags against `netbin.h` on every push, which is what stops a
stale binary shipping quietly.

`STATE_BIN_VERSION` is at `0x0B`: the per-tick option block grew two `u8`s, for
`maxBots` and `minHumansToStart`, straight after the roster size. It skipped
`0x0A` (the welcome's at the time) and `0x06` (burned by the retired FULL
packet). The flags byte could not absorb them — it has two free bits and these
need four each.

`WELCOME_BIN_VERSION` is at `0x0C`: the room identity block grew the room's
**name** and **preset key**, two length-prefixed strings after the kind byte and
still ahead of the static world. `0x0A` is burned by the layout without them — a
client that predates the change would read the name's length byte as the
boundary half-size and mis-slice every platform after it, so the tag bump is what
turns silent corruption into an honest mismatch.
