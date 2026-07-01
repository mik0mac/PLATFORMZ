# PLATFORMZ Bot AI — Behaviour Trees

How the computer-controlled players (`players[1..]` in local mode) think. All the
code is in `bot_logic.h`; the tree is assembled in `main.cpp` (`startGame`) and
tuned in `constants.h`. This doc is both a primer on behaviour trees and a map of
how ours works — plus one designed-but-not-yet-built feature (the `Chance` node).

## 1. What a behaviour tree is (and why)

A behaviour tree (BT) is a way to organise an agent's decision-making as a tree of
small, composable pieces instead of one big `if/else` blob or a state machine.

Every frame you **tick** the tree from the root. Each node, when ticked, returns
one of three statuses:

- **Success** — the node did its job / its condition is true.
- **Failure** — it couldn't / its condition is false.
- **Running** — it's an ongoing action that isn't finished yet.

Parent nodes use their children's statuses to decide what to do next. That's the
whole idea. The power comes from a few reusable "control-flow" node types you can
nest arbitrarily.

In our code the status type is:
```cpp
enum class Status { Success, Failure, Running };
```

## 2. The two kinds of nodes

**Leaf nodes** do the actual work:
- *Action* nodes make the bot do something — e.g. `MoveToTarget`, `FireAtTarget`,
  `MoveToPlatform`, `Idle`.
- *Condition* nodes just test something and return Success/Failure — e.g.
  `IsLowHealth`, `IsLowFuel`.

**Composite nodes** are the control flow — they own a list of children and decide
how to run them. Ours (in `bot_logic.h`):

- **`Selector`** — "OR / try in priority order." Ticks children until one returns
  non-Failure, and returns that. Use it for *fallbacks*: try A, else B, else C.
- **`Sequence`** — "AND / do in order." Ticks children until one returns
  non-Success, so every child must succeed for the sequence to succeed. Use it to
  gate an action behind a condition: `Sequence{ IsLowHealth, evade }` = "if low
  health, evade."
- **`Parallel`** — ticks *all* children every tick. Use it for things that happen
  at the same time — e.g. move AND shoot.

That's the vocabulary. Everything below is built from these.

## 3. How a bot node talks to the game: the Blackboard

Nodes never move the bot directly. Instead every node receives a **`Blackboard`**
— the per-bot, per-tick context — and writes its *intent* into an output struct:

```cpp
template <typename TargetT>
struct Blackboard {
    Player& bot;                              // the bot we're deciding for
    const TargetT& target;                    // who it's fighting (players[0])
    const std::vector<Player>& allPlayers;    // for threat avoidance
    const std::vector<Platform>& allPlatforms;
    const std::vector<Asteroid>& allAsteroids;
    PlayerInput& out;                         // <-- nodes write intent HERE
    float dt;
    BotDecision& decision;                    // per-bot state (see §6)
    const BotProfile& profile;                // per-bot personality (see §7)
};
```

The key idea: a node sets fields on `out` (a `PlayerInput` — the same struct a
human generates from keyboard/mouse). After the tree ticks, `main.cpp` feeds that
`PlayerInput` through `ApplyPlayerInput()` — **the exact same function a human or
network player goes through**. So a bot is "just another player" as far as the
physics/movement code is concerned. This is a deliberate, tidy separation:
*deciding* (the tree) is fully divorced from *acting* (ApplyPlayerInput).

The entry point that ties it together:
```cpp
PlayerInput botInput(bot, target, players, platforms, asteroids,
                     tree, dt, decision, profile);
```
It builds a `Blackboard`, ticks the tree once, and returns the filled `out`.

## 4. Our actual tree

Assembled in `main.cpp` (`startGame`). In plain English:

```
botTree = Parallel(               // do movement AND shooting at once
    movement = LatchedSelector(   // pick ONE movement behaviour, by priority:
        lowHealthResponse,        //   1. hurt?  -> evade + shoot asteroids
        lowFuelResponse,          //   2. low fuel? -> go land on a platform
        moveToTarget,             //   3. otherwise -> close in on the player
        idle                      //   4. nothing to do -> hold still
    ),
    fireAtTarget                  // always: aim at the player and fire when lined up
)
```
where:
```
lowHealthResponse = Sequence(IsLowHealth, evadeAndShootAsteroid)
lowFuelResponse   = Sequence(IsLowFuel,  moveToPlatform)
evadeAndShootAsteroid = Parallel(MoveToSafety, AttackAsteroid)
```

Read the `Selector` top-to-bottom as priorities: self-preservation (health, then
fuel) outranks attacking, which outranks idling. `fireAtTarget` sits in the outer
`Parallel`, so the bot can shoot *while* doing any of those movements.

## 5. Design decision #1 — one shared tree, per-bot state in the Blackboard

There is **one** tree object, and it is ticked for **every** bot. That means the
nodes must be **stateless** — if a node stored "am I currently evading?" in a
member variable, all bots would clobber each other's state.

So any per-bot memory lives in the Blackboard instead. Two structs do this:
- `BotDecision` — bookkeeping for the `LatchedSelector` (below).
- `BotProfile` — the bot's personality (§7).

`main.cpp` keeps one of each per bot (`std::vector<BotDecision> botDecisions`,
`std::vector<BotProfile> botProfiles`) and passes the right slot in each frame.
Remember this rule — it's the reason the `Chance` node in §8 is designed the way
it is.

## 6. Design decision #2 — `LatchedSelector`: throttle the *decision*, not the *action*

A plain `Selector` re-evaluates which branch wins *every frame*. For bots that's
both wasteful and twitchy — a bot could change its mind 60 times a second. We want
a bot to *commit* to a choice for a beat (~1 second), like a human would.

But we can't just run the whole tree once a second — then movement and aim would
update once a second too, and the bot could never actually track and shoot.

`LatchedSelector` resolves this: it re-decides **which branch** only every
`BOT_TICK_TIME` (~1s), but **re-ticks the chosen branch every frame**. So the
*decision* is throttled while the *execution* stays smooth. The per-bot state it
needs (in `BotDecision`):
```cpp
struct BotDecision {
    float timer;      // time since last decision
    float interval;   // this decision's length (jittered, so bots aren't in lockstep)
    int   activeBranch; // which child index is currently latched
};
```
Each ~1s it runs a normal selector pass to pick the first non-Failure child,
stores that index, and for the rest of the interval just ticks that child. (It
also re-decides early if the latched branch suddenly fails — e.g. its guard
condition flipped.)

**This latch behaviour is the crux of the `Chance` design in §8 — keep it in
mind.**

## 7. Personalities & difficulty (BotProfile)

Bots aren't identical. Each carries a `BotProfile`:
```cpp
struct BotProfile {
    float aggression; // 0 timid/kites .. 1 reckless/in-your-face
    float accuracy;   // 0 sprays .. 1 snaps on target
};
```
Assigned **once at spawn** in `startGame`, seeded from `player.id` so a given bot
plays consistently. A global `BOT_DIFFICULTY` sets the *center*; each bot is
`difficulty ± spread`, and `spread` grows with the number of bots (a lone bot ≈
the difficulty; a crowd fans out into varied personalities).

Nodes then read the profile to scale the baseline `BOT_*` constants — e.g.
aggressive bots close in nearer (`MoveToTarget`), tolerate more damage before
fleeing (`IsLowHealth`), and take riskier asteroid shots (`AttackAsteroid`);
low-accuracy bots get random aim spread (`steerYawToward`). Unpredictability also
comes from per-decision interval jitter.

## 8. FUTURE ADDITION — the `Chance` node (probabilistic decisions)

**Status: designed, not yet implemented.** Documented here so the reasoning isn't
lost.

### The goal
Make a choice a coin flip: when the bot picks a new action, a `P` chance it takes
branch A, `1 − P` it takes branch B. E.g. when hurt, sometimes retreat, sometimes
stand and fight — so it's not perfectly predictable.

### The trap (why the obvious version breaks)
The obvious approach is a decorator that runs its child with probability `p`, else
returns Failure:
```cpp
Status tick(bb) { return RandomFloat(0,1) < p ? child->tick(bb) : Failure; }
```
Two problems, both from things we already established:
1. **It re-rolls every frame.** Under `LatchedSelector` (§6) the latched child is
   re-ticked ~60×/s, so this coin flips 60 times a second → the branch flickers
   on and off instead of committing.
2. **You can't cache the roll in the node.** The tree is shared across all bots
   (§5), so a member variable storing "this bot's roll" would be shared by every
   bot.

### The fix — roll only at the decision boundary, using a stateless hash
The insight (which prompted this doc): the roll should happen **only when a new
decision is made**, and its result held for the interval. `LatchedSelector`
already has exactly one spot that runs once per decision. So:

1. Add a **decision counter** to `BotDecision`:
   ```cpp
   uint32_t epoch = 0; // ++ each time LatchedSelector re-decides
   ```
   This is the "a new decision is happening" signal.

2. Make the roll a **deterministic hash** of `(epoch, bot.id, nodeSeed)` rather
   than a stored random draw — so it needs *no* stored state:
   ```cpp
   inline float hashToUnit(uint32_t x) {        // -> [0,1)
       x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15;
       x *= 0x846ca68bU; x ^= x >> 16;
       return (x >> 8) * (1.0f / 16777216.0f);
   }

   template <typename TargetT>
   class Chance : public Node<TargetT> {
       float p; uint32_t seed; Node<TargetT>* child;
   public:
       Chance(float prob, uint32_t nodeSeed, Node<TargetT>* c)
           : p(prob), seed(nodeSeed), child(c) {}
       Status tick(Blackboard<TargetT>& bb) override {
           uint32_t mix = bb.decision.epoch * 2654435761u
                        ^ bb.bot.id          * 2246822519u
                        ^ seed               * 3266489917u;
           if (hashToUnit(mix) < p) return child->tick(bb);
           return Status::Failure;
       }
   };
   ```

### Why this works — trace it against §5 and §6
- **No flicker.** Between decisions `epoch` is constant → the hash is constant →
  the same pass/fail every frame → the latched branch is stable. New decision →
  `epoch++` → a genuinely fresh roll.
- **No shared-state bug.** `bot.id` is in the hash, so each bot rolls
  independently with zero stored state. `seed` makes multiple `Chance` nodes
  independent of each other.
- **A latched branch is never a *failed* `Chance`.** If a roll fails, the node
  returns Failure *during the selector's decision pass*, so `LatchedSelector`
  simply moves on and latches a different child. It only ever latches a branch
  whose `Chance` passed — and that passing outcome is what's held for the
  interval.

### How you'd wire it
Wrap a branch and place it in the `movement` selector. Example — sometimes evade,
sometimes fight on:
```cpp
Chance<Player> maybeEvade(EVADE_CHANCE, 1u, &lowHealthResponse);
LatchedSelector<Player> movement({ &maybeEvade, &lowFuelResponse,
                                   &moveToTarget, &idle });
```
`P(evade) = EVADE_CHANCE` when low-health triggers; otherwise it falls through to
attack-move. The probability could be a plain constant, or personality-scaled —
e.g. `p = EVADE_CHANCE * (1 - aggression)` so bolder bots flee less often (this is
also a natural spot for a future third trait like "caution").

### Checklist if/when implementing
- `BotDecision`: add `epoch`; increment it in `LatchedSelector`'s decision block.
- `bot_logic.h`: add `hashToUnit` + the `Chance` decorator.
- `main.cpp`: wrap a branch in `Chance` inside the `movement` selector.
- `constants.h`: add the probability constant.
- Verify: over repeated triggers the bot sometimes A, sometimes B; and it does
  **not** flicker mid-interval (proves the roll is latched, not per-frame).

## 9. Mental-model recap
- Tick the tree each frame; nodes return Success / Failure / Running.
- `Selector` = priority fallback, `Sequence` = gated AND, `Parallel` = do-both.
- Nodes write *intent* to `PlayerInput`; `ApplyPlayerInput` does the acting.
- One shared tree → nodes are stateless → per-bot memory lives in the Blackboard.
- `LatchedSelector` commits a decision for ~1s but keeps executing every frame.
- Randomness that must "stick" for a decision keys off the decision `epoch`, not
  per-frame rolls.
