# Load removal and auto-splitting — Dead Rising 2: Case Zero (Xenon Recomp)

**This is the document for speedrunners.** It is complete on its own: you do not need to
read anything else in this repository to write a load remover or an autosplitter against
this port.

It exists because of a problem a runner raised. Normally you find your signals by scanning
the game executable for a pointer path or a signature. That does not work here — this port
recompiles and relinks about 58,000 functions on most development days, so any offset you
find goes stale within days, and signature-scanning a 150 MB binary is slow and fragile.

So the game publishes the signals for you, at an address it promises not to move.

---

## Quick start

The game writes a 256-byte block at a **fixed address in its own process**:

| | |
|---|---|
| **Address** | `0x0000435A00000000` |
| **Magic** | the first 8 bytes are the ASCII `CZSPDRN1` |
| **Process** | `cz_runtime` on Linux, `cz_runtime.exe` on Windows |

Three bytes are probably all you need:

| Offset | Meaning |
|---|---|
| `0x38` | `isLoading` — 1 while the game is loading |
| `0x3A` | `isCutsceneExclusive` — 1 during a real, screen-taking cutscene |
| `0x68` | `cutsceneName` — 64-byte NUL-terminated ASCII, the cutscene that started **last** |

A minimal load remover is `isLoading || isCutsceneExclusive`.

**Check the magic before trusting the address.** If the 8 bytes at `0x0000435A00000000`
are not `CZSPDRN1`, either the build is too old or the block was relocated — see
[Troubleshooting](#troubleshooting).

### Which builds have it

Builds **newer than `v1.1.1`**. If the magic is absent on a current release, the feature
has not shipped yet — ask, and check the releases page. The game also states it on startup
in `cz_runtime.log` beside the game folder:

```
[speedrun] status block v1 at 0x435a00000000 (the documented fixed address), 256 bytes, magic CZSPDRN1
```

---

## The stability promise

This is the part that answers the original worry, so it is worth being precise about.

1. **The address does not move.** Not between builds, not between releases, not between
   Linux and Windows.
2. **Field offsets and meanings in version 1 never change.** New fields come out of the
   reserved tail at the end. If a field ever has to change meaning, the `version` word at
   `0x08` changes instead and the old meaning stays.
3. **If the address is ever unavailable** (nothing has been seen to take it, but a future
   driver or allocator could), the game maps the block elsewhere, says so in
   `cz_runtime.log`, and writes the real address to **`cz_speedrun_block.txt`** beside the
   game folder. A reader that checks the magic and falls back to that file cannot be caught
   out.

The repository treats this as a published contract rather than internal code, precisely so
that a tool compiled against it keeps working.

---

## Full layout

All values are **little-endian host-native** (x86-64 on both platforms). That is the host's
byte order — *not* the emulated console's, which matters only in the last section.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| `0x00` | `char[8]` | `magic` | `CZSPDRN1`, not NUL-terminated |
| `0x08` | `u32` | `version` | `1` |
| `0x0C` | `u32` | `size` | `256` |
| `0x10` | `u64` | `seq` | seqlock — **odd means a write is in progress** |
| `0x18` | `u64` | `frame` | presented frames since launch |
| `0x20` | `u64` | `monotonicNs` | host monotonic clock at the last update |
| `0x28` | `u64` | `guestBase` | host address of console address 0 — see [Reading anything else](#reading-anything-else) |
| `0x30` | `u32` | `gameState` | index into the game's own state table, `0xFFFFFFFF` if unresolved |
| `0x34` | `u32` | `gameStateId` | the game's internal name hash for that state |
| `0x38` | `u8` | `isLoading` | 1 while loading |
| `0x39` | `u8` | `isCutscene` | 1 during **any** cinematic — read the warning below |
| `0x3A` | `u8` | `isCutsceneExclusive` | 1 during a screen-taking cutscene. **Use this one.** |
| `0x3B` | `u8` | `isInGame` | 1 in `InGame` / `InGameTut1` / `GameShow` |
| `0x3C` | `u32` | `loadCount` | how many times `isLoading` has gone 0 → 1 |
| `0x40` | `u32` | `cutsceneCount` | how many cinematics have started |
| `0x44` | `u32` | — | reserved |
| `0x48` | `u32` | `guestFlowObject` | console address of the game manager (diagnostic) |
| `0x4C` | `u32` | `guestCineManager` | console address of the cinematic manager (diagnostic) |
| `0x50` | `u32` | `guestCurrentCine` | console address of the running cinematic, `0` if none |
| `0x54` | `u32` | — | reserved |
| `0x58` | `char[16]` | `gameStateName` | `"Loading"`, `"InGame"`, … NUL-terminated |
| `0x68` | `char[64]` | `cutsceneName` | the cinematic that started **last**, NUL-terminated. It *persists* after that cinematic ends |
| `0xA8` | `u8[88]` | — | reserved, zero |

### Reading it consistently

`seq` at `0x10` is a seqlock. The writer sets it odd, writes the body, then sets it even.

```
loop:
    s1 = read_u64(0x0000435A00000010)
    if s1 is odd: retry
    body = read(0x0000435A00000000, 256)
    s2 = read_u64(0x0000435A00000010)
    if s2 != s1: retry
    # body is now a consistent snapshot
```

In practice a plain read is almost always fine — the update is a single 256-byte store on
one thread, once per presented frame. But the retry costs nothing, and `cutsceneName` is
the field a torn read would visibly mangle.

`monotonicNs` is there to distinguish **"not loading"** from **"the game has stopped
updating"**. If it stops advancing, the game is hung or closed; do not read a stale `0` as
gameplay.

---

## Writing a load remover

`isLoading` is 1 for the game's own `FEToGame` and `Loading` states. Measured over two full
sessions including a complete playthrough, a Case Zero run has exactly **three** loads:

1. the boot into the main menu,
2. starting a game,
3. the ending's return to the front end.

**There are no loads during play.** Case Zero streams its single map, so walking between
the safehouse and Still Creek does not load. Two independent sessions confirm this.

If your timing rules differ about the boot logos — some runs count them, some do not —
ignore `isLoading` and build your own predicate from `gameState`. That is why the raw index
and hash are both published.

| Index | Name | `gameStateId` | Seen in play? |
|---|---|---|---|
| 0 | `None` | — | the slot the game never fills |
| 1 | `Startup` | `0x26344A65` | yes |
| 2 | `LegalScreen` | `0x267CDB6F` | yes — lasts about **7 ms**, so a 30 Hz poll will miss it |
| 3 | `BCGIntro` | — | never entered |
| 4 | `FrontEnd` | `0xD6BB036E` | yes |
| 5 | `FEToGame` | `0xB3876116` | yes — counted as loading |
| 6 | `Loading` | `0x817315A6` | yes — counted as loading |
| 7 | `InGame` | `0xA334C769` | yes |
| 8 | `InGameTut1` | — | never entered |
| 9 | `FEToGameShow` | — | never entered |
| 10 | `GameShow` | — | never entered |

Only those six ever appear, so you can treat them as the whole vocabulary.

---

## Splitting on cutscenes

### Use `isCutsceneExclusive`, not `isCutscene`

This is the one thing most likely to bite you, and it is measured rather than theoretical.

The game runs some **ambient** cinematics through the same system as real cutscenes — they
play in the world while you keep control. The most important one is **`workbench1`, the
combo-weapon crafting animation.** It sets `isCutscene` to 1 for about a third of a second
*every time the player builds a weapon*, which in a run is constantly. `625_pawncam` (the
pawnshop camera, about 6 s) behaves the same way.

`isCutsceneExclusive` is 0 for both. A load remover built on `isCutscene` would pause the
timer on every craft.

### Detecting a new cutscene

`cutsceneName` keeps its value after a cutscene ends, which is deliberate — it answers
"what played last". To detect a *new* one, watch **`cutsceneCount` (`0x40`)** rather than
the name, because the same cutscene can legitimately play twice in a session (restarting
the game replays `700_prologue_intro`).

### The cutscene names

`cutsceneName` is a script name without its `.txt`. There are two sources.

**`data/cinematics/cinematics.big` — 29 entries.** The story cutscenes, all exclusive:

```
700_prologue_intro                   709_leave_the_garage
701_chuck_arrives_in_town            710_ending_a
702_in_the_garage                    711_alternative_exit
703_roadblock_discovered             712_leaving_town
703a_zombrex_bike_found              713_ending_b1_katey_turns
704_making_a_list                    714_ending_b2_katey_turns
705_intro_to_queen_wasps             715_ending_c_quarantined
706_chuck_the_repairman              716_black
707_give_katey_zombrex_psycho_intro  716_nightdead
708_psycho_is_dead
```

plus the survivor-death scenes — `601_survivor_deaths`, `604_survivor_deaths`,
`604b_survivor_deaths_female`, `605_survivor_deaths`, `605b_survivor_deaths_female`,
`609_survivor_deaths`, `609b_survivor_deaths_female`, `610_survivor_deaths`,
`610b_survivor_deaths_female` — and `sd_male_a`.

**`data/cinematics/permanent.big` — 9 entries.** These are effects and animations, not
cutscenes; treat them as ambient unless you observe otherwise:

```
625_pawncam    door_fade_out    gnd_gpl_back_right    male_case_6_4_assemble
pike_sequence_1    pike_sequence_2    pike_sequence_3    pike_sequence_4    workbench1
```

Note `710_ending_a`, `713_ending_b1_katey_turns`, `714_ending_b2_katey_turns` and
`715_ending_c_quarantined` — the endings are distinct names, so a final split can key on
which ending was reached.

### Verified in play

Across two operator sessions, one a complete playthrough, these fired with the flag shown:

```
700_prologue_intro    701_chuck_arrives_in_town    702_in_the_garage
703_roadblock_discovered    703a_zombrex_bike_found    704_making_a_list
705_intro_to_queen_wasps    706_chuck_the_repairman
707_give_katey_zombrex_psycho_intro    708_psycho_is_dead
709_leave_the_garage    710_ending_a    711_alternative_exit
712_leaving_town    716_nightdead                                   -> EXCLUSIVE
625_pawncam    workbench1                                           -> ambient
```

**A skipped cutscene still ends cleanly.** Skipped ones ran about 2 s against 14–34 s
unskipped, and the flag returned to 0 both times — so you do not need to special-case
skips.

---

## Example readers

### Python (this one is tested)

`tools/speedrun_block_read.py` in the repository is the reference reader. It uses
`process_vm_readv`, which does **not** pause the game — important, because a debugger
attach would stall the very loading you are measuring.

```
python3 tools/speedrun_block_read.py --watch
frame     4005  state  4 FrontEnd  loading=0 cutscene=1(excl=1)  loads=1 cines=1  '700_prologue_intro'
```

The essentials, if you would rather write your own:

```python
import struct

BLOCK = 0x0000435A00000000

def snapshot(read):                      # read(addr, n) -> bytes
    for _ in range(64):
        s1 = struct.unpack("<Q", read(BLOCK + 0x10, 8))[0]
        if s1 & 1:
            continue
        raw = read(BLOCK, 256)
        if struct.unpack("<Q", read(BLOCK + 0x10, 8))[0] != s1:
            continue
        assert raw[:8] == b"CZSPDRN1"
        return {
            "state":     struct.unpack_from("<I", raw, 0x30)[0],
            "loading":   raw[0x38],
            "cutscene":  raw[0x39],
            "exclusive": raw[0x3A],
            "loads":     struct.unpack_from("<I", raw, 0x3C)[0],
            "cutscenes": struct.unpack_from("<I", raw, 0x40)[0],
            "name":      raw[0x68:0xA8].split(b"\0")[0].decode("ascii", "replace"),
        }
    return None
```

### LiveSplit ASL

**Untested — adapt it.** Watcher constructors vary between LiveSplit versions, so treat
this as the shape rather than as something to paste unchanged. The Python above is the
version that has actually been run.

```csharp
state("cz_runtime") {}

startup {
    vars.BLOCK = new IntPtr(0x0000435A00000000);
}

init {
    vars.loading   = new MemoryWatcher<byte>(IntPtr.Add(vars.BLOCK, 0x38));
    vars.exclusive = new MemoryWatcher<byte>(IntPtr.Add(vars.BLOCK, 0x3A));
    vars.cineCount = new MemoryWatcher<uint>(IntPtr.Add(vars.BLOCK, 0x40));
    vars.cineName  = new StringWatcher(IntPtr.Add(vars.BLOCK, 0x68), 64);
    vars.watchers  = new MemoryWatcherList {
        vars.loading, vars.exclusive, vars.cineCount, vars.cineName
    };
}

update {
    vars.watchers.UpdateAll(game);
}

isLoading {
    return vars.loading.Current != 0 || vars.exclusive.Current != 0;
}

split {
    // A NEW cutscene started, and it is the one we want.
    return vars.cineCount.Changed && vars.cineName.Current == "708_psycho_is_dead";
}
```

On Windows the process is `cz_runtime.exe`; `state("cz_runtime")` matches either.

### asr (Rust auto-splitting runtime)

```rust
const BLOCK: u64 = 0x0000_435A_0000_0000;

let magic = process.read::<[u8; 8]>(BLOCK.into()).ok()?;
if &magic != b"CZSPDRN1" { return None; }

let loading   = process.read::<u8>((BLOCK + 0x38).into()).ok()?;
let exclusive = process.read::<u8>((BLOCK + 0x3A).into()).ok()?;
let cine_name = process.read::<[u8; 64]>((BLOCK + 0x68).into()).ok()?;
```

---

## Troubleshooting

**The magic is not at the address.**
1. The build may predate the feature — it needs a build newer than `v1.1.1`. Check
   `cz_runtime.log` for a `[speedrun]` line.
2. Someone may have set `CZ_SPEEDRUN_BLOCK=0`, which disables it entirely.
3. The block may have been relocated. `cz_runtime.log` says so explicitly, and
   `cz_speedrun_block.txt` beside the game folder carries the real address:
   ```
   address=0x435a00000000
   version=1
   size=256
   magic=CZSPDRN1
   fixed=1
   ```
   You can also scan the process's small read-write mappings for `CZSPDRN1`;
   `tools/speedrun_block_read.py` does this automatically.

**Everything reads zero.** Check `monotonicNs` at `0x20` is advancing. If it is not, the
game is not presenting frames — the block only updates on a presented frame.

**`cutsceneName` is empty.** It is empty until the first cinematic of the session. That is
correct, not a failure.

**A cutscene name looks wrong.** Launch with `CZ_SPEEDRUN_TRACE=1`. Every cinematic start
then prints the name twice, from two independent sources, and says whether they agree:
```
[speedrun] cinematic #1 starts: PlayCinematic said '700_prologue_intro', manager+159C decodes '700_prologue_intro' — AGREE
```
The game also prints the first disagreement *without* that variable set, and falls back to
the source that cannot be wrong. Across 21 cutscene starts spanning a full playthrough
there have been **no disagreements**, so if you see one it is worth reporting.

**Getting the whole session's history at once.** Press **F9** in game at any point. It
writes a report whose `system.txt` contains every load and cutscene transition of the
session, with timestamps and frame numbers — useful for checking your splits against what
the game actually did. On Linux the reports land in `~/.config/XenonLive/captures/`; the
XenonLive launcher's Issues tab lists them and can send one.

```
speedrun transitions this session (oldest first):
  [    8.82s f   2464] LOAD BEGIN  state Loading
  [    9.73s f   2738] LOAD END    state FrontEnd
  [    9.77s f   2750] CUTSCENE BEGIN  700_prologue_intro  EXCLUSIVE  len 18 (inline)  decode AGREE
  [   39.12s f   7361] LOAD BEGIN  state FEToGame
  [   40.44s f   7765] LOAD END    state InGame
```

The timing of the press does not matter — the history is kept regardless.

---

## Reading anything else

`guestBase` at `0x28` is the escape hatch, and it is more useful than it looks.

It is where the emulated console's 4 GB address space begins in the process. **Console
addresses are properties of the original Xbox 360 executable**, so unlike anything in this
port's binary they never change when it is rebuilt — and they are the same addresses any
Xenia-based research would give you.

```
host address = guestBase + console_address
```

Values there are **big-endian** — byte-swap after reading. Two worked examples, the ones
this block itself uses:

* `0x82AD5EF8` → pointer to the game manager; `+0x24` is the current state's name hash.
* `0x82A57428` → `+0x2C` → `+0x78` → `+0x48` is the mission clock, four `u32`s:
  `+0x14` days, `+0x18` hours, `+0x1C` minutes, `+0x20` seconds.

That second one is likely interesting to you: it is the in-game clock, and Case Zero is
time-gated throughout.

If there is something you need that the block does not publish — PP, a mission id, inventory
— find it once against console addresses and it will keep working. **Or ask, and it can be
added to the block properly**, which is better than everyone maintaining their own pointer
path.

---

## Reporting problems, and asking for fields

Open an issue at
<https://github.com/wivi514/Dead_Rising_2_Case_Zero_Xenon_Recomp>. Useful things to include:

* the `[speedrun]` lines from `cz_runtime.log`,
* an F9 report's `system.txt` (it carries the whole session's transitions),
* what you expected the flags to do and what they did.

Requests for extra fields are welcome. There is deliberate room in the reserved tail, and
adding a field there breaks nobody.

---

## What is verified, and what is not

Being straight about this, since you are going to build on it.

**Verified by measurement:**

* both flags and the state index take every value they should, in both directions, across
  a full playthrough and several headless runs;
* the cutscene name is correct for every length the game produces — 21 starts, two
  independent sources agreeing every time;
* a skipped cutscene still produces a clean end;
* the three-load structure, on two independent sessions.

**Not yet verified:**

* **Windows.** The block is created the same way at the same address and nothing in the
  code is platform-specific, but nobody has run it on Windows yet. The startup log line
  states the address and whether it was the documented one, so this is easy to confirm —
  if you are on Windows and it works, saying so is genuinely useful.
* The ambient/exclusive classification of the `permanent.big` entries other than
  `workbench1` and `625_pawncam`.
* The survivor-death cutscenes have not been observed firing.

The engineering record — how each address was found, what was tried and failed, and one
retracted mistake — is in `docs/speedrun-block.md` for anyone who wants it.
