# Co-op part 10 kickoff — issue #9, with the response read and one line left to measure

> **SUPERSEDED, 2026-09-27, by `docs/coop-part11-kickoff.md`.** The operator ran the
> two-machine session §1 asks for and it CONFIRMED the reading: the response runs as
> player 0 while the acting player is 1. The fix is in and on by default; the prop-lookup
> lead (§1 line 2) is REFUTED — one candidate every time. Read part 11 first; this is kept
> because its §2 reading of the chain is what the session was built on and still stands.

**Read `docs/coop-part9-kickoff.md` first** (it is still the authority on the four refuted
mechanisms — do not re-buy any of them), then `docs/coop-plan.md`'s
*"Player issue #9, part 10"* section, which is this session's record. Written overnight on
2026-09-26 with the operator asleep, so **nothing in it has been seen on two machines**.

## 0. The one-paragraph state

Part 9 said: stop inferring, hook the removal, print `lr`. Done — and the response chain is
read end to end now, which turned one question into two, both of which the operator's next
single placement answers on one line each.

**The response to `WheelPawnPlaced` runs through a NETWORK EVENT.** Every static caller of
the mission-action dispatcher `sub_82378FA0` — there are three, where part 9 recorded none —
is a replicated-event listener (classes `0x6A`, `0x6B`, `0x6C`), and the player index the
action reads comes out of the message: for class `0x6C` literally,
`sub_8248BE28(ctx, .., 0, event->0x10)` storing `+0x10 = event->0x10`. The first **state 34**
this project has ever observed read `ctx+0x10 = 0` — not the type tag 9 that part 9's
candidate 3 was built on, which is now refuted by direct measurement rather than by census.
**Which of the three classes delivers it is NOT established** — the line that appeared to say
so came from a hook that had failed to be added, so its `0` was an initialiser and not a
measurement (retraction in the plan section). Both hooks are in now, each with a `hook alive`
line.

**And the response's last action cannot pick the right prop.** It is
`cMissionSendCommandToProp { PropCommand = "17" PropName = "WheelPawn" }`; that action
(`sub_82408908`) resolves the name through `sub_821A2200`, a nine-instruction linear scan of
the item pool's 2,048-entry object table returning **the first live entry whose instance name
matches**, and command 17 then **takes the prop off whatever holder it landed on and releases
it into the world with `actor = 0`**. A census of that pool in single player, standing at the
bike, found **four instance names already naming more than one live entry** (`Nails` x5,
`ChuckWalkieTalkie` x4, `fe_watch` x4, `WrenchLarge` x2).

Either of those accounts for all three of the operator's symptoms. **They are independent,
and the same run reads both.** Nothing here has been seen on two machines.

## 1. THE ONE THING TO DO FIRST — it costs the operator one placement

Have them run **both** machines with the trace on and place ONE bike part as the guest. Two
lines decide it, and they are in the same log.

### Line 1 — whose Chuck is the response running for?

```
[place] BATCH class 6B ... event ........, N record(s), from lr ........
[place]   record 0 at ........: class 6A, +0x10 = N (the player the action runs AS), +0x14 = ........
[place] STATE 34 ... ctx+0x10 = N, that is player N ... dispatched from 821899B8 (event class 0x6B)
```

The dispatch path is measured: the response's actions arrive as a **class-0x6B batch** of
0x1C-byte class-0x6A records, and each record carries its own player index at `+0x10` — the
field `cMissionSetChuckState::Execute` hands to `GetUserPlayer`. The record constructor
(`0x821619D0`) initialises it to **4**, a "nobody" sentinel out of range for
`MAX_USER_PLAYERS`, so whatever value shows up was WRITTEN by whoever queued the action.

* **`N == 1` on the host** — the response is running for the right Chuck. Go to line 2.
* **`N == 0` on the host** — the response is running for the LOCAL Chuck: the animation plays
  on the host and the item comes out of the host's hands, which is the operator's third
  symptom exactly. **The defect is then that one field of that one record**, and the next step
  is to find which of the 56 sites that build such a record built this one (the `lr` on the
  BATCH line plus the record index narrows it).

### Line 2 — did the destroy command pick the right prop?

```
[place] PROPFIND hash 878FC97B (WheelPawn) for PROPCMD 17 "WheelPawn" -> ........,
        N live pool entries match [id .. obj .. holder .. itemHash ..] ...
```

* **`N == 1`** — the lookup was exact, so §2 of the plan section is refuted in one line and
  the `holder` field says whose part the one candidate was.
* **`N >= 2`** — it is named. The list says which one the title took and whose hand each was
  in, and `CZ_COOP_PLACE_FIX=1` is worth trying in the same sitting.

### The command line, both machines

```
CZ_ITEM_TRACE=1 CZ_COOP_PLACE_TRACE=1 CZ_COOP_POOL_CENSUS_MS=3000
```

on top of whatever the co-op launch already sets. The joiner's own copy of the response
returns early (the host-only gate at `0x823E790C`), but its pool census is what says whether
the two machines' pools have drifted, and its `STATE 34` line — if it prints one — says
whether the event reached it with the same index.

**Two more lines, neither ever seen:**

| line | what it settles |
|---|---|
| `[place] REMOVE player N ... loses item ... from lr ........` | **part 9 §3.1's exact request.** Whichever inventory loses an item names the bug, and the `lr` names the caller the call graph could not |
| `[place] STATE 35` / `[place] DROP-ALL` | state 35's handler (`0x8240A9C0`) resolves its actor from **`world->0x80`**, a world-global player index, and empties that Chuck's whole bag. If a placement reaches it, that field is the defect; if it does not, the candidate is closed by measurement |

## 2. What is established, with the instruction that states it

- **`cMissionSendCommandToProp::Execute` = `sub_82408908`**, from the vtable (slot 14 of
  `0x8204C870`, against `0x82409900` at slot 14 of `cMissionSetChuckState`'s `0x8204D390`),
  confirmed live by the hook printing `PROPCMD 22 on prop named "Bike2"`.
- **`PropCommand 17` = detach + release.** `0x8240916C` gets the holder, `0x82409194` calls
  `game->vt[0x38C](game, holder, prop, 0)`, `0x824091B4` calls `sub_8223BBB8(mgr, 0, prop,
  ...)`. No actor argument anywhere.
- **`sub_821A2200(mgr, nameHash)` returns the lowest-id live match** out of
  `mgr + 0x30 + id*4` — the item pool's own object table.
- **A prop's `+0x98` is its INSTANCE name and `+0x100` its item TYPE.** Measured:
  `PropName = "Bike2"` resolved `+0x98 == hash("Bike2")` with `+0x100 == hash("BikeBody")`,
  exactly as `cMissionSpawnItem Bike2 { ItemName = "BikeBody" }` declares.
- **No spawn action in `missions.txt` is named after a bike part**, so the five destroy
  commands can only be pointing at the generic instance a player carries — and generic
  instances (`+0x98 == +0x100`) demonstrably exist and demonstrably duplicate.
- **`Inventory::RemoveItemAt` = `sub_821A75B8`**, the mirror of `sub_821A7550`, hooked.
- **The state jump table re-verified against the image**: 77 entries at `0x82043388`, base
  `0x82409954`, index = `state - 1`. State 61 -> `0x8240AF7C`, state 34 -> `0x8240A930`
  (both as previously recorded), **state 35 -> `0x8240A9C0`**.
- **All three static callers of `sub_82378FA0` are network event listeners** (classes
  `0x6A`/`0x6B`/`0x6C`), and `sub_8248BE28` is three stores that put `event->0x10` into the
  action context's `+0x10`. Part 9's "no static callers" is corrected.
- **The objective-event context is NOT what reaches the action.** `sub_821AD238` writes 11 at
  `+0x10`, `sub_8248B838` overwrites it with 9, and the observed state 34 read 0.
- **`cMissionOnTrigger::Update` is entered ONCE in the safehouse** — so the inventory watch
  has been armed and silent there since co-op part 1. See §5 of the plan section.

## 3. What is NOT established

- No two-machine reading of any of it.
- The generic-instance claim is an inference from three measured facts, not a watched
  pickup. Watching one costs nothing now: `CZ_COOP_PICKUP_TRACE=1` plus the census.
- `CZ_COOP_PLACE_FIX` has never engaged. It ships OFF for that reason.

## 4. If §1 comes back `N == 1`

Then the response found exactly one candidate and the wrong-Chuck effect is upstream of the
lookup. The two live leads, in order, and both are already instrumented:

1. **state 34's player index** (`[place] STATE 34 ... ctx+0x10 = N`). If it is not the
   acting player, that is the defect and part 9's candidate 3 was right about the field and
   wrong about the value.
2. **`world + 0x80`** via state 35 / `DROP-ALL`.

And if neither line appears at all, the response is not reaching this machine's effect path
and the next question is the deferral (part 9 §2, link B) — which the `lr` on the `PROPCMD`
line already answers in part: it read `82378FFC`, i.e. `sub_82378FA0`, the action dispatcher
that has no static callers.

## 5. The route that exists now

**`DOWN` twice on the DebugJump screen selects Case 0-4** and lands Chuck two metres from
the bike, in the safehouse garage, headless:

```
CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000 \
CZ_FAKE_PRESS_SEQ='F2,START,WAITJUMP,NONE,DOWN,DOWN,A,NONE' \
CZ_ITEM_TRACE=1 CZ_COOP_PLACE_TRACE=1 CZ_COOP_POOL_CENSUS_MS=3000 \
CZ_COOP_RAISE_EVENT='WheelPawnPlaced@15' timeout 420 ./cz_runtime
```

Two dead ends recorded so they are not re-tried: **there is nothing to pick up in the
garage** (the five interactable parts spawn in `LEVEL_PROLOGUE`; the ones at the bike are
`NonInteractableProp = "true"`), and **`CZ_AUTOCHUCK="MISSION MASTER"` does not move Chuck
there at all** — fifteen minutes left him on the spawn point. `CZ_COOP_RAISE_EVENT` exists
because of those two.
