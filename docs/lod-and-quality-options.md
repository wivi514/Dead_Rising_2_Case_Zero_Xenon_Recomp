# LOD and quality options in the image (2026-10-08)

**Operator report:** "it happens to all zombies: they are low poly close and you need to be
really close to see their high poly version" (the sheriff, ~4-5 m from Chuck, flat far
model; captures `~/DR2CZ-troubleshooting/play/visual_1008_0116/`). Shipped the same night as
the **ZOMBIE DETAIL** setting, operator-verified by eye ("looks way better"). The operator
then asked what other LOD controls exist; that census is §3.

## 1. How the crowd picks a model (title code, read in full)

`sub_825B8680` is the crowd renderer's per-frame LOD assignment. `sub_825A6B38` scores
every crowd zombie (up to 500):

    key = cot(fov/2) / (view depth * camera+0x424) * (0.75 for one flag, else 1)

where fov = camera `+0x44` + `+0x34` in degrees, clamped to 1..150. The list is sorted by
key, largest first (`sub_825B70B0`). Then, from LOD1 (or LOD2 in one mode, or the debug
global `0x829DFA74` when it is not -1) through LOD3, each LOD takes the next `N_i` zombies
whose key is `>= T_i`:

| LOD | count `N` (0x8208A050) | key `T` (0x829DF708) | model |
|---|---|---|---|
| 0 | 1 | 9999 | never reached in play |
| 1 | 30 | 0.13 | the close kit, `<type>_2.big` |
| 2 | 70 | 0.01 | `_3.big` |
| 3 | 400 | 0.001 | `_4.big` |

**Both are scaled by one quality value Q**: `N_i * Q` (each clamped to 1..500, and the
total to 500 by the title itself) and `T_i / Q`. Q = 2 means 60 close-model zombies, each
holding the close model to twice the distance.

The per-LOD counts feed the engine's own profile counters `LOD0..LOD3 Zombie Count`
(strings at 0x8208ACE8..), which is how the function was found.

## 2. Q is Dead Rising 2 PC's graphics-options table, compiled into the 360 build

`sub_8279A488(settings, group)` returns the float of the selected entry in a 23-entry option
table at `0x82AD0480` (0x14 bytes each: name hash, group, float, int, name pointer). The
table and its selections are filled at boot (`sub_829AF5D0` builds the defaults at
`0x82AD06E0`; `sub_8279A3B8` copies them into the settings object `0x82A52D88 + 0x1C`).
The names are the PC port's graphics options. Live selection at gameplay (shipped default
in brackets where it differs):

| group | entries | selected live | read by |
|---|---|---|---|
| 0 | `MSAA_NONE` .. `MSAA_8X` | `MSAA_4X` [NONE] | one site, `sub_82491048`; our renderer owns MSAA |
| 1 | `FULLSCREEN_ENABLED/DISABLED` | DISABLED | none found |
| 2 | `ZOMBIE_COUNTS_HALF/ONE/TWOFOLD` = 0.5/1.0/2.0 | ONE | **only `0x825B8884`**, the crowd LOD above |
| 3 | `COMBINED_BLUR_ENABLED/DISABLED` | DISABLED [ENABLED] | five sites (render loop, `cHWPostFX::HWDoPostProcess`); `sub_82474790` forces DISABLED (`Select(14)` via `sub_8279A508`) and restores it later |
| 4 | `TF_TRILINEAR`, `TF_ANISOX2..16` | `TF_ANISOX4` | no reader found; our sampler honours each fetch constant's own aniso field |
| 5 | `SHADOW_QUALITY_LOW/MEDIUM/HIGH` = 0/1/2 | MEDIUM | one site, `sub_825A11F0`: the value goes into a lighting shader constant block (`+0xFC`) |

Accessors: `sub_8279A488` (value pointer), `sub_8279A550` (selected index),
`sub_8279A508` (select an entry). The read sites were found by a census of every `bl` to
those three and the `li r4,N` that precedes it.

**Verified live before anything was written:** in a running game, poking the group-2 index
at `0x82A52D88 + 0x24` from 11 to 12 (Q 1.0 -> 2.0) gave the operator's "looks way better".
Nothing in the game rewrote the index afterwards.

### The shipped fix: ZOMBIE DETAIL

`runtime/cpu/crowd_lod.cpp` hooks `sub_8279A488`. For group 2 it returns a pointer to a guest
float the runtime owns, which holds the setting. Every other group passes through.

- **Range:** 0.5 to 5.0 in steps of 0.5, stored as `crowd_lod_x10` in `cz_settings.txt`.
- **Default:** 2.0, or **1.0 on the Steam Deck build** (`CZ_DECK_DEFAULTS`, the operator's
  call: twice the close-model zombies is twice their draws).
- **Panel:** the tenth row of the in-game settings panel, ZOMBIE DETAIL. It applies live,
  because the crowd renderer looks Q up every frame.
- **Env vars:** `CZ_CROWD_LOD=<value>` overrides the setting, and `CZ_CROWD_LOD=0` is the
  control (the title's own 1.0). The first lookup prints `[crowdlod] crowd LOD scale ...`.

**Not this repo's defect.** Every input is the title's: the counts, the thresholds, Q's
selection and the camera fov. Our fov hook is inert at 16:9 with `fov=0`, as it was in the
session that reported this. Xenia has not been asked, and the operator did not need it
answered. The setting is an enhancement on top of the 360's medium choice.

**Owed:** a frame-time number at a big crowd for 2.0 and 5.0. At 5.0 the count clamps
(LOD3's 2,000 becomes 500, and the 500 total binds) and LOD1 takes 150.

## 3. Other LOD and distance controls — census, not yet acted on

| Name in the image | Where bound | What it is | State |
|---|---|---|---|
| `SHADOW_QUALITY_*` (group 5) | `sub_825A11F0` | 0/1/2 into a lighting constant, MEDIUM selected | **next test**: poke `0x82A52D88 + 0x30` to 22 (HIGH) live and compare by eye |
| `Start_/End_CascadeDist`, `CascSliceOverlap`, `CascZeroNearPlane`, `NearPlaneCull`, `FadePercent` | sun light object, `sub_823C1A00..` (field map below) | the shadow cascades | parked since part 93, `shadow-distance-investigation.md` |
| `mVisibleDistance` | `sub_82495BB0`, with `mPropState`/`mOpCode` | a per-prop visible distance | unread; a candidate for props appearing late |
| `LoadDistance`, `LoadDistanceType` | `sub_8239C1F8`, with `SecondsBeforeDeadPropVanishes` | a per-prop spawn/stream distance | unread |
| `LOD4Optimization`, `ZombieSprite`, `BulkInstanced` | `sub_829A5DE4` | probably sprite stand-ins for the farthest crowd | unread |
| `MaxZombieDist` | `sub_82343BB0` | an AI targeting query (with `MaxSteering`, `IncludeCrawling`), NOT rendering | no action |
| `ZombieStartFadeOut` | `sub_8299CB34` | a network event name | no action |
| world / buildings | `cZone::UpdatePriorities`, `COMMON_TEXTURE_LOD.tex` | per-zone streaming, no distance scalar | part 43 exonerated our side (open-items 00i) |

The sun light's property map, read statically from the binder calls at `0x823C1A00..`
(field offset -> name, assuming one `lis r11` base for the whole block): `+0x94
Start_Attenuation, +0xA0 Start_DepthBias, +0xB0 Start_FadePercent, +0xB4 Start_CascadeDist,
+0xC4 Start_CascSliceOverlap, +0xC8 Start_CascZeroNearPlane, +0xCC Start_NearPlaneCull`, and
the `End_` set from `+0xD0` (`End_RGB`) to `+0x134` (`End_NearPlaneCull`), with
`End_CascadeDist` at `+0x11C`. **This disagrees with `shadow-distance-investigation.md`**,
which records `End_CascadeDist -> +0xF8` from the live `CZ_PROP_TRACE_ALL` trace and notes
that the interpolator pairs `+0xB4` with `+0x11C`. The static map would explain that pairing,
but the trace is a measurement and this is a reading. Re-run the trace before acting on
either.

## 4. For Case West

The same engine ships the same option table and almost certainly the same crowd LOD code.
To re-derive it there:

1. Find `ZOMBIE_COUNTS_ONE` in the image, then the `bl` to the value accessor with
   `li r4,2` in front of it.
2. Read the counts and thresholds tables next to it. Case West has bigger crowds, so its
   counts may differ.
3. Hook the accessor the same way.

Also check whether the rest of DR2 PC's graphics menu (shadow quality, blur) does anything
on the 360 build before offering any of it as a setting.
