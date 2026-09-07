# Part 101 plan — the stutter and the flickering black square (czamd)

**Written at the end of part 100, to be executed fresh.** Context: the czamd boot hang
is SOLVED (`docs/part99-amd-hang.md` §6 — it was `NtReleaseSemaphore` ignoring `maximum`).
The game now reaches the main title and gameplay on czamd (AMD RX 6600 / Ryzen 5 5500).
Two issues the operator reported once in-game remain, and this plan is how to close them.

**Both are separate from v1.0.1.** v1.0.1 (semaphore fix + thread-budget floor +
AMD depth-format negotiation) is unblocked and can ship independently of this plan — these
two are polish, not boot blockers. Decide with the operator whether to ship v1.0.1 first
or fold these in.

## Reaching czamd (unchanged from part 100)
`ssh czamd` = `lisab@192.168.0.60`, Win10, RX 6600, Ryzen 5 5500 (6c/12t). PowerShell via
`-EncodedCommand` (base64 UTF-16LE); strip the CLIXML header with a regex, not grep. Build
loop: commit/push here → czwin `git pull` + `vc.bat cmake --build …\runtime\build` → scp
`cz_runtime.exe` to `C:\Users\lisab\Desktop\CaseZeroRecomp\`. Visible desktop launch:
`schtasks /run /tn cz_play` (runs `cz_play.bat`, logs to `run_visible.{out,err}.log`).
**A czamd boot to title is SLOW (~90-130s) — never call it hung under ~150s** (this
already misread the fix as unsolved once). The operator drives the game; headless is for
A/Bs.

---

## Issue A — the stutter

### What it is (evidence, part 100 log)
Classic part-98 session-ONE shader-compile stutter (gotcha 508). The `run_visible.err.log`
shows many `[vk] first-sight translation: … (32-52 ms)` and `[vk] async pipeline: N built
in background`, and `[vk] pipeline pre-warm: 23 of 23` / `32 keys` — tiny, because on a
machine seeing this content for the first time the pre-warm keys name VERTEX shaders,
which are first-sight-only and cannot be pre-built at boot.

### Step 0 — confirm it self-heals before treating it as a bug
Part 98's design is that session TWO self-heals (caches populated; the pre-warm CHAIN
builds parked keys the moment first-sight translation delivers their shader). **Have the
operator play the SAME area twice.** If session two is smooth, the "stutter" is expected
first-run behaviour and the work is only to soften the FIRST run. If session two still
stutters, the pre-warm chain is not doing its job on this content — that is the real bug.

### Diagnostics
- `CZ_VK_NO_PREWARM_CHAIN=1` vs default — does the chain actually reduce first-sight
  builds on session two? (It should; if not, the chain has a gap on this content.)
- Count `first-sight translation` and `async pipeline` lines per session; the number
  should collapse session two.
- Check whether first-sight translation itself (32-52 ms of DXC on the frame thread's
  critical path?) is the stutter, vs the pipeline create (already async). If the SPIR-V
  translation is on the frame thread, THAT is the hitch, not the pipeline build.

### Candidate fixes (in order of likely payoff)
1. **Move first-sight SPIR-V translation off the frame thread** if it isn't already — the
   32-52 ms `first-sight translation` is the biggest single number in the log and, unlike
   the pipeline build, may still be synchronous. Same skip-while-building visual contract
   as the async pipeline (draw is skipped until ready).
2. **Ship a fuller pre-warm key set** that includes the pixel-shader/pipeline keys reached
   in a full playthrough (the operator completed the game 2026-08-21 — dump the keys from
   that route and ship them), so session ONE pre-warms far more than 23.
3. **Widen the pre-warm chain** to cover vertex shaders as their microcode is first
   translated, if Step-0 shows it is missing them.

Gate: session-one outdoor frames >100 ms should stay near zero (part 98's metric). Keep
`CZ_VK_SYNC_PIPELINE=1` as the control arm.

---

## Issue B — the flickering black square, centre screen

### What is already known
- **The all-black cube `01330000` is a RED HERRING.** It logs "4x4 uploaded ENTIRELY
  BLACK — every reflection sampling it is dead" on BOTH czamd (AMD) and the NVIDIA dev
  box, which renders correctly. So it is a pre-existing guest-provided black cube, not the
  AMD artifact.
- The operator reports the square only on czamd (AMD), and it FLICKERS (frame-dependent).
- Part 100 changed the AMD depth format to `D32_SFLOAT_S8_UINT` (D24S8 isn't sampleable on
  AMD). This is the prime suspect BUT the square also appeared during the boot hang
  (before that change was on czamd), so do not assume it.

### Step 0 — SEE it (this is a shape question; gotcha "operator's eye answers shape")
Relaunch on czamd with `CZ_VK_FRAME_DUMP=<dir>` (every 64th frame as a PPM) and
`CZ_VK_SNAP_DUMP=<dir>` (every resolve snapshot of one frame → which PASS is black), have
the operator reproduce the square, and pull the frames. This says WHICH surface/pass and
whether it is a reflection, a post buffer, or a resolve. Do not theorise before this.

### The decisive bisection — is it the D32 depth change?
Run **`CZ_VK_DEPTH_FLOAT=1` on the NVIDIA dev box** (forces D32S8 there too). If the black
square appears on NVIDIA under D32, the depth-resolve path with D32 is the cause and it is
NOT AMD-hardware-specific — fixable and testable on the dev box. If NVIDIA+D32 is clean,
the square is AMD-hardware/driver-specific and must be chased on czamd.

### Hypotheses to test (after the frame dump narrows it)
1. **D32 depth resolve mis-read.** The guest reads depth resolves expecting D24 packing;
   with D32 the bytes differ. Check every path that interprets `R->depth` contents (not
   just the snap dump, which part 100 already made format-aware) — a resolve-to-guest-memory
   copy, or a shader sampling depth, that assumes 24-bit will produce garbage/black.
   `CZ_VK_SYNC_VALIDATION=1` and `CZ_VK_VALIDATION=1` first — a format/layout mismatch
   often shows as a validation hazard.
2. **Self-rendered reflection cube `06805000` black some frames.** The log shows its six
   faces "filled from its resolve snapshot"; if a face is sometimes unfilled the reflection
   flickers black. `CZ_VK_NO_CUBE_SNAPSHOT=1` is the control arm — if the flicker stops
   with it, the snapshot path is it.
3. **An AMD resolve/scissor artifact** unrelated to the depth change (the square is a fixed
   central rectangle). `CZ_VK_NO_DEFERRED_CLEAR=1`, then `CZ_VK_DEFER_FULL_RECT=1`, then
   `CZ_VK_NO_PAR_RECORD=1` is the standing picture-complaint bisection order (gotcha 506
   territory).

### Fix
Depends entirely on which hypothesis the frame dump + bisection confirms. If it is the D32
depth path (hypothesis 1), the fix is to make whatever reads depth format-aware for D32,
the same way part 100 fixed the snap dump — grep every reader of `R->depth.format` /
depth-resolve bytes. If it is AMD-only and not the depth change, treat it as a new
AMD-renderer defect and chase it on czamd with the frame/snap dumps.

---

## Suggested order
1. Frame dump + snap dump of the black square (Step 0 of B) AND the NVIDIA+D32 bisection —
   these run in parallel and one of them localises it fast.
2. In parallel, the operator plays the same area twice for Issue A's Step 0.
3. Fix whichever is confirmed first. The black square is the more visible defect; the
   stutter may turn out to be expected first-run behaviour.

See gotchas 506, 508; memories [[amd-gpu-depth-format-d24s8]],
[[part100-semaphore-limit-and-czamd-hang]], [[the-operator-eye-answers-shape-questions]],
[[part71-pipeline-cache-was-the-stutter]].

---

## Execution record (2026-09-06, same day — the local half is DONE; the operator half is owed)

**The full record is `phase5-notes.md` §6er; gotcha 513 is the transferable finding.**

**Issue A — root cause found, fixed, verified locally (95611b9, pushed, deployed to czamd).**
The plan's reading of the czamd log was wrong: "23 of [32]" cannot be the
vertex-shader gap (skipped keys stay in the denominator) — the per-user key file
held only 32 keys because czamd's hang-debugging sessions each saved a tiny file
from the Loading screen, and the per-user file SHADOWED the shipped 1,365-key
seed (which is present and dated 2026-08-29 beside czamd's exe). Fix: the loader
unions both files. Verified: fresh-start run (all caches parked, operator's
request) then warm re-run — `1079 per-user + shipped seed -> 1365 after union`,
first-sight 0, zero skipped draws, zero outdoor >100 ms frames.

Answers to the plan's diagnostics, from the fresh-start pair on the dev box:
- First-sight translation is ALREADY off the frame thread (`shaderjit::Worker`);
  candidate fix 1 does not exist as work. The 32-52 ms czamd lines are worker
  time; the felt cost is pop-in (draws skipped), not a stall.
- Session one on a truly fresh machine here: 4 frames >100 ms of 19,506 (1
  outdoor); the disc prebuild and the chain both work (1,079 background builds).
- Step 0 (does czamd session two self-heal) is still the operator's to run, now
  with the union binary swapped in there.

**Issue B — the decisive bisection is done and NVIDIA+D32 is CLEAN.** 286
outdoor frames at up to 8,024 draws under `CZ_VK_DEPTH_FLOAT=1`, tile-scanned
for interior black; all 15 flags were scene content. The code audit found no
D24-packing reader on the D32 path either (depth is copied between identical
formats and sampled normalized; the MSAA depth resolve negotiates SAMPLE_ZERO
with a loud refusal). The square is czamd-specific until czamd's own frames say
otherwise: its `cz_play.bat` now arms `CZ_VK_FRAME_DUMP` into `p101frames`, so
the operator's next sighting is recorded. Hypotheses 2 and 3 (cube snapshot
faces, the deferred-clear bisection order) remain live for that session.

**Owed:** operator on czamd — (1) same area twice for the stutter verdict, (2)
reproduce the square and note when; then pull `p101frames` and
`run_visible.err.log`. v1.0.1 remains unblocked and now also carries 95611b9.

## Addendum — the czamd MENU FLICKER, caught on F8 (2026-09-06 evening)

The operator's fresh-start session on czamd surfaced a better-shaped defect than
the black square: **the main-menu zombies flicker**, and one F8 burst caught the
transition. Frames 2705–2706 show four zombies; 2707 shows none — **with the
identical drawFingerprint and the identical 928 zombie-VS
(`vs_fa161b0fde7aa4d5`) draws in every frame, visible or not.** The NVIDIA
control (same F8 instrument at the title screen) holds 928/frame with zombies
continuously visible. So: czamd-specific, and the mechanism is *executed draws
producing no pixels* — not shader warm-up (that run had 0 first-sight
translations and 0 async pipeline builds), not draw-list churn.

- **Bisection arm 1 REFUTED for this symptom**: `CZ_VK_NO_DEFERRED_CLEAR=1`
  still flickers at the menu (operator, one relaunch). Next arms when resumed:
  `CZ_VK_DEFER_FULL_RECT=1`, `CZ_VK_NO_PAR_RECORD=1`, then the dynamic-data
  family (`CZ_VK_NO_PARALLEL_GUARD=1`, `CZ_VK_STREAM_GUARD_BYTES` raised).
- **In-game does NOT flicker** (operator, same session) — the defect is
  menu-era, or timing-dependent in a way gameplay load masks.
- **PAUSED at the operator's instruction** ("we'll stop there"); czamd's
  `cz_play.bat` is back to defaults with F8/F9 still armed (press-only cost).
  Burst evidence: `~/DR2CZ-troubleshooting/part101-czamd-burst1/`, NVIDIA
  control in `part101-devmenu-burst/`.
- **New owed item: the boot pre-warm is SYNCHRONOUS and cost czamd 138 s of
  black screen** (1,168 pipelines x 118 ms on a fresh AMD driver cache; the
  union fix widened the population it builds). It should build through the
  async pipeline machinery, or be budget-bounded with the rest parked for the
  chain. One-time per fresh driver cache, but a first-run player sees it as a
  hang — and the part-99 "stuck on Capcom logo" report should be re-read
  against this number.
- The czamd union line fired in the field: `1168 per-user + shipped seed ->
  1366 keys after union`.
