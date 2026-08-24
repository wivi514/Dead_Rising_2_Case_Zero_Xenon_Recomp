# Part 75 kickoff — TWO performance problems, and the operator notices the one that is not a hitch

> **THIS IS THE LIVE HAND-OFF**, superseding `part74-kickoff.md`.
>
> **Read `phase5-notes.md` §6do FIRST.** It is the operator's own marked-stutter session and
> it splits the subject in two. Everything else in this file depends on it.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6do** | **the operator marked 10 stutters — there are TWO problems** |
> | `phase5-notes.md` §6dn | the per-frame CPU/GPU profiler, and the texture decode |
> | `phase5-notes.md` §6dm | the frame decomposition; "outside the renderer" retracted |
> | `phase5-notes.md` §6dj/§6dk/§6dl | the RT-cost answer, and the sky flicker solved |
> | `docs/perf-state-parked.md` | the reference the older item designs came from — still not superseded |
>
> Lessons: gotchas **439-445**. `perf-plan-autonomous.md` is EXHAUSTED; there is no live plan.

---

## 0. THE DECISION THAT OPENS THE PART

The operator, playing with `F7` marking every felt stutter: *"stutter right after loading the
game and then a lot of stutter when moving the camera when big crowd of zombie on main
street."* **Those are two mechanisms and they need opposite work.**

| | **A — post-load hitch** | **B — the crowd** |
|---|---|---|
| what it is | texture bursts: 150-305 ms frames | frame time TRACKING DRAW COUNT |
| evidence | `texPh 229.3` of a 305.5 ms frame; 807 uploads | p95 within 10-14% of the median in every draw band — **no spikes** |
| the number | 469 ms decode + 244 ms submit per run | **12.5 ms at 2,500 draws -> 33.8 ms at 9,000+**, i.e. 80 -> 30 fps |
| is it a hitch? | **yes** | **no** — it is throughput, felt as stutter because it arrives as you turn |
| specified? | **yes, fully** (§1) | it is the ORIGINAL crowd problem; `perf-state-parked.md` is its reference |

**B is what they notice most, and no fix for A touches it.** That is the decision: do the
specified item, or re-open the throughput problem the campaign started from. **Ask them
before starting** — this file deliberately does not choose.

## 1. ITEM A — THE TEXTURE PATH, fully specified and priced

**82-90% of every post-load hitch frame.** Two halves, and the decode is the bigger one:

| half | cost/run | per texture | fix |
|---|---|---|---|
| **decode** — untile every mip, endian swap, image creation | **469.0 ms (66%)** | 209 us | **parallelise or cache. Take this first.** Pure CPU on the pump; the port already has a worker pool (part 53) |
| **staging + submit** — `memcpy` + `RunImmediate`, 94% of it `vkQueueWaitIdle` | 244.0 ms (34%) | 109 us | **batch** — constraints below |

**The batching constraints, already found:** `RunImmediate` exists precisely to be OUTSIDE
the frame's command buffer, because a pipeline barrier is illegal inside a dynamic-rendering
scope. Batching into the frame's command buffer means **breaking the render pass at each
upload** — priced at **~6.6 us** a cycle (§6di §1), so 2,243 uploads is **~15 ms against
271.6 ms, roughly 18x cheaper**. And `R->staging` is ONE buffer written at offset zero by
every upload, which is why the current code must drain before reusing it; the per-frame arena
(`ArenaAlloc`, 256 MB, host-visible) is the obvious replacement.

**Part 74 proved no cheaper WAIT PRIMITIVE exists** (three arms, gotcha 436). Batching or
nothing.

**Pre-registered kill, per half: below 40 ms off the worst frame of the route, do not ship
that half.** Amortised the whole path is 0.020 ms/frame — it is a HITCH item, so a small win
does not justify touching the upload path. Picture gate plus the operator's look before
shipping: it changes when pixels arrive.

## 2. ITEM B — THE CROWD, if they pick it

**It is not a hitch and must not be chased as one.** The frame is uniformly heavy: a marked
40-46 ms frame is `constants ~12 + textures ~10 + readback ~3.6 + recordState ~2.6`, nothing
spiking. Per-draw cost is **3.65-3.8 us** at high load (with the profiler's ~3% on top).

Everything already known about it: `perf-state-parked.md` (the reference), and part 74's
`§6do` for the shape. **Do not re-buy** the items part 73 closed — item A's whole-pass
recording, geometry in VRAM, the near-empty passes, the wide culling, the pipeline cache.

**What is genuinely unexplored** is the per-draw cost itself at 9,000+ draws, now that a
per-frame phase breakdown exists that ADDS UP (§6do §4). The four columns above are the whole
frame; each is a candidate and none has been attacked since part 55.

## 3. THE INSTRUMENTS PART 74 LEFT — all unconditional unless noted

```
(always on)         wall = CPUrec + fence + sleep + residual, which SUM, plus GPU, which
                    overlaps. GPU is from the frame's OWN command-buffer timestamps
F7                  the operator's STUTTER MARKER — stamps the frame into log and trace.
                    Names a NEIGHBOURHOOD (~200-500 ms reaction); read backwards ~1 s
CZ_VK_FRAME_TRACE=<file>   one line per presented frame. With CZ_VK_PROFILE also set it
                    carries ALL SIXTEEN phase columns, which then account for the frame to
                    within 2.7-4.5 ms. **Without CZ_VK_PROFILE those columns read 0**
CZ_PUMP_POISON_MS=N the residual column's positive control (40 ms in -> 40.0 ms reported)
CZ_VK_CONST_RACE=1  the constant-slot race detector (+ _POISON=1)
CZ_VK_SKY_ASYM=<f>  sky asymmetry — a THREE-RUN aggregate, not single-run (gotcha 444)
BIN_SRC=<path>      run any binary on the route — THE control for an intermittent defect
STILL=1 / PRESSMS   hold the camera / menu-walk speed
CZ_KEEP_SYNTH=<dir> keep the HLSL so alu_const_gate.py --hlsl-dir can run at all
```

**`CZ_VK_PROFILE` costs 2-4 ms a frame** — ~3% on a 150 ms stutter frame and a tenth of a
28 ms one. Fine for diagnosing a hitch; **never quote a normal frame's time from such a run.**

## 4. WHAT IS RULED OUT — do not start these

* **the GPU.** 7.93 ms mean, 0.37 ms fence wait, and all 10 operator marks had fence 0.0.
* **the guest side / outside the renderer.** Residual is 0.0 ms on every hitch frame (§6dm).
* **reverting the RT era for performance** — 0.5-0.7 ms (§6dj).
* everything on part 73's list: the pipeline-cache A/B, route (a) for the wide culling,
  whole-pass parallel recording, a range copy for the constants, geometry in VRAM, the 41
  near-empty passes, a cheaper wait primitive for uploads.

## 5. GATES AT PART 74's CLOSE — all clean

```
--smoke OK                              A5  exit 0, 4 permutation windows, 0 real
alu_const_gate --hlsl-dir  clean/449    shader_dim_census  clean
rt_world_xform  104 of 104              play cache NAME diff  empty
three play sessions: 0 'no translated shader', 0 slot mix-ups, 0 CONST MEMO STALE
```

**A5 is no longer owed** — carried from part 67 to part 74 and now clean.

## 6. THE ONE THING TO CARRY FORWARD

Part 74 reversed four conclusions, and every one had been drawn from an **absence**: "the
cost is outside the renderer" (three columns reading zero), "the lists are clean" (a caveat
printed beside exit 0), "the fix works" (one quiet run of an intermittent defect), and "the
phases explain the frame" (six columns that summed to 60% because the residual's own
children were missing). **The repair was the same shape every time — build the thing that
cannot return a false absence**: a decomposition where every millisecond lands somewhere, a
gate whose inputs survive by default, a positive control that must scream, a breakdown whose
arithmetic is checkable. When a conclusion rests on not having seen something, that is the
moment to spend the effort.
