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
2. **Diff a host pickup against a guest pickup** — both exist in
   `~/DR2CZ-troubleshooting/play/play_0927_0205.log`: the host's accidental gas canister
   (line 13389, that part ends GREEN) and the guest's wheel (line 248837, stays RED).
   **BUT TWO WAYS OF DOING IT WERE TRIED AND BOTH ARE DEAD — read this before repeating
   them:**

   * **`prologue_bikepart.tex` is NOT the discriminator.** It looked like one: the
     bike-part UI texture loads right after the host's pickup and not after the guest's.
     It loads after the **guest's wheel** too (line 248848, sixteen lines after the
     pickup). The first comparison had used the guest's *replicated copy* of the item the
     host had just taken, which is not a new part at all.
   * **A WINDOW DIFF BETWEEN AN EARLY AND A LATE EVENT IS CONTAMINATED BY EVERY ONE-SHOT
     LINE IN THE RUN.** Done properly (host's canister vs guest's wheel) the only
     game-relevant differences left were `hook alive:` and `... seen for the first time`,
     which are printed once per run by construction and therefore always land on
     whichever event happened first. Nothing survives that filter. The method cannot
     answer this question; do not spend another hour on it.
   * A first pass also found no `RaiseMissionEvent` within 300 lines of either, so the
     found flag is not a mission event.

   What is needed instead is a **purpose-built instrument**: find the found state itself
   (in the image, or by watching the store that sets it) and count it per pickup, per
   player. That is the same shape as the two things that worked this session.

3. **The decorative-prop idea is UNSETTLED, not refuted.** The harness raising
   `WheelPawnPlaced` solo produced state 34 and **no `WheelPilePawn2`** — with the census
   proven alive by 158 `APPEARED` lines before the raise, so the silence is real. But the
   harness is not a placement, and a `cMissionLevelReady` block may simply not re-fire
   mid-level, in which case the decorative prop never spawns from any placement and the
   bike's visual comes from elsewhere. **A real solo placement with
   `CZ_COOP_POOL_CENSUS_MS` on settles it in one round** and costs the operator two
   minutes; nothing should be built on the absence until then.
4. **Only then** consider a repair, and pre-register what it must move: the guest picks up
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
