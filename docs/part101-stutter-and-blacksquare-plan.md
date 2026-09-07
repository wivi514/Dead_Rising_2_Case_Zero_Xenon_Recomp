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
