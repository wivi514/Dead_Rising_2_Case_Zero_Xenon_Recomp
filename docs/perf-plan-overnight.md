# Overnight CPU/performance plan. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This is a **self-contained work plan for an unattended
overnight session** — no operator will be awake, so everything here is self-servable.
`docs/d3d-phase-c18-kickoff.md` is the general hand-off and is NOT superseded by this;
this file covers performance only.

The goal: **Case Zero runs at ~11.5 fps headless / ~15 fps windowed and that is what
limits how much of the game anyone can test.** It is a blocker on EVIDENCE, not a polish
item.

---

## 0. Ground rules. These are not boilerplate — every one was paid for.

* **`df -h /tmp` BEFORE and AFTER every run.** `/tmp` is a **32 GB tmpfs on this machine,
  i.e. RAM**. When it fills, commands die with `write error: Disk quota exceeded`, which
  presents as *the shell dying seconds after a run* and also breaks the operator's
  screenshot tool. Part 17 lost its best evidence to this. Frame dumps are ~3.7 MB each —
  **delete every dump directory as soon as it has been read.** Keep `/tmp` under ~15 GB.
* **Run timed arms SERIALLY.** Never two `cz_runtime` at once: a second copy is an
  intervention on the variable under test (gotcha 183), and frame rate is exactly that
  variable. Do not build while a timed run is in flight either.
* **Every claim needs a same-binary control arm and TWO runs per arm, alternated.** A
  single run of a rate is a fact about that minute (gotchas 50/51/86/159). If an
  improvement is under ~5%, say it is not distinguishable rather than claiming it.
* **One change per commit**, with the falsifiable prediction it makes written in the
  message. Commit ONLY after the gates below pass.
* **Do not "fix" anything Step 1 has not shown to be hot.** Part 17 picked the per-draw
  constant upload as the prime suspect on the strength of its arithmetic (8 KB x ~1,900
  draws = ~19 MB a frame). It is **0.5%** of the frame. Write the oracle before the
  theory (gotcha 80).

### The gates, after every change

```
./runtime/build/cz_runtime --smoke
cd runtime/build && CZ_SAVE_DIR=/tmp/perf/es CZ_RING_TRACE=1 timeout 130 ./cz_runtime > /tmp/perf/gate.log 2>&1
python3 tools/kernel_call_diff.py --xenia "Xenia logs/A1_boot_title_fullgame/cz_run1.log" --ours /tmp/perf/gate.log
python3 tools/kernel_call_diff.py --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" --ours /tmp/perf/gate.log --include-high-frequency
```

Expected: `--smoke` OK · A1 **exact 84-deep prefix** · A5 **exit 0, 0 real windows** ·
`truncated=0` · `no translated shader` = 0 · deepest file **#83 cinezombie.big**.
A1 position 71 permutes on both arms sometimes and is a long-known scheduling window
(gotcha 86) — not a regression.

### The measurement recipe (this is the only fps number that counts)

Gameplay, headless, no operator. Arrives at ~185 s, ~1,900 draws/frame:

```
cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_VK_PROFILE=5 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,NONE \
  CZ_VK_FRAME_STATS=/tmp/perf/<arm><n>.txt timeout 300 ./cz_runtime > /tmp/perf/<arm><n>.log 2>&1
grep vkprof /tmp/perf/<arm><n>.log | tail -8
```

Read the fps off the **last few `[vkprof]` lines** (steady gameplay), never off the whole
run — the run averages boot, four menus and a load with gameplay. `CZ_VK_FRAME_STATS`'s
last column is `msec`, so any era can be timed directly.

---

## 1. STEP ONE, AND NOTHING ELSE UNTIL IT IS DONE: attribute the 58%.

Measured on the current binary, gameplay, ~1,900 draws, **87 ms/frame**, **134% process
CPU** (i.e. one saturated thread — not a 16-thread workload):

| phase | share | ms |
|---|---|---|
| `submit` (GPU wait) | 32.6% | ~28 |
| **`outside`** — everything that is not the renderer | **58.5%** | **~51** |
| `draw` — the ENTIRE renderer | 8.6% | ~7.5 |
| ├ record (the vkCmd calls) | 5.0% | 4.4 |
| ├ streams (vertex/index copy+swap) | 2.2% | 1.9 |
| ├ textures (untile+upload) | 1.0% | 0.9 |
| ├ constants | 0.5% | 0.4 |
| └ other (decode, pipeline lookup) | 0.0% | ~0 |
| `readback` | 0.4% | 0.35 |

**`outside` is unattributed and it is the majority of the frame.** It contains at least:
the recompiled guest's own code (game logic, physics, the crowd), the PM4 command
processor walking the ring, the resolve/snapshot path (which is NOT inside any timed
scope), the kernel HLE import seam, and any inter-frame waiting. Those are four completely
different investigations and one number.

**Do not optimise anything until this is split.** In descending order of preference:

### 1a. `perf`, if it exists

`perf` was **not installed** as of part 17 (`which perf` empty; `perf_event_paranoid` is
2, which permits user-mode sampling without root). If the operator has installed it:

```
perf record -F 199 -g --call-graph=dwarf -p $(pgrep -x cz_runtime) -- sleep 60
perf report --stdio --sort=symbol | head -60
```

Attach **during the gameplay era** (after ~185 s), not during boot. The recompiled TUs
carry symbols (`-g` is on), so `sub_XXXXXXXX` names appear directly and can be fed to
`tools/gdis.py`. Beware `--call-graph=dwarf` on a 155 MB image — if it is too slow, use
`-g` (frame pointers) or plain `-F 199` with no call graph; a flat symbol profile is
already enough to answer the question.

### 1b. The poor-man's sampling profiler — no install, and this project already trusts it

`gdb -p` is how this port read the bindless heap out of a live game and recovered two
shaders (gotchas 143/199/217). The same trick samples a profile:

```
for i in $(seq 60); do
  gdb -p $(pgrep -x cz_runtime) -batch -ex "thread apply all bt 12" 2>/dev/null
  sleep 1
done > /tmp/perf/stacks.txt
```

Then aggregate the top frame per thread and rank. Join thread ids to guest threads with
the always-on `guest thread tid=... host tid=...` line. **Caveat that matters:** each
attach stops the process for ~0.2-1 s, so this both perturbs the thing it measures and
under-samples fast functions — read it as a ranking, never as percentages, and never
quote a frame rate from a run being sampled.

### 1c. In-runtime phase scopes — the fallback that cannot be wrong about ITS OWN buckets

`ProfScope`/`g_prof` in `runtime/gpu/vk_renderer.cpp` is three lines to extend. Add
scopes so `outside` splits into named parts, at minimum:

* the PM4 walk — `gpu/pm4.cpp`'s packet loop (this is the command processor and it runs
  every frame over a large stream);
* the resolve/snapshot path in `vk_renderer.cpp` (snapshot creation, `vkCmdCopyImage`,
  `RunImmediate`) — currently timed nowhere, and this title issues ~20 resolves a frame;
* the kernel import seam, or at least `RtlEnterCriticalSection`/`RtlLeaveCriticalSection`,
  which the operator log shows at **75.3 M and 94.2 M calls** in one session (~20,000
  enters per frame).

Whatever remains after those is the guest's own recompiled code, by subtraction — which
is the number that decides whether there is anything to win at all.

**Deliverable of Step 1: a ranked attribution of the ~51 ms.** Write it into
`docs/phase5-notes.md` before changing a line of code. If it turns out the guest's own
logic is the bulk, say so plainly — that is a much harder problem and the honest answer
is that the frame rate is near its floor without deeper work.

---

## 2. Candidate work items. Act on these ONLY where Step 1 supports them.

Ranked by expected value **given what is already known**, but Step 1 outranks this list.

### 2a. Overlap the GPU with the CPU — the biggest known-shape win (`submit`, 33%)

`SubmitAndWait()` submits the frame and blocks on a fence, so ~28 ms of GPU time is fully
serialised with ~59 ms of CPU. Double-buffering (record frame N while the GPU runs N-1)
could recover most of it — a ceiling of about **1.5x**, realistically ~1.25-1.35x.

**Read `SubmitAndWait`'s comment before touching it.** The synchronous design is
deliberate: the guest's own ring flow control paces this runtime (findings 38-39), and a
second host-side pipelining scheme makes "which frame is on screen" a question with two
answers.

Hazards, all real:
* **The frame arena is reused per frame** (`ArenaAlloc`, `streamCache`). Two frames in
  flight need **two arenas**, or frame N overwrites the vertex data frame N-1 is still
  reading. This is the part most likely to produce silent corruption rather than a crash.
* The readback for `Host_PresentPixels` needs a finished frame — present N-1, and accept
  one frame of latency (state that plainly; it is a real behavioural change).
* The resolve snapshots are sampled by later passes *within* a frame; check nothing
  crosses the new boundary.
* Put it behind `CZ_VK_PIPELINED=1` **off by default** first, so it is its own control
  arm, and only promote it after two clean gate runs and an A/B.

### 2b. Compiler flags on the recompiled image — cheap, low risk, uncertain size

Already `-O2 -g -DNDEBUG -msse4.1 -mavx`; **not** `-O3`, **no** `-march=native`, **no**
LTO. The image is 228 TUs / 57,822 functions and is most of `outside` if the guest is the
bottleneck, so even 10% there is ~5 ms.

Try in this order, gating and A/B-ing each separately: `-O3`; then `-march=native`
(the machine is a **Ryzen 7 5700**, Zen 3 — AVX2/BMI2 available, and `-march=native` is
safe because this binary is only ever run here); then ThinLTO. Expect single-digit
percentages and be prepared to report "no measurable difference" — that is a perfectly
good result and much better than a story. Watch build time and RAM: LTO on 155 MB of
objects can be enormous, and this machine's `/tmp` is RAM.

### 2c. The GPU side, if `submit` stays large after 2a

The EDRAM stand-in is **1280x1024** and window coordinates are scaled for **4x MSAA**
(part 9/14). That is a lot of fill. Worth ONE measurement, as an experiment arm and not a
fix: does GPU time scale with it? If the GPU is a real limit, the honest options are
narrow — this is a faithfulness/performance trade and **must not** be traded away
silently. Do not ship a resolution change without saying so.

### 2d. Critical-section call volume (~20,000 enters/frame)

Finding 41 fixed the *spin* (317% -> 121% CPU) but not the call count. Each call crosses
the kernel HLE seam. If Step 1 ranks it, look at the fast path — an uncontended
acquire should be one atomic and nothing else. `CZ_CS_STATS=1` says how many are
contended (it was 0.26% of 1.6 M, with 2 ever parking), so the cost is per-call overhead
rather than blocking.

### 2e. Per-draw record cost (`record`, 5.0%)

~9 `vkCmd*` calls per draw at ~1,900 draws. Redundant `vkCmdBindDescriptorSets` (the same
5 sets every draw) and `vkCmdSetViewport`/`Scissor`/`BlendConstants` could be skipped when
unchanged. Ceiling is ~4 ms, so only worth it once the bigger items are done — and note
`other` is **0.0%**, so the decode around it is already free.

---

## 3. What NOT to do

* **Do not optimise the renderer's decode, constants, textures or streams.** Together they
  are 3.7% of the frame. The numbers are in §1.
* **Do not touch the ring brake, the per-CPU ISR acknowledge, or the vblank gate.** They
  are this title's frame pacing (parts 5-6, 40 runs). If frame rate rises and the swap
  queue stops having `head == tail`, that is a regression however good the fps looks.
* **Do not use `CZ_FAKE_PRESS_SEQ` runs as gate runs** — synthetic input manufactures
  progress (gotcha 78). It is for the fps measurement only; the gates use a plain boot.
* **Do not quote a frame rate from a run that was being sampled by gdb or perf.**
* **Do not delete `~/DR2CZ-troubleshooting/ucode-dumps/`** — 33 of the 370 shaders exist
  only there.

---

## 4. If everything in §2 lands

87 ms -> maybe 60-65 ms, i.e. **~15-16 fps headless / ~19-20 windowed**. That is worth
having and it is not the 30 fps the title targets. If Step 1 shows the bulk of `outside`
is the guest's own recompiled logic, then say so in the notes and stop: the next real
lever would be a much larger piece of work, and an honest floor is more useful to the
operator than an optimistic plan.

**Leave the repo committed and the gates green.** The operator will be testing the game
in the morning, so a working slow binary beats a fast broken one — if an item cannot be
finished cleanly, revert it and write up what was learned.
