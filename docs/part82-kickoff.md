# Part 82 kickoff — performance is PARKED; part 81 left two changes shipping and their price unmeasured

> **THIS IS THE LIVE HAND-OFF**, superseding `part81-kickoff.md`.
>
> **The subject is no longer performance.** The operator's instruction closing part 81
> (2026-08-27), after telling me to stop launching the game: *"Update your memory and all
> we'll switch to something else then performance."*
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6ee** | **part 81 end to end — the census, the two changes, the verifier, the guard census, and §7 on what was NOT measured** |
> | `docs/perf-plan-part81.md` | the plan, with its §5 execution record filed in place. **It is the reference that RESUMES performance** — it is not exhausted |
> | `docs/part81-kickoff.md` | part 81's board. Its item 1 (the guard's 86.2 MB) is now CLOSED; the rest stands |
>
> Lessons: gotchas **481-483**.

---

## 0. THE ONE THING A NEW SESSION MUST KNOW

**Two changes are live in the renderer, ON BY DEFAULT, whose CORRECTNESS is verified and
whose PERFORMANCE VALUE HAS NEVER BEEN MEASURED.**

| change | control arm | state |
|---|---|---|
| the **vertex bind batch** — one `vkCmdBindVertexBuffers` per contiguous run of CHANGED bindings | `CZ_VK_NO_BIND_BATCH=1` | mechanism measured at **0.464-0.474 calls/draw** against 1.742 before |
| the **device command table** — 13 record-path `vkCmd*` via `vkGetDeviceProcAddr` | `CZ_VK_NO_DEVICE_PFN=1` | engaged, proven by `nm -u` |

**If the operator reports anything wrong with the picture, bisect with those two variables
first** — it is two runs and they are the newest thing in the renderer.

The arithmetic says the batch is worth **~0.62 ms a frame** at their load. **That is a
calculation, not a measurement.** Do not quote it as a saving.

## 1. WHAT PART 81 ESTABLISHED

* **The bind-run census, and it is the reason the batch exists.** 118,515,047 draws: 3.292
  bindings offered/draw, 1.742 changed, **0.468 contiguous runs**, mean run 3.72 bindings,
  mode 6 at 28.0%, **zero untracked binds**. The pre-registered kill was 1.30. It also
  reproduces part 80's independently-derived 3.310/1.725 to within 1%. §6ee §1.
* **The guard's 86.2 MB is NOT on the pump, and that retires a board item.** Measured: **PUMP
  1.11 MB/frame over 67 hashes (2.9% of bytes), POOL 36.85 MB/frame (97.1%)**, the pump's
  half costing **0.077 ms/frame** at 15.08 GB/s. The "59 MB on the pump" was a subtraction
  between two counters read over different windows — gotcha 481. §6ee §6. It also prices what
  part 53's guard pool bought, for the first time.
* **The verifier is the transferable part.** 0 disagreements over 335 M `(binding, buffer,
  offset)` triples, poison at 100.0000%, and the poison had to be designed to fire on every
  check because 22% of runs are one binding long. Gotcha 482, §6ee §4.
* **Gotcha 480's closing question is answered in the negative.** Every other singleton-array
  call in this renderer was censused and none is an item — viewport and scissor are elided
  99.4%/99.3%, the barriers are 7 us a frame. §6ee §5. **Do not go looking for more of them.**

## 2. WHAT IS OWED IF PERFORMANCE RESUMES

1. **`tools/part81_bind_ab.sh`** — the three-arm campaign, written and never run. Three runs
   an arm, alternated, one binary, plus a profiler run an arm for `record` ns/draw (read the
   `vertex` sub-scope: it is the bind path directly). Pre-registered kill: **combined below
   0.30 ms, ship neither step.**
2. **`tools/part81_picture_gate.sh`** — written; two of its four arms died at 0 draws.
3. **Understand gotcha 483 first.** Runs began exiting ~3 s after start with a clean
   shutdown, no error and **no `[fps]` line at all**, while still reporting a number through
   the gate path. Disk was fine. Until that is understood, no run on this machine should be
   trusted without checking its log line count.
4. `perf-plan-part81.md` §3 — the always-on coarse split of the pump's walk, for the last
   unexplained hitch class. Never started.

## 3. WHAT IS NOT OWED

* **Nothing to the operator.** Part 79's sentence was collected in part 80; part 80 owed
  nothing; part 81 shipped no request.
* The gates part 81 ran, so part 82 knows what it inherits: `--smoke` OK on every build;
  `CZ_VK_VALIDATION=1` **6 `topology-08773` and nothing else**, the standing baseline; the
  bind verifier at 0 of 335 M with its poison at 100%; every reportable route run passed its
  own 8,000-draw gate. **Nothing outside `gpu/vk_renderer.cpp` was touched** — no config,
  kernel, PM4, shader or texture path — so part 74's A5 and `alu_const_gate --hlsl-dir`
  sweeps, part 75's cache gates, part 78's barrier gates and part 80's PM4 boundary oracles
  all stand.

## 4. A TOOLING DEFECT FOUND AND NOT FIXED

`tools/part80_crowdroute.sh`'s "is a run already going" guard is
`pgrep -x cz_runtime_crowd`, and **that name is 16 characters**, past the 15-character limit
`pgrep -x` matches against — so it prints a warning and matches nothing, every time. The
guard has never protected anything. It is one line (`pgrep cz_runtime` matches the truncated
name), it was left unfixed because the script was in use by a running campaign when it was
found, and **it is a candidate cause for gotcha 483** — two overlapping runs would produce
exactly that symptom.
