# Co-op part 11 kickoff — issue #9: the player index is FIXED, the FOUND flag is not

**Read `docs/coop-plan.md`'s "Player issue #9, part 11" for the session record**, then part
10's §2 for the chain it rests on. Part 9's kickoff remains the authority on the four
refuted mechanisms — do not re-buy them, and note that a fifth (the prop lookup) is now
refuted too.

## 0. The one-paragraph state

Part 10 predicted that the mission's response runs for the wrong player. The operator's
two-machine session on 2026-09-27 **confirmed it line by line and by eye**: the guest
places a part, the host's `STATE 34` record says player 0, and the HOST's held item — a
Broadsword — comes out of his hands onto the floor. `CZ_COOP_RESPONSE_PLAYER` fixes it,
was verified twice on two machines, ships **ON by default**, and has both a positive and a
null control. **That is the end of that half.**

The other half is not the placement at all. The operator's capture of the bike-parts
screen shows `Bidon d'essence` **GREEN** and `Roue` **RED** on the same screen, with
***"Je n'ai pas encore TROUVÉ cette pièce"*** — *not found*, not *not placed* — and the
only difference between those two parts is that the host had accidentally picked the
canister up first. **So a part the GUEST picks up is never marked FOUND, and placing an
unfound part does nothing.** Same shape of bug one layer up: something counts only
player 0.

## 1. THE NEXT WORK — find what sets the found flag, by measurement

**Do not build the "fake a host pickup" fix first**, however well it fits. That is an
inference about *what* a host pickup registers, and inferring is what killed five
mechanisms; both of this session's wins came from watching a store.

In order:

1. **Does the part mission even start?** Each of the five part missions
   (`PrologueWheelPawn`, `PrologueGasCan`, …) starts on its `...Placed` event and half a
   second later spawns a NonInteractableProp of that part **at the bike**
   (`WheelPilePawn2` at `-268.983,3.380,-60.600`). `CZ_COOP_POOL_CENSUS_MS` now reports
   **APPEARED / GONE by name**, and those five names are in the hash table, so
   `[census] APPEARED 679E4F19 (WheelPilePawn2)` is the game's own positive statement that
   the wheel is on the bike. Absence of it after a correctly raised event says the part
   mission never started — which is a much narrower question than "the screen is red".
2. **Diff a host pickup against a guest pickup.** Both exist in
   `~/DR2CZ-troubleshooting/play/play_0927_0205.log`: the host's accidental gas canister
   and the guest's wheel, same item class, same session. Whatever a host pickup does that
   a guest pickup does not is the flag. A first pass found no `RaiseMissionEvent` within
   300 lines of either, so it is not a mission event.
3. **Only then** consider a repair, and pre-register what it must move: the guest picks up
   a part, the bike-parts screen goes green for it on **both** screens, and placing it
   credits the bike.

## 2. What is established

- The response's class-0x6B records carry **0** where the trigger's carry the acting
  player, and **0 is not "the local player"** — the joiner's `world+0x80` is 1 and its
  record still read 0.
- **The control pair is in one log, one process, one binary** (co-op vs solo placement),
  which is what isolates it.
- **The prop lookup is refuted**: 1 live candidate in every arm measured, co-op and solo.
  `CZ_COOP_PLACE_FIX` is aimed at the innocent half; do not quote it as a fix.
- **Part 9's owed control is answered**: solo works end to end, so the tracker was never
  broken for both players.
- The `RELEASE ... actor = 0` that leaves the part next to the bike is command 17 doing
  what it is written to do, and it happens identically in the solo run that works.

## 3. The arms

| arm | state |
|---|---|
| `CZ_COOP_RESPONSE_PLAYER` | **THE FIX, ON by default.** `=0` control, `=2` observe |
| `CZ_COOP_RESPONSE_PLAYER_TEST=N` | the positive control; **destructive**, bring-up only |
| `CZ_COOP_PLACE_TRACE=1` | the effect path; `STATE`, `REMOVE`, `RELEASE`, `PROPCMD`, `PROPFIND`, `BATCH` |
| `CZ_COOP_POOL_CENSUS_MS=N` | duplicates **and now APPEARED/GONE by name** |
| `CZ_COOP_RAISE_EVENT=NAME@SEC` | the single-machine harness; manufactures a mission event, never a gate run |
| `CZ_COOP_PLACE_FIX` | **refuted**, off, keep as a control |

`tools/issue9_host.sh` is the host leg in one command; `C:\cz\play.bat` on czwin is the
joiner's (patched 2026-09-27, `play.bat.pre-part10` is the original — **its two trace
lines are marked for removal once this is closed**).
