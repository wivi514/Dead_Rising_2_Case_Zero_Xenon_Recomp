# Part 60 kickoff — wake the shipped PC graphics menu: display mode, resolution, shadows

> **THIS IS THE LIVE HAND-OFF**, superseding `part59-kickoff.md`. Part 59 closed
> everything it opened: the owed gate sweep ran ALL GREEN (§1b there), Night Run 1
> answered the parked plan's owed items 1/2/4 overnight (`perf-nightrun1.md`), and the
> operator's R6 trace closed the gas-sign/distance class THE SAME DAY it was delivered —
> small packed textures, level 0 included, were read at the wrong tile offset
> (`phase5-notes.md` §6co, open item 00r, commit cf62229). Operator: *"Work really well
> now."* There is NO outstanding capture request.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction). And do
> NOT consult the Fable 2 port for renderer behavior — operator instruction, part 59:
> nothing works in 3D there; this port's own history is the reference.

## 0. THE ASSIGNMENT, in the operator's words

*"Get rid of the actual visual menu that lets us change gamma (anyway it doesn't even
work) and change it for a proper visual menu: add option for fullscreen, windowed
fullscreen, windowed. Options to change the resolution. If possible add more options
for shadow — the one right now is low, medium at higher resolution, high higher still.
Follow how games are and include everything necessary in a visual menu that we can do."*

This means the title's OWN pause/options menu (`Options -> Visuals`), NOT the F4 host
overlay.

## 1. THE DISCOVERY THAT SHAPES THE WHOLE PART (made during prep, verified on disc)

`data/frontend/fecmn.big` ships TWO options screens:

* **`options_visual.txt`** (7,919 B decompressed) — the LIVE 360 Visuals screen. Its
  ONLY setting is the Gamma meter (`ACT:Prev:Gamma` / `ACT:Next:Gamma`, a `cFEMeter`).
  This is the screen the operator wants gone, and gamma genuinely does nothing in our
  runtime.
* **`options_pc.txt`** (33,816 B decompressed) — **a COMPLETE shipped PC graphics
  menu**: `cFESpinGroup` items for **Resolution** (`ACT:Resolution:Prev/Next`),
  **DisplayMode** (`IDS_OPTIONS_PC_FULLSCREEN` / `IDS_OPTIONS_PC_WINDOWED`), **VSync**
  (On/Off), **Shadow** (`IDS_OPTIONS_PC_LOW/MEDIUM/HIGH`), plus Texture,
  Multisampling, Blur, Zombie and Filter groups — with art references, focus chains
  (`FOC:...`) and localization string ids all present.

So this is the DebugJump situation again (the F2 screen was also shipped data + dormant
code, resurrected in `debug_tunables.cpp`): the game carries the menu the operator is
asking for. The part's job is to WAKE IT and wire its verbs to the host.

Extraction recipe (the text-oracle fix to `big_decompress` landed in part 59):
```
python3 tools/big_list.py assets/game/data/frontend/fecmn.big --extract options_pc.txt --out DIR
tools/big_decompress DIR/options_pc.txt DIR/options_pc.dec
```

## 2. THE INVESTIGATION LADDER (do these IN ORDER; each answer changes the next step)

1. **Does the 360 image still carry the OptionsPC screen class and its ACT verbs?**
   `strings`/`gdis.py` the image for `options_pc`, `OptionsPC`, `Resolution`,
   `DisplayMode`, the `IDS_OPTIONS_PC_*` names, and the ACT dispatch site (find where
   `ACT:Prev:Gamma` is parsed — options_visual's verb must have a live handler, and
   that handler is the map to the whole verb namespace). The DebugJump precedent says
   dev/PC paths survive in this XEX; `retail-builds-may-still-contain-the-debug-build`
   is the memory.
2. **How does the frontend open a screen by name?** Already half-known: the DebugJump
   request goes "through frontend manager A33F4CC0" (`debug_tunables.cpp`). The pause
   menu's Visuals entry names its target screen somewhere — in `options.txt` (the hub
   screen, also extracted) or in code. Swapping THAT one reference to `OptionsPC`
   (or hooking the open) replaces the screen without touching the archive.
3. **String table**: do `IDS_OPTIONS_PC_*` ids resolve in the 360 string bank? Check
   the text .big (search `big_list --all --find` for the string/text bank; the ids are
   numeric — `Text="10718 IDS_OPTIONS_PC_RESOLUTION"` gives the id). If absent, the
   items render blank — fallback is serving a patched layout with literal `Text=`
   values through the VFS (see 4).
4. **Where to intercept the data**: if the layout itself needs edits (e.g. dropping
   the Mouse/Controller rows, or literal text), the VFS can serve a MODIFIED
   `fecmn.big` — or better, check whether the frontend tries loose-file paths before
   the archive (the engine builds paths like `anm_%s.big` at runtime; many Blue Castle
   loaders check loose first — `CZ_FILE_TRACE=1` on a menu boot answers this in one
   run, headless is fine for a file-trace question if the operator agrees, otherwise
   read the loader in gdis).
5. **Verb plumbing**: whichever ACT handlers exist, the SETTINGS must reach the host.
   The clean seam: a small `runtime/host/settings.{h,cpp}` holding the five values
   (display mode, resolution, vsync, shadow tier, +future), persisted to a file next
   to the save root, loaded in `main.cpp` BEFORE the window is created —
   **display mode and `SDL_WINDOW_VULKAN` are creation-time decisions
   (`host/window.cpp`, part 54's note)**. Guest-side ACT verbs land in a hook that
   updates settings + applies live where possible.

## 3. THE APPLIERS (host side), and what each needs checked

| setting | applier | state / risk |
|---|---|---|
| Fullscreen / borderless / windowed | `SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN / _FULLSCREEN_DESKTOP / 0)` on the WINDOW THREAD (SDL rule — everything window runs on the main thread, gotcha 99) | live-switchable; the swapchain must recreate on the resulting resize — part 54 built resize handling after the blurry-resize lesson (`replacing-a-librarys-present-inherits-hidden-jobs`); VERIFY it fires on fullscreen toggles too |
| Resolution | the internal render scale (`CZ_VK_RES` machinery — INTEGER multiples of 1280x720 only: 720p/1440p/4K/5K) | today it is boot-time. Live switch = recreating the render targets + resolve chain; if that turns out hairy, v1 is "applies after restart" with the menu saying so — games do this, the operator asked to "follow how games are" |
| VSync | present mode FIFO vs MAILBOX (part 54 shipped MAILBOX; FIFO is the "vsync on" behavior) | swapchain recreate with the other mode; cheap |
| Shadow Low/Med/High | scale the SHADOW-pass surfaces (the 4096x1024 atlas fmt-22 the R6 draws bind at s2/s7) by 1x/2x/4x — the same sample-rate scaling CZ_VK_RES applies to the scene, applied per-surface | INVESTIGATE FIRST: how does CZ_VK_RES treat the shadow surfaces today (scaled with everything, or native?); the scale must be transparent to fetches (normalized UVs — verify the shadow fetch path has no texel-space assumption). One change per experiment: land the menu with Low only wired if this drags |
| Gamma | REMOVED with the screen (the operator's call). If OptionsPC's Blur/Filter/etc. rows are kept, each either works or is hidden — **never a dead row that pretends** (honest-failure rule applied to UI) |

Texture/Multisampling/Zombie rows: hide them in v1 unless one turns out trivial —
a menu item that does nothing is the gamma slider again.

## 4. ORDER OF WORK

1. Investigation ladder §2 (mostly offline: strings, gdis, big_list; one file-trace
   run if needed).
2. `host/settings.{h,cpp}` + persistence + boot-time application (display mode,
   resolution via existing RES machinery, vsync). Testable without any menu via a
   hand-written settings file.
3. Screen swap: Visuals -> OptionsPC (hook or layout reference), rows for what works,
   hide the rest. Verb dispatch to the settings seam.
4. Shadow tiers (investigation §3 first; possibly its own part if surface scaling
   fights back).
5. Operator session: menu navigation, each setting applied and persisted across a
   restart. `--smoke` + the A5 diff after the VFS/hook changes (this part touches the
   file layer and the frontend, both on the boot path).

## 5. WHAT ALREADY EXISTS THAT THIS PART MUST NOT REWRITE

* Screen-by-name opening through the frontend manager + WAITJUMP barrier
  (`debug_tunables.cpp` — the DebugJump resurrection is the worked example of
  everything in §2.2).
* The title's menu-item native types (bool/int/action/selector) and how to drive them
  (`debug_tunables.cpp` ~line 1674).
* Window/present: `host/window.cpp` owns the window thread, the swapchain seam and
  resize handling (part 54); `CZ_VK_RES` scaling and the readback/swapchain arms.
* Screen HASHES for detecting which screen is up (`CZ_SCREEN_TRACE=1`, the map-close
  work) — useful for verifying the swap took.
* `big_list.py` + `big_decompress` (now with the text oracle) for every frontend asset.
* Settings that must survive: whatever file format `settings.cpp` picks, keep env vars
  WINNING over the file (an A/B arm must never be silently overridden by a menu).

## 6. STANDING ITEMS (unchanged)

* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1` ready.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Performance stays PARKED (`perf-state-parked.md`); Night Run 1's §6 updated it —
  remaining supervised items are item A's order gate and item C's gather design.
