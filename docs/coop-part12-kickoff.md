# Co-op part 12 — the FOUND flag is named, and one round closed it

**STATUS: ISSUE #9 IS CLOSED (2026-09-29).** Both halves fixed, both ON by default, both
operator-verified on two machines. `coop-plan.md` §12.10-§12.12 is the record of the rounds
and of the two predictions this document made that measurement had to retract. Nothing here
is open work; it is kept as the derivation and as the map of the guest structures in §1.

**Supersedes `coop-part11-kickoff.md` on "where issue #9 is".** Part 11's rule still
stands and is the reason this part exists: *do not build the fix before the flag is found
by measurement.* The flag is now found by measurement. `docs/coop-plan.md` §12 is the
derivation with every number; this is the hand-off.

## 0. Where issue #9 is, in four lines

* **The PLACEMENT half is FIXED and operator-verified.** `CZ_COOP_RESPONSE_PLAYER` is ON
  by default (`=0` the control). Do not re-open it.
* **The FOUND half is DIAGNOSED.** A bike part is marked found when the prerequisite of
  the mission `Prologue<Part>Objective` — a `cMissionObjectiveGiveItemToNPC` naming the
  item — sees it in an inventory, and that test looks in exactly ONE inventory: the LOCAL
  player's (`GetUserPlayer(world->0x7C, world->0x80)`, index 0 on the host).
* **`CZ_COOP_FOUND_ANY_PLAYER` FIXES IT AND IS ON BY DEFAULT** (`=0` the control).
  Operator-verified on two machines, 2026-09-29 — `coop-plan.md` §12.10 is the record of
  both rounds. §2 below is the round as it was asked for; it has been RUN and every
  predicted line appeared.
* **What is open is no longer about bike parts**: §4's prediction, which costs one pickup.

## 1. What already exists, so it is not rebuilt from the plan text

This is the section a kickoff is most useful for. All of it is in
`runtime/kernel/coop_items.cpp` unless stated.

| thing | what it is |
|---|---|
| `CZ_COOP_MISSIONWATCH=MS` | diffs the mission manager's mission table (`+0x4D8`, count `+0x10AC`) **by name** plus the six list counts §11 measured. Names come from a CENSUS of the definition's offsets, not a guess |
| `CZ_COOP_MISSIONWATCH_NAMEOFF=0xNN` | pins the name offset if the census picks the wrong member. Measured value is `+0x1C` |
| `CZ_COOP_OBJTRACE=1` | hooks **all seven** class-specific vtable slots of `cMissionObjectiveGiveItemToNPC` and counts them; for slot 20 prints the `ITEM_NAME`, the answer, `+0xA8`/`+0x7C`/`+0xA4`, and the player index it asked `GetUserPlayer` for |
| `CZ_COOP_FOUND_ANY_PLAYER=1` | THE CANDIDATE FIX, shipped OFF. Re-runs the test once per other user player when the local player said no. Six guards; its own `[found] RESCUED` line is the diagnosis |
| `tools/find_field_access.py` | census of every D-form load/store touching a structure offset, with the enclosing function |
| `tools/issue9_found_host.sh` | the host leg of §2's round, one command. `FIX=1` arms the candidate |
| `CZ_COOP_FLAGHUNT`, `CZ_COOP_POOL_CENSUS_MS`, `CZ_COOP_PLACE_TRACE`, `CZ_COOP_RAISE_EVENT` | part 10/11's instruments, all still there |

Guest-structure facts established, so they are not re-derived:

```
missionMgr = world->0x78->0x5C
  +0x04D8   array of mission pointers      +0x10AC  its count (max 0x158)
  +0x0A3C   the PENDING-START queue        +0x0FA0  its count
  +0x1408 / +0x15F4 / +0x17E0  three identical 0x1EC-byte list units,
            each an array of 0x79 pointers then its counts at +0x1E0/+0x1E4/+0x1E8
mission (0x38 bytes, allocated by sub_821AEC40)
  +0x08  STATE      +0x18  the DEFINITION      +0x1C  the world      +0x24  a start time
definition (0x140 bytes)
  +0x1C  the NAME, as an engine string
engine string: capacity byte at +0x20; below 0x1F the chars are inline, at or above
  it the object's first dword is a heap pointer
cMissionObjectiveGiveItemToNPC (0xAC bytes, ctor 0x823AF1B8, vtable 0x8204E540)
  +0x7C  a short-circuit byte   +0x80  the ITEM_NAME string
  +0xA4  the NPC               +0xA8  the satisfied LATCH
  slot 20 = sub_823AF418 is the test; six of its seven own slots fire, slot 18 does not
sub_82482AD8(world, idx) = world->0x7C then sub_8247B020, which for 0..3 is
  *(players + 0xC + idx*4).  ~440 callers image-wide, so "asks for the local player"
  is the engine's ordinary idiom and not by itself a defect
```

## 2. THE ROUND THAT CLOSED IT — RUN 2026-09-29, every predicted line appeared

```
host:   tools/issue9_found_host.sh
joiner: C:\cz\play.bat   (must carry the same two arms — see §3)
```

**The host picks up ONE bike part. The guest picks up a DIFFERENT one. NOBODY PLACES
ANYTHING.** Placing is not part of this test.

Predicted on the HOST's log, stated before the run so the run can say no:

```
[obj] ... ITEM_NAME "<the host's part>":  answer 0 -> 1, index 0
[obj] ... ITEM_NAME "<the guest's part>": answer 0, index 0, and never 1
[mw]  CHANGED ... Prologue<HostPart>Objective : state(+08) 0 -> N
      and NO such line for the guest's part
```

Three observations that refute the whole reading:

1. the answer never reaching 1 for the host's own part — then slot 20 is not the gate;
2. the index printed being anything other than 0 — then the test is not local-bound;
3. the guest's objective advancing anyway — then the prereq is not what gates the screen.

Then, and only if the round agreed, a second round with `FIX=1 tools/issue9_found_host.sh`.
The whole test there is whether `[found] RESCUED: "<the guest's part>"` appears and the
bike-parts screen turns that row green. `[found] consulted N time(s)` prints at 1 and every
power of ten, so an armed run that did nothing says how many times it was asked.

## 3. czwin's launcher — DONE, and what is still owed there

Patched 2026-09-29: `CZ_COOP_OBJTRACE=1` and `CZ_COOP_MISSIONWATCH=500` added, part 10's
`CZ_COOP_PLACE_TRACE` / `CZ_COOP_POOL_CENSUS_MS` / `CZ_COOP_FLAGHUNT` commented out rather
than deleted. `play.bat.pre-part12` is the pre-part-12 copy and `play.bat.pre-part10` the
original. **STILL OWED: take the two trace lines back out now that the measurement is done**
— they are diagnostics, not arms a player should carry. **Both machines must be on the same
commit** or the joiner's half of the log is empty; that has already happened once here.

## 4. A prediction that is not about bike parts, and why it is worth testing

**CORRECTED 2026-09-29 — see `coop-plan.md` §12.11.** The original form of this said all
eleven `cMissionObjectiveGiveItemToNPC` instances should be broken the same way, and named
the shed key as the cheapest refutation. The operator ran it: a guest picked up the shed
key and the host credited it with **no rescue and no pickup line**, because
`Key_MasterKey` is a KEY item (id 85038) granted by a type-0x13 network message rather than
picked up into an inventory. `items.txt` declares exactly two `KeyItemID`s — 85038 and
85001 Zombrex — so **both of those were always fine**, and the prediction should never have
included them.

What is left is **`Gems`** (`PrologueMoMoneyMoProblems`), the only non-key, non-bike-part
instance, and it is the better test anyway: its objective sits inside a `cMissionObjective`
with `NPCName = "srv_jemi"`, **not** inside a `cMissionPrereq` the way every bike part's
does, so it tests whether the repair is really in the CLASS. Reaching it needs
`PrologueWinSomeLoseSome` done, Jemi and Fausto both rescued, and 7:00; the gems spawn at
`-104.856, 3.276, -127.693`.

## 5. What is retired, so it is not re-bought

Everything part 11 retired, plus:

* **"Hook the store sites for the 31 offsets."** Refuted by census before it was tried:
  every counter among them has exactly four D-form store sites and all four are the
  mission manager's reset and constructor storing zero. The mutators are indexed and carry
  no displacement, which is also the real reason `+0x1648` looked like it had none —
  nothing special about that offset, the whole list family is invisible to a static grep.
* **"`+0x15E8`/`+0x17D4`/`+0x19C0` are listener-list heads."** Part 11's reading; corrected
  in §12.2. They are list COUNTS, and the 31 fields together are one object moving between
  four lists — two `erase(front)`s, two `push_front`s, and the counts either side.
* **The `ITEM PICKER` debug AI as a way to get a headless pickup.** The operator's verdict:
  it is too poor to reach a bike part. The positive control needs a human.
