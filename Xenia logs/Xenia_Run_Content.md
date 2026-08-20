# Dead Rising 2: Case Zero — Xenia capture run index

One entry per run. Requests: `xenia-capture-requests.md` (round 1).
All captures on the user's Windows PC, on the instrumented **xenia-canary fork**
(`canary_experimental@a635ac64f on Jul 22 2026`, Release), custom instruments OFF,
vanilla settings unless noted. STFS **package** launched directly
(`Assets/package/58410A8D/000D0000/3A98C69EE94FD53A3D592BBAC2236F2247A2957158`),
**not** the extracted `default.xex`. Title ID **58410A8D**.

---

## Round 1

### A1 — SHORT boot at max verbosity (+ D shader dump)  → `A1_boot_title_fullgame/`
**Delivered 2026-08-04.** Boot → every logo/intro movie played **in full (skipped
nothing)** → main menu → sat ~60 s → clean quit (`taskkill` no `/F` / window-close;
log ends with `Cheap-skate exit!`). `log_level=3`, `flush_log=false`, `apu="any"`,
`gpu="any"`, 1×1, `dump_shaders` on.

- **★ LICENSE / TRIAL — this bit us exactly as the request predicted.** First take
  booted the **TRIAL** (Xenia `license_mask=0` → `XamContentGetLicenseMask` returns
  unlicensed; main menu showed an **"unlock full game"** option). Fixed with
  **`license_mask = 1`** → unlock option gone → **full game**. The canonical A1 is the
  full-game run. **All future Case Zero captures must keep `license_mask = 1`.**
  `XamContentGetLicenseMask` is imported (82000410/ord 266) but not logged per-call at
  L3 — license state is proven behaviourally + by the config-dump header.
- **Trial path is measurably different** (don't diff against it): identical `.big`
  bank set both runs, except `chuckwalkietalkie.big` **trial 1164× vs full 2×** — that
  reload storm is the whole gap (file opens 1476 vs 314; log 45.8 MB vs 13.9 MB).
- **D — shader dump WORKS.** `dump_shaders` exists and filled: 120 distinct guest
  shaders (91 ps + 29 vs) as **raw Xenos microcode** (`.ucode.bin`) + disassembly
  (`.ucode`) + translated `.d3d12` DXBC. Xenia gives ground-truth microcode to compare
  the loose disc banks against — no runtime SHADER_DUMP hook needed for this. Frontend/
  menu set only (boot→title); capture again in A2/B2 for the gameplay set.
- **Movies:** no Bink; cinematics stream via an in-house Movie Player Object loading
  `.big` archives (`ratinglogos.big`, `700_prologue_intro.big`, `cinematics.big`). File-
  I/O lines preserved for the `.big`-format oracle.
- **Gotcha:** `log_file="C:/xenia_logs/…"` was ignored (this fork always writes
  `<cwd>\xenia.log`); log copied out after clean exit. `dump_shaders` forward-slash path
  worked, so it's log_file-specific, not path-escaping.

Files: `cz_run1.log` (13.9 MB, canonical A1) · `cz_shaders_A1.zip` (5.1 MB, 381 files)
· `cz_run1_TRIAL_license0.log.gz` (trial evidence only) · `A1_NOTES.txt`.

**A1 handed over alone, as requested.**

### A2 — into gameplay (Still Creek) (+ gameplay shader dump)  → `A2_gameplay_stillcreek/`
**Delivered 2026-08-04.** Boot → New Game → skipped in-game cutscenes → fought
zombies through Still Creek → continued to the cinematic right before the military
encampment (grab Katie's Zombrex). Full game, `log_level=3`, `flush_log=false`,
`apu="any"`, `dump_shaders` on. No crash.

- **★ FINDING — A2 does NOT contain the `.big` READ seek patterns; the request's
  premise is wrong.** `NtReadFile` is **`kHighFrequency`** → suppressed at plain L3.
  Runtime calls: `NtCreateFile` **23,965** vs `NtReadFile` **0** (the 2 hits are
  import-table decls). The game opens 433 `.big` archives but every read is invisible
  without `log_high_frequency_kernel_calls=true`. **To get the `.big` container
  format:** (a) A5 (boot + high-freq) already covers boot-era `.big` reads and the
  container format is uniform, so A5 likely suffices; or (b) authorize an **A2b** =
  this gameplay drive with `high_freq=true` + `flush_log=false` for gameplay-era seek
  order. Decide (a)/(b) before re-driving.
- **What A2 does give:** 24k streaming opens; 433 `.big` incl. gameplay-only
  (`701_chuck_arrives_in_town`, `702_in_the_garage`, `703_roadblock_discovered`,
  `anm_*` banks); full **XMA** context lifecycle (~5.33M lines, contexts 0–17+ — hence
  606 MB); **86 guest threads** (physics/audio/streaming bring-up); **1109 gameplay
  shaders** (vs A1's 381).
- **Integrity:** 606 MB, no crash/assert. Missing `Cheap-skate exit!` — Xenia was
  force-closed before my graceful taskkill, so the final KB-scale buffer batch didn't
  flush (`flush_log=false`); gameplay content complete. Future long runs: let me
  taskkill the live process so the exit flush lands.

Files: `cz_run2.log.gz` (606 MB → 57.6 MB) · `cz_shaders_A2.zip` (1109 files) · `A2_NOTES.txt`.

### A5 — high-frequency kernel calls (A1 drive)  → `A5_highfreq_boot/`
**Delivered 2026-08-04.** A1's drive (all movies in full → title → ~60 s) with
`log_high_frequency_kernel_calls=true`. Full game, `log_level=3`, **`flush_log=false`**.

- **Did NOT hang** (request warned it might). `flush_log=false` is the mitigation —
  the AW-D1b livelock was high-freq **+ flush=true**. Keep flush off for high-freq.
- **★ Closes the A2 `.big`-read gap — A5 IS the seek oracle.** High-freq makes
  `NtReadFile` visible (408 calls, 0 at plain L3). Reads are **handle-keyed**, not
  filename-keyed, so grepping `.big` on a read line finds nothing — but the log has
  the full chain: `NtCreateFile(name)` → `Added handle:H` (same thread) →
  `NtReadFile(H, …, Length, ByteOffsetPtr(value))` → `NtClose(H)`/`Removed handle:H
  for XFile`. Decoded example (handle F8000148, a datafile `.big`): `0x800@0`,
  `0x800@0`, `0x6C000@0`, `0xB000…` = header/index probes then payload — exactly
  "where header/index/payload live." Full reconstruction recipe + field map in
  `A5_NOTES.txt`. Scope = boot→title `.big` set; container format is uniform so this
  should suffice to RE the format (only do an A2b if gameplay streaming *order* is
  wanted).
- **Bonus surfaces (all high-freq, invisible before):** `VdSwap` 3131 (logs the
  `0x500`×`0x2D0`=1280×720 swap params → boot→title flip/vsync cadence, which AW
  could NOT get from any kernel-log level); `XamInputGetState` 12365; 178,629 sync
  primitives.
- **Integrity:** 231 MB, no crash. No `Cheap-skate exit!` (same as A2 — `flush_log=false`
  drops the final KB-scale buffer on close for large logs); content complete.

Files: `cz_run5.log.gz` (231 MB → 11.3 MB) · `A5_NOTES.txt`.

### B1 + B1b — GPU `.xtr` streams, boot→title (+ determinism control)  → `gpu_B1_boot/`, `gpu_B1b_boot_repeat/`
**Delivered 2026-08-04.** `trace_gpu_stream=true` from process start, A1 drive (movies
in full → title → graceful exit, no idle). Full game. Each run also emitted its
**same-run L3 correlation log** (fork emits both in one run). Frame 0 present.

- **B1** `58410A8D_stream.xtr` **1.61 GiB** · **B1b** (identical repeat) **1.12 GiB**.
  Both: header `01 00 00 00`+GUID, finalized (real-data tail), under the 2 GiB cliff,
  `license_mask=1`, reached title.
- **★ Determinism: size is NOT the metric.** B1 1.61 vs B1b 1.12 GiB (0.70) is idle/
  load length + per-run ASLR host fields, NOT non-determinism; a raw `cmp` is also
  meaningless (streams diverge immediately on host fields). Judge per-frame with the
  decoder over the fixed boot+movie prefix (AW E1b: content-deterministic, ±2-frame
  phase jitter). Both correlation logs lack `Cheap-skate exit!` (GPU-trace shutdown
  preempts the final log flush; boot→title content complete).
- **★ 2 GiB cliff is a hard constraint here.** Boot→title alone = 1.61 GiB. So B2's
  A2-style "New Game → ~5 min Still Creek" CANNOT fit — a brief save-load gameplay
  test already hit **1.92 GiB (96%)**. B2 must be a bounded, monitored slice.

Files per folder: `58410A8D_stream.xtr` · `cz_B1{,b}_correlation.log.gz` · `B1_B1b_NOTES.txt`.

### B2 — GPU `.xtr` stream, gameplay  → `gpu_B2_gameplay/`
**Delivered 2026-08-04.** Boot → New Game → **skipped cutscenes** (matches A2) →
outside in Still Creek → killed 12 zombies + grabbed weapon → graceful exit. Full
game, same-run L3 correlation log. Gameplay draw profile (order-of-magnitude more
draws than B1's title) + gameplay shader set + EDRAM under load.

- **★★ THE 2 GiB `.xtr` CLIFF IS FIXED (source patch + rebuild).** Root cause:
  `trace_writer.cc`'s compressed-write path used 32-bit `long`/`std::ftell`/`std::fseek`
  to seek back and patch each command's length header → past 2 GiB the offset wrapped
  and patched the wrong place. Fixed by swapping all 4 sites to Xenia's portable 64-bit
  `xe::filesystem::Tell`/`Seek` and rebuilding Release ([23/23], exit 0). **Proof: this
  B2 `.xtr` = 7.95 GiB (8,541,373,182 B), ~4× past the old cliff, valid header,
  finalized, no corruption.** The `.xtr` FORMAT never had the limit (per-command
  `uint32` lengths, not absolute offsets) → a 64-bit sequential decoder (Python) reads
  it unchanged. **All future GPU captures on this rebuilt fork are cliff-free.** Full
  detail + decoder note in `B2_NOTES.txt`.

Files: `58410A8D_stream.xtr` (7.95 GiB) · `cz_B2_correlation.log.gz` (258 MB→21 MB) · `B2_NOTES.txt`.
Bonus held locally: `cz_B2test/` — a 1.92 GiB save-load gameplay variant (undelivered
unless a New-Game-vs-save contrast is wanted).

### A3 — save / content round-trip  → `A3_save_content/`
**Delivered 2026-08-04.** Boot → New Game → first save point → save → menu → load
save back → exit. Full game, plain L3 (no GPU trace).

- **★ Save shape captured.** `XamContentCreateEx(…,"save",…,flags=0x1012)` →
  `XamContentCreateInternal("save")` → **`Registered symbolic link: save: =>
  \Device\Content\1\`** → `NtCreateFile(save:\DR2P000.DSF)` → **`NtWriteFile` of
  `0x4A000` = 303,104 B** (the whole save is ONE write) → `XamContentClose` →
  unregister. Load-back: `XamContentCreateEnumerator`/`Aggregate…` → re-mount →
  re-open `save:\DR2P000.DSF`. Root name `"save"`, mount `\Device\Content\1\`, file
  `DR2P000.DSF` (DR2 Prologue).
- **Physical save delivered** (`cz_A3_save_DR2P000.zip`): on-disk under the fork's
  profile GUID `E030000072C80CEF` → `58410A8D\00000001(=SAVEDGAME)\DR2P000.DSF\` (303,104 B,
  matches the write exactly) + `Headers\…\DR2P000.DSF.header` (328 B). Lets the analyst RE
  the `.DSF` format directly. (Profile-GUID caveat: reusing this save on the stock build
  needs copying into its `E0300000442B6B2E` content dir, same as AW.)
- Log lacks `Cheap-skate exit!` (window-closed before graceful taskkill); save+load
  content complete.

Files: `cz_run3.log.gz` (129 MB→9.1 MB) · `cz_A3_save_DR2P000.zip` · `A3_NOTES.txt`.

### C1 — function coverage, boot→title  → `C_coverage/`
**Delivered 2026-08-04.** `trace_function_data=true` (from-boot, NOT deferred — did
not crash the boot, like AW) over the A1 drive. Full game, same-run L3 correlation log.

- **12,278 functions executed** boot→title (guest range `0x80050030`–`0x829C3554`).
  The "forwards" oracle: executed addresses we have no function for = missing entry
  points for `config/CaseZero.toml` (AW recovered 215 this way).
- **Format:** 32 MiB preallocated, 48-byte `FunctionTraceData::Header` records packed
  from offset 0 (12,278 used, rest zero-fill → gzips to 114 KB). Fields: `start_address`
  +4, `end_address` +8, `function_call_count` +24. **`call_count` is BOOLEAN here (all
  =1)** = coverage flag, not a rate — correct for the SET-based forwards oracle (use
  `fcount` for rates). Treat boundaries as ranges; classify by size before believing
  divergences (4-byte fns aren't comparable). Full field map in `C1_NOTES.txt`.
- **C2 (gameplay) is the pair:** C2−C1 delta = gameplay-only function set.

### C2 — function coverage, gameplay  → `C_coverage/`
**Delivered 2026-08-04.** Same config as C1 over the A2 gameplay drive (New Game →
Still Creek → zombies → weapon). Full game, same-run L3 log (young_chuck assets).

- **C1 12,278 · C2 17,118 · C2−C1 = 4,840 NEW gameplay-only functions** (world/combat/
  zombie/weapon). C2 is a strict superset of C1 (0 boot-only funcs missing — C2 re-runs
  boot before gameplay). Matches AW's shape (there C1 +17,217 → C2 +5,370). The 17,118
  executed addresses are the forwards oracle for `config/CaseZero.toml`. Field map +
  delta detail in `C1_NOTES.txt`/`C2_NOTES.txt`.

Files (C1+C2): `cz_C{1,2}_trace.0.gz` (32 MiB→114/150 KB) · `cz_C{1,2}_correlation.log.gz` · `C{1,2}_NOTES.txt`.

### A4 — long title-screen idle  → `A4_title_idle/`
**Delivered 2026-08-04.** Boot → title → ~5 min idle (graceful timed shutdown) → quit.
Full game, plain L3. The title isn't static — steady state is ~69% `G>` GPU (per-frame
title render), ~17% `A>` XMA (title music, 320k lines), ~14% `d>` kernel; that repeating
per-frame cycle is the legible idle cadence (why it's 171 MB despite "idle"; gzip 42:1).
Files: `cz_run4.log.gz` (171 MB→4 MB) · `A4_NOTES.txt`.

### E — screenshots (visual target)  → `E_screenshots/`
**Delivered 2026-08-04.** Five full-screen PNGs (Win+PrtScn, fork Release, 1280×720
vanilla, game letterboxed): `E1` ESRB MATURE 17+ first logo · `E2` DR2 Case Zero title
(PRESS START) · `E3` title's animated 3D Still Creek background (why A4 idle is GPU-heavy)
· `E4` first gameplay frame (HUD, 0 KILLED) · `E5` zombie crowd at the gas station. No
frame index (OS grabs); correlate against B1/B2 streams if an exact frame is needed. Labels
in `E_NOTES.txt`.

---

## Round 1 — COMPLETE (all items delivered 2026-08-04)

A1 (boot flow + shader dump D) · A2 (gameplay) · A3 (save) · A4 (title idle) · A5 (high-freq
`.big`-read oracle) · B1/B1b (GPU title + determinism) · B2 (GPU gameplay) · C1/C2 (coverage,
+4,840 gameplay funcs) · E (screenshots). **All as the FULL game (`license_mask=1`).**

Cross-cutting wins this round:
- **Trial trap caught** (`license_mask=0` → trial; fixed to 1 for every run).
- **Section D answered**: `dump_shaders` yields raw Xenos microcode (`.ucode.bin`).
- **`.big` read oracle** is A5 (high-freq, handle-keyed), not A2 (`NtReadFile` is kHighFreq).
- **2 GiB `.xtr` cliff FIXED** in the fork (64-bit `trace_writer` seek + rebuild) — B2 = 7.95 GiB clean.

Held local (undelivered): `cz_B2test/` (1.92 GiB save-load GPU variant). Possible next: A2b
(gameplay high-freq `.big` order) only if the analyst wants gameplay-era seek order.
(A2b = gameplay high-freq only if the analyst wants gameplay-era `.big` seek order.)

---

## R2_world — the WORLD captures, round 2 (delivered 2026-08-10)

Seven **single-frame F4 GPU traces**, one per surface this port renders wrongly, each with
a screenshot, plus the session's whole `dump_shaders` output. Request:
`docs/xenia-capture-requests-round2.md`. Operator notes, including the deliberate method
change and its justification: `R2_world/R2_WORLD_CAPTURE_NOTES.md`.

Full game (`license_mask = 1`), 1280x720 native, canary fork `a635ac64f`.

| folder | what it is | trace |
|---|---|---|
| `w1_spawn` | the flat-white ground patch — the top defect | 64 MB |
| `w2_gasstation` | the same white ground, a second instance | 72 MB |
| `w3_pawnshop` | glass showing a ProtoMan cardboard; changes per session | 67 MB |
| `w4_bathroom` | Uncle Bill's window, blown white; a confirmed dummy-sampler | 67 MB |
| `w5_newsboxes_cactus` | white props | 59 MB |
| `w6_register_door` | cash register + white door side (screenshot is the WHOLE window, crop it) | 72 MB |
| `w7_slotmachine` | Barnyard Bonanza machines | 60 MB |
| `w_shaders.zip` | 357 distinct shaders — ALL of them already in our cache | 3 MB |

**Read them with `tools/xtr_draw_bindings.py`**, which names shaders by the same FNV-1a
hash the runtime and the cache use, so one draw is identifiable in both stacks. A
single-frame trace is self-contained (an `EdramSnapshot`, then a `MemoryRead` carrying the
actual sampled bytes of every texture, vertex and index buffer), so it replays standalone
— **ask for this shape rather than a continuous stream** for any question about a place
(gotcha 259).

**Screenshots were taken on a return trip AFTER the traces**, so camera and clock differ
slightly and at w5 a newsstand had fallen over. Use them for "what it should look like" and
place identification, never for a frame-locked pixel diff — the exact framebuffer for each
spot is inside its `.xtr`.

## R3_world/ (2026-08-12) — round-3 single-frame traces at part 35's four defect sites

Delivered the same night part 35 found the striped-material class (open-items 0s), one
paired `<name>.xtr` + `<name>.png` per site, plus `r3_shaders.zip` (331 distinct guest
shaders). Full method notes in `R3_world/R3_CAPTURE_NOTES.md`.

**NEW this round: the PNG is frame-locked** — the operator's fork now takes Xenia's own
guest-framebuffer screenshot at the F4 press, so each PNG is the trace's own frame
(round 2's return-trip screenshot caveat is gone; these ARE usable for pixel questions).

| spot | what it answers | verdict from the PNG alone |
|---|---|---|
| `tanker` | the close-range blotches | hardware's cab is clean CREAM — the cream skin our runtime showed once after a reload was the CORRECT one; the grey-green blotch state is the garbage |
| `survivor_dick` | Dick's striped distance material | hardware clean at the same range |
| `green_building` | the close-up mottling | hardware clean |
| `pawnshop` | the checkered window boards | hardware's boards are plain wood |

**Four for four: hardware is clean, the class is ours** — and each `.xtr` carries the
MemoryRead bytes of every texture hardware sampled, i.e. the ground truth for what the
CPU-composed sheets SHOULD hold. That is item 0s's writer-hunt oracle, in hand before
the hunt starts.

## R4_world/ (2026-08-12, round 4 — the Big Buck approach, eight viewpoints)
Eight single-frame F4 traces + frame-locked PNGs of the Big Buck hardware store area,
capture order 01 (oldest) -> 08 (newest), walking closer; `r4_shaders.zip` (261 distinct)
and `R4_CAPTURE_NOTES.md`. Requested as the item-00i "one deliberate look" and it
ANSWERS IT: hardware shows fully textured buildings at every distance — the flat-color
panel look at range is OUR defect. The PNGs also show hardware's foliage as proper
alpha cutouts (the shard-tree comparison), and each .xtr carries full register state
at the foliage draws — the oracle for the alpha-mode question (RB_COLORCONTROL).

## R6_gas_sign (delivered 2026-08-20)
One single-frame F4 trace at the far gas-sign viewpoint (`gas_station_sign.xtr`,
81 MB, trace #13691) + frame-locked 1280x720 PNG + full session shader dump
(291 distinct: 236 PS + 55 VS). Fulfills capture request R6; the analysis and the
fix it produced are `docs/phase5-notes.md` §6co (small packed textures).
