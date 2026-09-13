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
| **preset** | `DEFAULT`, … | at creation | the rule set it started from |
| **map** | `SMALL` \| `MEDIUM` \| `LARGE` \| `XL` | its options | which arena |

**Kind and visibility are different questions,** and conflating them was a real
bug (#107). *Kind* says who is in charge: an **official** room has its options
locked and starts itself once `PUBLIC_MIN_PLAYERS` arrive, so it promises a game
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
| `quick` | — | fullest joinable official lobby, or a new one |
| `leave` | — | back to the default room |
| `start` | the options bundle | host only; locked rooms ignore it |
| `options` | the options bundle | host only; echoed live to everyone |
| `endmatch` | — | host only |
| *(input)* | `seq`, `ep`, `mx`, `mz`, `jp`, `grav`, `fire`, `yaw`, `pitch` | **no `type`** — the 60 Hz packet, kept small |

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
| **welcome** | slot + room identity + static world | JSON over WS, binary tag `0x0A` over UDP |
| **state** | phase, countdown, epoch, options, roster | JSON over WS, binary tag `0x09` over UDP, 60 Hz |
| `matchlist` | `cur`, `next`, `total`, `m[]` | **public rooms only** |
| `created` | `m` | the code of the room you just made — the only place a private room's code is ever revealed |
| `joinfail` | `why` | see below |
| `leaderboard` | `lb[{n,s,b}]` | the all-time table, already ranked. `b` marks a bot row; the top rows of both classes are sent together |
| *(chunk)* | tag `0x03` | transport framing, reassembled below the protocol |

A `matchlist` row is `{c, n, pre, k, map, ph, p, max, j}` — code, name, preset,
kind, map, phase, players, max players, joinable. **`j` is honest**: a room that
is full or already playing says so, which is what makes "pick another" advice
somebody can act on.

The list is **capped to one datagram** (~1160 bytes, 8 rows) and paged with
`cur`/`next`; `next < 0` means that was the last page. The page size is
deliberately *below* the room cap so the paging path runs from day one rather
than rotting until the cap is raised.

### `joinfail` reasons

| `why` | Means |
|---|---|
| `notfound` | no such room, or it was reaped |
| `badcode` | wrong join code for a private room |
| `full` | that room has no free slot — you were put somewhere else, or left where you were |
| `server_full` | nowhere free at all, or you are over your room-creation budget |
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

The all-time table is keyed on the identity, not on the display name, which is
what stops two players called `MIKE` sharing a row and what makes renaming
yourself keep your history — the name is a **property of the row**.

Bots have no identity (nothing signs for a bot), so a bot's row is keyed on its
name behind a `-`. That prefix is load-bearing: a human id is 32 hex characters
and can only begin `[0-9a-f]`, while `-` is 0x2D, below `'0'` at 0x30. So the two
classes are disjoint by construction, "is this a bot" is one character compare,
and bots sort ahead of every human for free in both the map and the file.

```
    3	-GEOFF	GEOFF                              a bot
    0	13f67c4c8f5b0f8d32d392dee979510a	MIKE    a player
    0	ec6582d8780b7943bd4d6c720fc007b8	MIKE    a different player, same name
```

The wire sends the top rows of **both** classes tagged with `b`, so the client's
LEADERBOARD toggles PLAYERS/BOTS without a round trip. It shows players by
default: a bot plays every single match, so a combined board is nothing but bots
within a day.

The **id** (the token's first half) is what anything persistent should key on —
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
| Rooms one **address** may mint | 3, one back every 2 min | `server_full` |
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
