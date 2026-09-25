# The speedrun status block — a stable interface for load removers and autosplitters

**Requested by Pokisal (2026-09-24), who speedruns Case Zero and needs the same
load-removal signals the 360 and PC versions give them.** The problem they named is
real and is the reason this exists rather than a list of pointers: this port relinks
57,822 recompiled functions plus its runtime on most sessions, so any address or
signature found in the executable is stale within days.

So the runtime publishes the facts instead, at an address it promises not to move.

* **Address:** `0x0000435A00000000` (fixed, both Linux and Windows)
* **Size:** 256 bytes
* **Magic:** the first 8 bytes are the ASCII `CZSPDRN1`
* **Version:** `1`
* **On by default.** `CZ_SPEEDRUN_BLOCK=0` in the environment switches it off.

## The stability promise

1. The block stays at `0x0000435A00000000`. If that address is ever unavailable (nothing
   has been seen to take it, but a future driver or allocator could), the runtime maps
   the block elsewhere, says so on stderr and in `cz_runtime.log`, and writes the real
   address to **`cz_speedrun_block.txt`** beside the game root. The magic also makes a
   scan possible. A reader that checks the magic before trusting the address cannot be
   fooled by either case.
2. Field offsets and meanings in version 1 never change. New fields come out of the
   reserved tail. If a field ever has to change meaning, `version` changes instead.
3. All values are little-endian host-native (x86-64 on both shipped platforms). This is
   the host's byte order, **not** the guest's — the emulated PowerPC is big-endian, which
   matters only if you follow `guestBase` (below).

## Layout

| Offset | Type | Field | Meaning |
|---|---|---|---|
| `0x00` | `char[8]` | `magic` | `CZSPDRN1`, not NUL-terminated |
| `0x08` | `u32` | `version` | `1` |
| `0x0C` | `u32` | `size` | `256` |
| `0x10` | `u64` | `seq` | seqlock; **odd = a write is in progress** |
| `0x18` | `u64` | `frame` | presented frames since launch |
| `0x20` | `u64` | `monotonicNs` | host steady clock at the last publish — use it to tell "not loading" from "the game has stopped publishing" |
| `0x28` | `u64` | `guestBase` | host address of guest virtual address 0 (see below) |
| `0x30` | `u32` | `gameState` | index into the engine's own state table, `0xFFFFFFFF` if unresolved |
| `0x34` | `u32` | `gameStateId` | the engine's interned name hash for that state |
| `0x38` | `u8` | `isLoading` | **1 while the game is loading** |
| `0x39` | `u8` | `isCutscene` | 1 while any cinematic is running |
| `0x3A` | `u8` | `isCutsceneExclusive` | 1 while that cinematic is an *exclusive* one — a full-screen NIS. **This is the one you want.** |
| `0x3B` | `u8` | `isInGame` | 1 in `InGame` / `InGameTut1` / `GameShow` |
| `0x3C` | `u32` | `loadCount` | times `isLoading` went 0 → 1 |
| `0x40` | `u32` | `cutsceneCount` | times a cinematic started |
| `0x44` | `u32` | — | reserved |
| `0x48` | `u32` | `guestFlowObject` | guest VA of the game manager (diagnostics) |
| `0x4C` | `u32` | `guestCineManager` | guest VA of the cinematic manager (diagnostics) |
| `0x50` | `u32` | `guestCurrentCine` | guest VA of the running cinematic, `0` if none |
| `0x54` | `u32` | — | reserved |
| `0x58` | `char[16]` | `gameStateName` | `"Loading"`, `"InGame"`, … NUL-terminated |
| `0x68` | `char[64]` | `cutsceneName` | **the cinematic that started last**, e.g. `701_chuck_arrives_in_town`. NUL-terminated, and it *persists* after that cinematic ends |
| `0xA8` | `u8[88]` | — | reserved, zero |

## Reading it safely

`seq` is a seqlock. The publisher sets it odd, writes the body, then sets it even again.

```
loop:
    s1 = read_u64(base + 0x10)
    if s1 is odd: retry
    body = read(base, 256)
    s2 = read_u64(base + 0x10)
    if s2 != s1: retry
    -> body is a consistent snapshot
```

In practice a single read is almost always fine — the publish is one 256-byte store on
one thread, once per presented frame — but the retry costs nothing and removes a class
of one-in-a-million wrong answers, and `cutsceneName` is the field a torn read would
mangle most visibly.

`tools/speedrun_block_read.py` in this repo is the reference reader (Linux,
`process_vm_readv`, no ptrace stop so it does not perturb the game). It decodes by
offset rather than by struct, so it is a second implementation of this table and not a
restatement of it:

```
python3 tools/speedrun_block_read.py --watch
frame     4005  state  4 FrontEnd      loading=0 cutscene=1(excl=1)  loads=1 cines=1  '700_prologue_intro'
```

### LiveSplit ASL

```
state("cz_runtime") {}

startup { vars.BLOCK = new IntPtr(0x0000435A00000000); }

init {
    vars.loading   = new MemoryWatcher<byte>(vars.BLOCK + 0x38);
    vars.cutscene  = new MemoryWatcher<byte>(vars.BLOCK + 0x3A);
    vars.cineName  = new StringWatcher(vars.BLOCK + 0x68, 64);
    vars.watchers  = new MemoryWatcherList { vars.loading, vars.cutscene, vars.cineName };
}

update { vars.watchers.UpdateAll(game); }

isLoading { return vars.loading.Current != 0 || vars.cutscene.Current != 0; }

split { return vars.cineName.Changed && vars.cineName.Current == "708_psycho_is_dead"; }
```

On Windows the process is `cz_runtime.exe`; adjust the `state(...)` name accordingly.
`asr` (the Rust auto-splitting runtime) works the same way — `process.read::<u8>(0x435A00000038)`.

## The game states

`gameState` is an index into the engine's own top-level state table. The names and ids
are the game's, not ours, and the ids are string hashes of those names, so they are
identical in every build and on real hardware.

| Index | Name | `gameStateId` |
|---|---|---|
| 0 | `None` | — (the slot the engine never fills) |
| 1 | `Startup` | `0x26344A65` |
| 2 | `LegalScreen` | `0x267CDB6F` |
| 3 | `BCGIntro` | never requested on a boot |
| 4 | `FrontEnd` | `0xD6BB036E` |
| 5 | `FEToGame` | `0xB3876116` |
| 6 | `Loading` | `0x817315A6` |
| 7 | `InGame` | `0xA334C769` |
| 8 | `InGameTut1` | |
| 9 | `FEToGameShow` | |
| 10 | `GameShow` | |

`isLoading` is `gameState == FEToGame || gameState == Loading`. A boot is
`Startup → LegalScreen → Loading → FrontEnd`; starting a game is
`FEToGame → Loading → InGame`.

If your timing rules differ — some runs want the boot logos counted, some do not —
ignore `isLoading` and build your own predicate from `gameState`. That is why the raw
index and hash are both published.

## The cutscenes

Case Zero ships 29 cinematic scripts; `cutsceneName` is one of these names, without the
`.txt`. The story ones are in `700`–`716`:

```
700_prologue_intro                 709_leave_the_garage
701_chuck_arrives_in_town          710_ending_a
702_in_the_garage                  711_alternative_exit
703_roadblock_discovered           712_leaving_town
703a_zombrex_bike_found            713_ending_b1_katey_turns
704_making_a_list                  714_ending_b2_katey_turns
705_intro_to_queen_wasps           715_ending_c_quarantined
706_chuck_the_repairman            716_black
707_give_katey_zombrex_psycho_intro  716_nightdead
708_psycho_is_dead
```

plus the survivor-death scenes (`601_`, `604_`, `604b_…_female`, `605_`, `605b_`, `609_`,
`609b_`, `610_`, `610b_`) and `sd_male_a`.

**Prefer `isCutsceneExclusive` over `isCutscene`.** The engine distinguishes *exclusive*
cinematics — the ones that take the screen and the controller — from non-exclusive ones
that play in the world while you keep playing. Only the first kind is a thing a run
should have its timer paused for.

## `guestBase`, and reading anything else the game knows

`guestBase` is where the emulated console's 4 GB address space starts in this process.
Guest addresses are properties of the **original Xbox 360 executable**, so unlike
anything in our binary they never change when this port is rebuilt, and they are the
same addresses any Xenia-based research would give you.

So `host address = guestBase + guestVA`, and **values there are big-endian** — byte-swap
after reading. Two that this block itself uses, as worked examples:

* `0x82AD5EF8` → pointer to the game manager; `+0x24` is the current state's name id.
* `0x82A57428` → `+0x2C` → `+0x08` is the cinematic manager; `+0x1570` is the running
  cinematic (0 if none), `+0x159C` its name.

That is the escape hatch: if you need a split condition this block does not publish —
PP, a mission id, game time — you can find it once against guest addresses and it will
keep working, and we can add it here if you tell us what it is.

## How the signals were derived, and what is still owed

Every address above was read out of the title's own code with `tools/gdis.py`; the
derivations are in the header comment of `runtime/cpu/speedrun_block.cpp`, naming the
instruction that states each one. The two that matter:

* `sub_824B57C8` **is** the engine's "which top-level state am I in" accessor, so the
  state is read exactly where the game reads it.
* `sub_8248F728` reaches the cinematic manager the same way every frame and calls the
  manager's update **only when `+0x1570` is non-zero**, which is what makes that word the
  game's own "is a cutscene playing" predicate rather than a side effect of one.

Measured, not assumed:

* A headless DebugJump run with `CZ_STATE_TRACE=1` printed
  `Startup → LegalScreen → Loading → FrontEnd → FEToGame → Loading → InGame`, i.e. **a
  level load is a top-level `Loading` state** and not something private to the level
  system. That run is why `isLoading` is that simple.
* A second run walked out of the safehouse into Still Creek and produced **no further
  state change**, consistent with Case Zero streaming its one map rather than loading
  between areas.
* The `cutsceneName` decode has an oracle built in. The name is stored in an engine
  string class that is inline below 31 bytes and heap-allocated at or above it
  (`sub_827740F0`), so a decoder that handled one case would work for most of the game
  and produce rubbish for `707_give_katey_zombrex_psycho_intro`, which at 35 bytes is the
  only name in Case Zero that takes the heap path. The runtime therefore *also* captures
  the plain `const char*` that `PlayCinematic` was handed, compares the two at every
  cinematic start, **falls back to the passed name** if the decode looks wrong, and
  prints the first disagreement whether or not tracing is on. `CZ_SPEEDRUN_TRACE=1`
  prints every comparison; a headless prologue run reads
  `PlayCinematic said '700_prologue_intro', manager+159C decodes '700_prologue_intro' — AGREE`.

**Owed:** the inline branch is confirmed by that run; the heap branch — reachable only at
`707_give_katey_zombrex_psycho_intro` — has not been observed yet, and the fallback means
a wrong decode there would still publish the right name and say so in the log. Anyone who
plays to that mission with `CZ_SPEEDRUN_TRACE=1` closes it in one line.

Also owed: a Windows run. The block is mapped with `VirtualAlloc` at the same address and
nothing in it is platform-specific, but that is an argument, not a measurement.
