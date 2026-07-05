# References vs pointers (a plain-language rundown)

Written up from a real bug in `messages.h`: storing `Player& playerA` inside
`Message` refused to compile, but returning a reference from
`MessageQueue::getMessages()` was fine. Same keyword, opposite outcome. Here's
why.

## The two mental models

**A reference is a weld.** When a reference is born, you weld it to one specific
object, and that weld is permanent — you can never grind it off and re-attach it
to something else. `Player& playerA = bob;` means *"playerA IS bob, forever."*
There's no such thing as "point playerA at alice now."

**A pointer is a sticky note with an address written on it.**
`Player* playerA = &bob;` means *"this note currently says: go to bob's house."*
You can cross it out and write alice's address instead. You can also leave it
blank (null). It's re-writable.

So the one question that explains everything: **after this thing is set, do you
ever need to re-aim it?**

- Reference: you can *never* re-aim it.
- Pointer: you can re-aim it anytime.

## Why the reference in `getMessages()` is fine

There, the reference is a **fleeting alias** — you grab it, use it, throw it away:

```cpp
for (Message& msg : messageQueue.getMessages()) { ... }
```

`msg` is welded to one message for exactly one trip through the loop body, then
the next iteration makes a *fresh* `msg` welded to the next one. You never ask a
single `msg` to switch from message #1 to message #2 — the loop just makes a new
weld each time. **No re-aiming ever happens, so the "can't re-aim" rule never
bites you.** Same with passing `Player&` *into* a function: the alias lives for
one call and vanishes.

## Why the reference *inside* `Message` broke

A member reference isn't fleeting — it's **welded in permanently, baked into the
object's identity for the object's whole life.**

Remember what `std::vector::erase()` does: to delete a message from the middle,
it **slides the later messages down** to fill the hole — i.e. it *overwrites*
slot #4 with the contents of slot #5. "Overwrite this Message with that one"
means, among other things, *"playerA, stop being about Bob and start being about
whoever slot #5's playerA was."*

But playerA is a **weld**. You just asked it to re-aim. Illegal. So the compiler
declares the whole `Message` un-overwritable (the error phrase is *"copy
assignment operator is implicitly deleted"*), and since `erase` depends on
overwriting, `erase` won't compile.

The trap: it's the **exact same `Player&`**, but

- as a *loop variable / return / parameter* → a temporary alias, never re-aimed → fine.
- as a *stored member* → a permanent weld inside something that later gets overwritten/moved → fatal.

It's not "references are bad." It's **don't weld a permanent alias inside an
object that has to be reshuffled.**

## What a pointer would've done instead

If `Message` had held `Player* playerA`, overwriting a Message would just
**rewrite the sticky note's address** — perfectly legal, pointers re-aim all day.
So `erase` would've compiled. That's why "switch to `Player*`" was one of the
candidate fixes.

But a pointer drags in two hazards a reference doesn't have:

- **It can dangle.** The note still says "go to Bob's house," but Bob's house got
  bulldozed (the `players` vector reallocated and moved Bob elsewhere). Follow the
  note → you're reading rubble. Crash or garbage.
- **It can be null.** You have to remember to check "is this note even filled in?"
  before following it.

## Why storing the *name* beat both

By storing `std::string playerA_Name`, the Message stops pointing at Bob at all —
it keeps **its own photocopy of Bob's name**. Strings can be freely overwritten,
so `erase` works. And there's nothing to dangle or be null — even if Bob gets
bulldozed, the message still holds the name it needs. That's why by-value won: it
sidesteps the weld problem *and* both pointer hazards in one move.

## Cheat sheet

| | Reference (`Player&`) | Pointer (`Player*`) | Value (`std::string name`) |
|---|---|---|---|
| Re-aimable after set? | Never | Yes | N/A (owns its own data) |
| Can be null/empty-ish? | No | Yes (must check) | No |
| Can dangle? | Yes (rarely, if referent dies) | Yes | No |
| OK as a fleeting alias (loop/param/return)? | Yes | Yes | — |
| OK stored in something that gets erased/moved? | **No** | Yes | Yes |

The short version: **use a reference for a "use it right now and let go" alias;
reach for a pointer (or just a copy) the moment the thing needs to be stored,
re-aimed, or absent.**

## Bonus: when to use `.` and when to use `->`

This falls straight out of the weld-vs-sticky-note idea above.

- **`.` (dot)** — you're holding the **actual thing**. Reach in directly.
- **`->` (arrow)** — you're holding a **sticky note with an address** (a pointer).
  You have to **go to the address first**, *then* reach in. The arrow shape even
  looks like it's travelling somewhere to get there.

That's it. `.` = "it's right here." `->` = "follow the note, then grab."

### They're the same move, one with an extra step

`->` is just shorthand. These two lines mean the exact same thing:

```cpp
explosion.owner->id
(*explosion.owner).id
```

The `*` means *"follow the sticky note to the actual house."* Once you're standing
at the house, you use a plain `.` to reach in. `->` just bundles "follow the note"
+ "reach in" into one arrow so you don't have to write `(*...)` every time.

### How it maps to the models above

- **Actual object** → it's right here → `.`
  ```cpp
  Player player;
  player.fuel;        // player IS the thing
  ```
- **Reference (a weld / nickname)** → *still* `.`, because a nickname behaves
  exactly like the real thing — there's no address to follow, it just *is* Bob.
  ```cpp
  Player& msg = ...;
  msg.playerA_Name;   // reference → dot, same as a plain object
  ```
- **Pointer (a sticky note with an address)** → `->`, because you must follow the
  address first.
  ```cpp
  Player* localPlayer = ...;
  localPlayer->name;  // pointer → arrow
  ```

Punchline: **references use `.` (they *are* the thing); pointers use `->` (they
*point at* the thing).**

### Spotting it in this codebase

`collisions.cpp` uses both all over:

```cpp
space.emitAudio(...);                        // space is a reference  → dot
explosion.position;                          // explosion is an object → dot
explosion.owner->id;                         // owner is a Player*     → arrow
explosion.owner ? explosion.owner->id : 0;   // check the note isn't blank (null) BEFORE following it
```

That last line is the pointer dangle/null hazard in the wild: `owner` is a sticky
note that *might be blank* (null), so the code checks `explosion.owner ?` first —
because following a blank note (`->` on a null pointer) is the "reading rubble →
crash" case.

### The quick test when you're unsure

Ask: **"Do I have the thing, or an address of the thing?"**

- Have the thing (object or reference) → `.`
- Have an address (pointer) → `->`

If the compiler barks *"member reference type ... is a pointer"* or *"... is not a
pointer; did you mean '.'?"*, that's it telling you that you picked the wrong one
of the two. Just swap.
