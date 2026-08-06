# D3D phase C, part 2 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically and is current through phase C's build-out
(2026-08-06, session 13). `docs/d3d-translation-plan.md` carries the phase C
strategy (REDIRECTED EMISSION) and the build-out findings list; this file carries
what a fresh context needs to FINISH phase C. Read the git log for session 13
(`e6946ad..cf12ae5`) — the commit messages are the detailed record.

## What works, measured

- **The architecture is delivered.** `CZ_D3D_DRAW=1`: every content API call runs
  with the device's command-buffer cursor (`dev+0x30/0x34/0x38` — THREE fields)
  redirected into a private 4 MB guest scratch; the title's own flush encodes; the
  private walker (`runtime/gpu/d3d_draw.cpp`) folds packets into a private register
  file + shader hashes; draws/resolves dispatch into the phase-5 renderer through
  `VkRenderer_D3DDraw/D3DSwap` (register file and bindings as parameters).
- **The pictures are correct**: the '© CAPCOM 2010' legal screen and the CAPCOM
  logo render pixel-correct from the D3D arm (headless `CZ_VK_FRAME_DUMP`).
- **Kernel gates are the best this port has had**: A1 = exact 82-prefix (phase B's
  `KeResetEvent`/spinlock window CLOSED once real ring traffic resumed — kickoff
  trap 4's prediction); A5 = exit 0, zero real windows. Control arm (`CZ_D3D=1`)
  unchanged. `--smoke` OK.
- **Zero crashes** across every run of the final interrupt-delivery design.

## The one blocker: the boot deadlocks MID-CINEMATICS

Every run parks at `cinematics.big` (file #56 of 64) with the engine thread in its
per-frame GPU sync (`sub_82845230` → `sub_82845160`), waiting
`completedFence >= target` with a steady lag of ~16 — fences emitted but their
EVENT_WRITEs never consumed, CP fully caught up, `dev+0x3460` (async flag) zero.
One earlier design (the crashy mirror-replay one, commit `3e14c1c`) got PAST this
era to `prologue_z01.big` (#63), so the era CAN complete — which localises the
problem to interrupt-delivery/worker-kick semantics, not the era's content.

What is known about the era's machinery:

- The movie player submits through the D3D WORKER's token queue (finding 40's
  machinery — `CZ_JOBQ_PROBE=1` shows the jobs; the buffer carries the frame
  dimensions). The worker is kicked by callbacks the streams ARM in the scratch
  mirror before INTERRUPT packets: `82841878` (frame tick, arg 0x100) and
  `827CC628` (job callbacks, args 2/3).
- The arms are DUAL-TRANSPORT: some ride the content stream (our walker sees
  them), some ride the real ring (the movie player's unredirected emitters —
  `sub_82838568`/`sub_82838D10`/`sub_82837E08`, still undecoded, all
  reserve-callers). The walker's delivery rule (own-stream arm, ring-file
  fallback, never call poison — see the `0x54` case) delivers 145+/run with zero
  faults, and the movie plays ~150 frames — then the fence lag pins at 16 and
  nothing moves.
- The fence plumbing itself is understood: `sub_828459D0` is the fence-block
  emitter (fence += 2, EVENT_WRITE_SHD pair at a caller-supplied cursor, plus a
  CPU fast path); the completed-fence word is `[[dev+0x2A90]]`; `dev+0x2A9C` is
  the emitted counter. `CZ_PM4_MEM_WATCH=<hex va>` watches any word from BOTH
  stream halves with timestamps.

Hypotheses the next session should discriminate FIRST (cheapest first):

1. **The stalled target's fences live in segments queued for the WORKER to
   submit, and the worker is waiting on something else.** `CZ_JOBQ_PROBE=1` +
   `CZ_WAIT_TRACE=1` + gdb (`pgrep -x cz_runtime`, `thread apply all bt 6`) at
   the stall names the worker's wait in one run. Finding 40's threads and the
   token interpreter (`sub_8284B568`) are instrumented already.
2. **Decode the three movie-player entries** (`82838568`, `82838D10`,
   `82837E08`) — they emit to the real ring during this era and their semantics
   are unknown; one may need servicing (or REDIRECTING — they may emit
   copy/draw-like content whose side effects we currently discard to the ring
   where they execute as no-ops for the picture but move protocol state).
3. **The double-delivery question**: our walker delivers content INTERRUPTs AND
   pm4 delivers real-ring INTERRUPTs; if a kick consumed by the wrong side
   leaves a queue entry unprocessed, the pipeline wedges. The jobq probe's
   head/tail fields say whether jobs pile up.

## The fastest path to the picture gate (independent of the blocker)

The intro movie is START-skippable on hardware. `CZ_FAKE_START_MS` does NOT fire
under `CZ_NO_WINDOW=1` (the pad path answers neutral headless — measured, zero
presses) — either fix that arm to work headless, or run WITH a window and a real
press (gotcha 103: schedule with the operator). Skipping the movie should land
the boot on the title screen, where the phase C picture gates run:

```
CZ_D3D_DRAW=1 CZ_VK_FRAME_DUMP=dir ...    # title-screen frames
python3 tools/frame_signature.py --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" <frame>
tools/frame_matched_diff.py PM4 arm vs D3D arm   # two runs per arm, CZ_VK_FRAME_STATS(+_SURFACE)
```

## Housekeeping the next session inherits

- The sync-wait probe (`[d3d] sync-wait`, anomaly-triggered), the reserve-service
  prints (capped 12), and the INTERRUPT disposition prints (capped 16) are
  investigation scaffolding in `d3d_hooks.cpp`/`d3d_draw.cpp` — keep until the
  blocker falls, then strip to counters.
- Boot wall-time on the draw arm is ~4-5x the PM4 arm (load-era pacing on
  lifecycle round-trips). A number to re-measure after the picture is right —
  phase 5 precedent, not a defect to fix first.
- The forced segment close in the sync-wait hook (`kH_InsertCallback_q` service)
  predates the interrupt fixes and has not been re-measured alone since; once the
  blocker falls, A/B whether it is still needed.
- Phase A label retractions to remember: `sub_82845230` is the per-frame GPU sync
  WAIT (not InsertCallback); `sub_8283E950/EAF8` are sampler filter/aniso setters
  (not SetShaderConstantF). Both recorded in the plan doc.

## Gates for phase C completion (unchanged from the phase C kickoff)

```
./runtime/build/cz_runtime --smoke                    # passing
A1/A5 kernel gates, empty save root, BOTH arms        # passing, best-ever on the draw arm
frame_signature.py vs E2                              # BLOCKED on reaching the title screen
frame_matched_diff.py PM4 arm vs D3D arm              # BLOCKED on the same
```
