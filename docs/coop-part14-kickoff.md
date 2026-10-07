# Co-op part 14 hand-off: issue #11, item identity, and the two-player tracker

Written 2026-10-07 at the close of a two-day, ten-round, two-machine session (2026-10-06
afternoon to 2026-10-07 01:30). `docs/coop-plan.md` "Player issue #11" and every section
after it are the derivation; this file is where the work stands and what is owed.
Logs and captures for every round are in `~/DR2CZ-troubleshooting/issue11/`.

## 0. What is ON BY DEFAULT now (`6205cbf`, the operator's instruction)

| arm | what it fixes | verified |
|---|---|---|
| `CZ_COOP_ITEM_SYNC` | each machine publishes its own player's held item and whole bag over the coop link; a remote placement's mission event takes the owner's answer; the found test takes the owner's word | engaged every round; the tracker below reads it |
| `CZ_COOP_CONDITION_ANY_PLAYER` | mission condition 10 ("player does not carry X") asks every player in co-op, so both machines spawn the same items and their item handles stay in step | round 5: one `[cond]` answer, every handle in step after it |
| `CZ_COOP_TRACKER_ALL` | the "Find Bike Parts" tracker shows the partner's parts: the bring-item objective's one player lookup answers with the carrier | round 10, operator: *"it works great"*, both directions in both logs |

`=0` is the control on each; each acts only in a co-op session. Already default from
before: `CZ_COOP_RESPONSE_PLAYER`, `CZ_COOP_FOUND_ANY_PLAYER`, the outfit check and repair.

## 1. Owed, in order

1. **czwin has not pulled `6205cbf`.** It dropped off the network at the flip. `git pull` +
   build before the next co-op round (its `play.bat` sets the three arms explicitly, so the
   build it has behaves the same; the pull is for the default and the docs).
2. **The pawnshop wheel can still be taken twice** while the guest carries one through a
   zone change. Single player does not do this (operator's control: a carried wheel never
   respawns, a misplaced one does). The respawn is `GotBikeWheel` (condition 9, true while
   `PrologueWheelObjective` is in state 1), and the found test that drives that state is
   re-asked during a level load, when every co-op bag is cleared and rebuilt. Candidate:
   `CZ_COOP_SPAWN_CARRIED=1` (OFF), whose found-hook half accepts a per-player bag snapshot
   from before the load and prints `[found] RESCUED BY THE BAG BEFORE THE LOAD`. **It has
   never engaged**: the round that would test it was spent on the tracker. The test: the
   guest carries the wheel out of town and back in; no second wheel at the pawnshop on
   either machine. Its other half, a hook on the spawn action `sub_823A5238`, is MOOT for
   the bike parts: their definitions are not "unique" (`+0xB9` = 0, printed by `=3`), so
   the title never applies its carrier check to them. Leave it or delete it once the found
   half is measured.
3. **Other per-machine spawn differences** (a zombie's drop, a prop broken on one side)
   would shift the item serials exactly like the gas can did. `CZ_COOP_PICKUP_TRACE=1`
   prints the handle and the call chain on every remote pickup; leave it in the test
   launchers until a long session shows no mismatch.
4. The launchers (`tools/issue11_host.sh`, czwin `C:\cz\play.bat`) still set the arms and the
   traces explicitly. Trim them when the wheel is closed.

## 2. What was learned, in one place

- **An item is named on the wire by a serial at `+0x9C`**, resolved by `sub_821A2250`
  against the RECEIVER's pool (bit `0x40000000` = a different table). It is NOT the pool id:
  the two coincided in a fresh single-player world (149 of 149) and diverged in co-op
  (handle `0x426` = pool 1061 on the host, 1060 on the guest). The remote-pickup lookup is
  `0x82583484` in `sub_82583258`, under the network receive chain.
- **The drift's source was a per-machine conditional spawn.** `PrologueCase1-Start22`
  (LEVEL_PROLOGUE) spawns `GasCan7` only under condition 10, then the engine, the forks
  and the shed key. Condition 10 (`sub_823A7530` case `0x823A76E4`) reads ONE player's bag.
  A guest walking back in with the can skipped it on his machine only, and every serial
  after it was one apart: host engine = guest forks; guest gas can = host shed-key grant.
- **Every co-op bag is cleared and rebuilt on a level load**, on both machines. Anything
  evaluated in that gap sees nobody carrying anything.
- **The tracker** is the mission HUD with vtable `0x8207436C` (set-up `sub_82523CB0` ->
  `sub_82509B28`, tick `sub_825322C8`, message handler `sub_8250A8B8`). Icons at
  `+0x300 + i*12` (gas, engine, handlebar, fork, wheel), the `w_locked` overlay at `+0x304`,
  the part's objective MISSION at `+0x308`. `sub_824E1A80` decides every frame: lit iff
  the mission is active and the current objective's waypoint (`sub_821AD5E8` -> vt[0x44])
  is the bike, (-269.777, 3.279, -60.447). That vt[0x44] is
  `cMissionObjectiveBringItem` `sub_823E7420`, which looks in the LOCAL player's bag only
  (`GetUserPlayer` at `0x823E744C`). The fix answers that one lookup with the carrier.
- **Dead ends, do not re-read:** `sub_82243060` is the HUD's message dispatcher, not
  "ObtainItem"; message `0x7D` is a per-part online-stat notice (flags `online+0xAFC..`);
  `sub_8250BBA0` picks the inventory-slot icon; `sub_82523A80` is the NEIGHBOURING HUD class
  (vtable `0x82074320`); bit `0x00800000` of a widget's `+0x10` is SHOWN, and an icon's
  fade-in is its `cFEAnim` child's trigger (mask `0x80`, `vt[0x4C]`/`vt[0x50]`). Three
  rounds were spent driving the icon directly; the title recomputes it every frame, so
  only the decision could be fixed. The operator said so before the measurement did.

## 3. Instruments added

`CZ_COOP_PICKUP_TRACE=1` (call chain + last handles resolved on every insertion and key
grant; `=2` checks the chain walker on one machine), `CZ_COOP_INVQUERY_TRACE=1`, the
`[tracker]` icon-state log and widget set/clear trace (under `CZ_COOP_TRACKER_ALL`),
`CZ_COOP_SPAWN_CARRIED=3`. All in `docs/instruments.md`.

## 4. Session mechanics that cost time

- A signed-out XenonLive launcher leaves `~/.config/XenonLive/session.json` with EMPTY
  tokens; the game then boots signed out on the `default` save without complaint. Grep
  `signed in as` in both logs after every launch.
- Never run a headless check while the operator's game is open (done once this session by
  mistake; a `pgrep ... && echo skip` does not skip).
