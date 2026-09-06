# Async pipeline creation — the post-release stutter reports (part 98)

**Why this exists.** Within a day of v1.0.0 going public, multiple players reported
"it stutters a lot" (the operator's summary of the release thread; the thread itself
is unreadable from this network — every route returns a bot-wall, so the individual
reports are not quoted here). The repo already holds the measured mechanism that fits:
part 83's `GetPipeline` finding — on a machine with a cold driver cache,
`vkCreateGraphicsPipelines` costs **1–200 ms per pipeline**, is called lazily **on the
frame thread**, and fired 534 times in three minutes of play (one frame: 396 ms wall,
372 ms inside `GetPipeline`). The pre-warm shipped for it is only half a fix for a NEW
player: `prewarm.keys` carries all 1,364 keys, but only **757** can be created at boot,
because the other ~600 name **vertex shaders that do not exist yet on a fresh
install** — the disc holds no usable vertex microcode (release plan §1.4 retraction,
0 of 104), so every vertex shader arrives by first-sight translation *during play*,
and every pipeline that needs one compiles synchronously, mid-frame, at first
encounter. Our own machines never show this because their driver caches are warm
(the `GetPipeline` comment block says exactly this).

**The fingerprint that would confirm it from a user report**: the stutter is worst in
the first session, concentrated on first arrival in each area, and mostly gone by the
second session (per-user keys + driver cache). A report of stutter that *never* fades
is a different defect and should not be closed by this work.

## The two changes

**1. Async pipeline creation on miss** (`pipelinejit`, mirroring `shaderjit` — the
first-sight shader JIT, which already established the pattern AND the visual
contract: a draw whose pipeline is not ready yet is skipped under its own counter,
exactly as a draw whose shader is still translating already is today. So this adds
no new class of visual artifact; it moves ~1–200 ms stalls off the frame thread in
exchange for a few frames of a newly-appeared material being absent — the same trade
first-sight translation already made, once per shader, accepted since D.4.)

- `GetPipeline`'s build body becomes a **pure function** (`BuildPipelineObject`):
  no counters, no map access, no front-cache writes — those all stay on the pump
  thread (sync path and drain both end in one registration helper), so counter
  semantics stay single-threaded and honest.
- Jobs carry **copies** of the two `ShaderMeta` (they are plain data + handles), so
  the worker never reads the live shader tables.
- One worker thread (the operator's rule: leave the cores to the game), started
  lazily, never joined (this runtime exits with `_Exit`; same shape as `shaderjit`).
- `vkCreateGraphicsPipelines` against the same `VkPipelineCache` from a worker is
  legal: a default-created pipeline cache is internally synchronized.
- Drain on the pump thread at the `GetPipeline` miss site (the miss recurs every
  draw while pending, so the drain is reached) plus once per frame at `DoSwapImpl`
  (so a pipeline whose draws stopped recurring still lands and its key still saves).
- A pending key touches **neither** the pipelines map **nor** the front cache —
  emplacing a null would read as "refused forever".
- The async gate stays closed until boot pre-warm has run (`bootDone`), so the
  757-key load-time warm stays synchronous where a player expects to wait.
- **`CZ_VK_SYNC_PIPELINE=1` is the same-binary control arm**: the exact part-83
  behaviour, both features off.

**2. Chain the pre-warm to first-sight translation.** The keys skipped at boot for a
missing shader are kept (`prewarmWaiting`); when a first-sight translation lands in
`shaderjit::Drain`, every waiting key naming that hash (and whose other shader is
present) is enqueued to the worker — so the pipeline is usually built *before* the
first draw that needs it ever asks, and the on-miss skip becomes the rare backstop
rather than the mechanism. On-miss jobs jump the queue (a draw is actively being
skipped for them; chain jobs are speculative). **`CZ_VK_NO_PREWARM_CHAIN=1`**
disables only the chain, for bisection; `CZ_VK_SYNC_PIPELINE=1` disables both.

## Pre-registered predictions

Fresh-player simulation = fresh `CZ_ROOT` (shader cache rebuilt from disc: pixel half
only, so first-sight vs fires) + fresh `XDG_CACHE_HOME` (no VkPipelineCache blob, no
per-user keys — shipped `prewarm.keys` only) + `MESA_SHADER_CACHE_DISABLE=true`
(every compile is a real cold compile, every run). Route: the DebugJump crowd route.

1. **Arm S** (`CZ_VK_SYNC_PIPELINE=1`, the part-83 behaviour): tens-to-hundreds of
   frame-thread pipeline creates after boot; the slow-frame table shows frames whose
   time is dominated by pipeline ns (the part-83 shape).
2. **Arm A** (async, `CZ_VK_NO_PREWARM_CHAIN=1`): frame-thread creates after boot
   = 0; the skip counter is non-zero; the pipeline-dominated slow frames are gone.
   Draws skipped per new pipeline is small (single-digit frames at play frame rates).
3. **Arm C** (async + chain): skip counter well below arm A's, because chained keys
   build before their first draw; `prewarmWaiting` drains toward zero as vertex
   shaders arrive.
4. `CZ_VK_VALIDATION=1` stays at zero errors on arm C; `grep -c "no translated
   shader"` stays 0; total `pipeline: created` at end-of-run agrees across arms at
   matched routes (asynchrony must not lose or duplicate pipelines).

**Kill criteria**: any validation error attributable to the change; a skipped-draw
episode that does not converge (a key pending forever); arm C measurably worse than
arm S on any of the above. Prediction 3 failing kills only the chain commit, not the
on-miss commit.

## What this does not touch

The texture-hitch stutter class (closed in parts 74–77), the RT pipelines and the
resolve/present pipelines (their own creation sites, all boot-time), and the boot
pre-warm's synchronous placement. If player reports survive this fix, the next
suspect needs new evidence, not this mechanism again.
