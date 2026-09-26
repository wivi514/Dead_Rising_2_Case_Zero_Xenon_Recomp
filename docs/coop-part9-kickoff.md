# Co-op part 9 kickoff — player issue #9, after four refutations

**Read this before `docs/coop-plan.md`'s issue #9 sections, because it says which of
them are dead.** Written at the end of the 2026-09-25/26 session, which produced no
working fix and a great deal of ground truth. Every refutation below came from the
operator's own two-machine runs, not from reasoning.

## 0. The one-paragraph state

Placing a bike part **as the guest** does not add it to the bike, drops the HOST's held
item instead, and leaves the part lying next to the bike. Everything works when the host
places. The *decision* side is measured correct on both machines across three sessions —
the host raises the right mission event for the guest's placement every time — so the
defect is downstream of the decision and something acts on the wrong player. **Four
candidate mechanisms have been refuted; do not re-buy any of them.**

## 1. THE REFUTATION LIST — do not re-buy these

| # | candidate | how it died |
|---|---|---|
| 1 | the acting player index is wrong (`CZ_COOP_TRIGGER_PLAYER`) | the index tracked correctly on BOTH machines, 7/7 then 4/4 then 3/3 |
| 2 | the two machines raise different mission events (`CZ_COOP_ITEM_SYNC`) | **both raised `WheelPawnPlaced` for the guest's wheel and it still did not attach.** Agreement is necessary, not sufficient |
| 3 | `cMissionSetChuckState` reads a context TYPE as a player, causing an out-of-bounds player lookup (`CZ_COOP_ACTING_PLAYER`) | **census: 5,820,000 `sub_8247B020` calls, 0 out of range.** Dead code; it can never fire |
| 4 | the item pool's id drift makes the far machine resolve the wrong item | **the operator's own control killed it**: they dropped the canister as guest and re-grabbed it as host, and BOTH cases produced the same id split (host 1043 / joiner 1041). A divergence present in the WORKING case cannot cause the failing one |

**The structural lesson, and the reason four attempts failed**: this engine's interesting
paths are VIRTUAL, so the static call graph dead-ends exactly where it matters —
`sub_82378FA0` (the mission action dispatcher), `sub_8224AE20`/`sub_8224AFB8` (the
give-item path) and `sub_8250A8B8` all have **zero static callers**. Every failure was me
filling that gap with an inference and shipping it. **Measure at the point of the write
and print `lr`; do not read upward.**

## 2. What is ESTABLISHED, with the instruction that states it

- **The placement decision is correct.** State 61 (`0x8240AF7C`) resolves the acting actor
  with `sub_8247B020(world->0x7C, playerIdx)`, gets the held item with `sub_821A6C18`
  (which is exactly `sub_8215D330` + the selected-slot arithmetic), compares five name
  hashes and raises. Measured right on both machines.
- **State 61 never removes the item and never attaches it.** Its effect path
  (`0x8240B094`) is `sub_821CF0E0(game->0xBC, 7)` then a vtable call with the string
  `BikePartPlaced` — **neither takes an actor.**
- **The response is DATA**, in the game's own `missions.txt` (`datafile.big`, line 6970):
  `cMissionObjectiveEvent GetWheelPawn` -> `cMissionSetChuckState ChuckState=34` (the
  place animation), an emote, then after 0.5 s `cMissionSendCommandToProp PropCommand=17
  PropName="WheelPawn"` — the prop is found **by name**, so pool drift cannot break it.
- **The response is HOST-ONLY** (`0x823E790C` gates the whole thing on `IsHost()`), and
  **the raise carries no player** (state 61 passes `param = 0`).
- **The response is DEFERRED, not synchronous.** `sub_823E7890` publishes a type-9 event
  object via `sub_82188488(..., &ev, __FILE__, 51)`; a call-stack-based fix measured zero
  engagements while the host raised the correct event.
- **Item pickup replicates CORRECTLY** — right item, both machines, three runs.
- **The pool ids drift by exactly 2** and each machine stably reuses its own. Real,
  measured, and causally irrelevant (see refutation 4).
- **Key items are a separate list** (`OBJETS CLÉS`), which is why the inventory trace was
  silent for the shed key. Exactly two exist: `Zombrex` (85001) and `Key_MasterKey`
  (85038). **A key item is granted by message type `0x13` with the id at `+8`** —
  `0x822430D8` / `0x822439DC`, read not guessed.
- **No key-item grant occurred during any captured pickup**: zero type-`0x13` messages
  across both machines. So the shed key did not arrive at the moment of the canister grab
  through this path.

## 3. THE NEXT WORK, in order

**3.1 Find what removes the item, by measurement.** This is the whole remaining question
for the placement bug, and it must not be answered by reading upward again. The shape that
works is the one that found `Inventory::InsertItemAt`: scan the image for the STORE that
does the thing, hook it, print its arguments and `lr`. For a removal, that is the store
that clears `inv + slot*8 + 4` and decrements `inv + 0x64` — `sub_821A7550`'s mirror. Hook
it, print which inventory and which actor, and have the operator place one part as guest.
**Whichever inventory loses an item names the bug in one line.**

**3.2 The `lr` from 3.1 identifies the caller** that the call graph cannot. Follow it once,
then stop and measure again.

**3.3 Only then consider a fix**, and pre-register what it must move: the guest places a
part -> the GUEST's Chuck animates, the HOST keeps his held item, the part is added.

**Do not** build another fix before 3.1 produces a log line. Four have been built on
inference and four were refuted.

## 4. The instruments that exist, and their state

| arm | state |
|---|---|
| `CZ_COOP_PICKUP_TRACE=1` | **live and verified.** Inventory inserts with the **pool id**, the player, the name hash and `lr`; plus the key-item grant decoded (type `0x13`, id at `+8`). Positive control: 11 inserts under `CZ_AUTOCHUCK="ITEM PICKER"`, 100% resolved to a pool id, ID 0 landing exactly on the pool base |
| `CZ_ITEM_TRACE=1` + `CZ_ITEM_WATCH_MS` | live; the bike path, the inventory watch, the broadcast-event decode |
| `CZ_COOP_ACTING_TRACE=1` | live; the `GetUserPlayer` census and objective-event nesting. **Its census is what refuted candidate 3** |
| `CZ_COOP_ACTING_PLAYER` | **REFUTED, dead code, off** |
| `CZ_COOP_ITEM_SYNC` | **REFUTED, off.** The channel underneath it (`coop_link.h`, a reserved port on the punched path) is sound and self-tested and is worth reusing if state ever must cross |
| `CZ_COOP_TRIGGER_PLAYER` | refuted long ago; kept as a control |

## 5. Operating notes that cost time to learn

- **`tools/name_hash.py --lookup <hex>` reverses any item hash** (`54D34287` = Shampoo,
  `5F8D0521` = GasolineCanister). Do not embed a table; reverse offline from the log.
- **`missions.txt` and `items.txt` are readable**: `tools/big_list.py --extract` out of
  `datafile.big`, then `tools/big_decompress`. The mission *response* to any event is data,
  not code, and reading it is faster than disassembling.
- **The `EXPLORER` AI never picks anything up** — a pickup instrument verified on that
  route reads 0 and looks dead. Use `CZ_AUTOCHUCK="ITEM PICKER"`.
- **An arm needs an unconditional counter.** Two two-machine runs printed nothing and were
  read as "mistimed" when the truth was "can never fire" (gotcha 151).
- czwin will not relink while its game is running (`permission denied` on `cz_runtime.exe`).
