# Performance plan, part 116 — the guest's own 8.8 ms, and whether 120 fps exists

**Written 2026-09-12 in three minutes, for a 12-hour unattended session.** The operator
is at work; every item below is measurable on the autonomous crowd route with no
human. Read `docs/perf-plan-part111.md` §10 and `docs/part112-kickoff.md` §1.2 first:
they are where the ceiling was measured, and this plan starts from their numbers.

## §0. What is already known (do not re-derive)

- The frame is `wall ≈ max(pump, guest, GPU)`. At the operator's crowd (~9,300 draws,
  1080p, RTX 3070): pump CPU ~10.9 ms, **the title's own recompiled code 8.8 ms on two
  threads (76.7% + 61.7% of a core), GPU ~4 ms**. With the whole per-draw renderer
  deleted (`CZ_VK_NO_DODRAW=1`) the wall fell only to 8.80 ms — **the guest is the floor
  and nobody has ever profiled it** (part 110).
- The pump is bound by BYTES, not cycles (part 111, gotcha 551): parallelising it
  recovers nothing; fewer bytes and overlapped misses are its only class.
- 120 fps = **8.33 ms**. The guest floor alone is above it. So the only road to 120 is
  the guest's 8.8 ms, and it is open ground.
- Two verified sub-threshold items ship OFF: `CZ_VK_SCOPED_SHARED_ZERO=1` (−0.13 to
  −0.21 ms) and `CZ_VK_TEXMEMO=1` (−0.33 ms).

## §1. Rules (all from the ledger; each one cost a part)

- The rig is `tools/part80_crowdroute.sh` (the operator's route, 9,300-9,700 draws,
  ±2.9% floor). **Three runs an arm, alternated; medians and the pinned-share, never
  means** (gotchas 229, 237); matched draw bands (`tools/frame_perf_bins.py`,
  `tools/part110_pumpcpu.py`). Resolution pinned (`CZ_VK_RES=1920x1080`). GPU clock
  unpinned, sampled.
- **Archive the executable beside every `perf.data`** (gotcha 550). **Never rebuild
  while a campaign runs** (the route copies `cz_runtime` at each run's start).
- Every arm has a same-binary control and a counter that proves it engaged (151).
- Pre-registered kills, written before the run, in this file. No exceptions.
- No operator items. Nothing here changes a pixel; picture gates
  (`tools/part47_gates.sh`, pinned) run once at the end on whatever ships ON.

## §2. The order of work

### Item 0 — re-baseline (30 min)
Three runs, stock binary at HEAD: wall/median, pump cpu, GPU (`CZ_VK_GPU_PASSES=1` one
run only), and the **thread census** of the guest threads' CPU (the part-110 method).
Record in §4. Everything below is measured against this, not against part 110's.

### Item 1 — profile the guest (2 h) — THE ITEM THIS PLAN EXISTS FOR
`perf record -g --call-graph fp` (the ppc TUs keep frame pointers? if not, `-g` with
dwarf on one short window) over one crowd-route run, both guest threads. Symbols are
`sub_XXXXXXXX` in the 228 ppc TUs — resolvable. Produce, per thread, the top-40 self%
table and a per-CALLER table for the top 10. Then CLASSIFY each hot function by reading
it with `tools/gdis.py`: (a) CRT-shaped — memcpy/memset/strlen/strcmp/float math
(sqrt, sin/cos tables, matrix multiplies); (b) Havok; (c) CrowdEngine / animation;
(d) the title's D3D submission side (its own command building — which is on the guest,
not on our pump); (e) spin/wait (a thread waiting on OUR fence or ring reads as guest
CPU — part 107 found the Draw Thread's fence park; is there another spin?).
**Deliverable: `docs/perf-plan-part116.md` §4 with the table.** Kill: none — this is
measurement.

### Item 2 — native replacements for CRT-shaped guest functions (3 h)
The classic recompiler win. For every (a)-class function above 0.3% of a guest thread:
a `PPC_FUNC` hook that does the same work natively (a byte copy is endian-neutral;
`memset`, `memcpy`, `memmove`, `strlen`, `memcmp` are the safe set; float math only
where the result is bit-exact — sqrt yes, transcendental NO unless the title's own
table is used). **Gate before any A/B: a boot-time self-test that runs the recompiled
and the native form on 10,000 random inputs and compares bytes** (gotcha 30 — break it
on purpose once). Then the A/B. Kill: bundle < 0.3 ms median → revert, record.

### Item 3 — codegen on the recompiled TUs (3 h, builds are long: start it early on
a SEPARATE build dir, never on `runtime/build`)
Three arms, each its own binary, each measured 3×:
- **PGO**: `-fprofile-instr-generate` on the ppc TUs, one crowd-route run for the
  profile, `-fprofile-use`. No correctness risk. This is the single most likely
  large mover for the 8.8 ms.
- **ThinLTO** across the ppc TUs + kernel.
- `-O3` vs the current level on the ppc TUs only (check what it is first).
Kill per arm: < 0.3 ms median, or build > 60 min, or `--smoke`/A5 fails. If PGO wins,
the release recipe needs the profile checked in — write that up, do not do it.

### Item 4 — the two guest threads' critical path (1.5 h)
Which thread is the frame's critical path at the crowd, and what does the other wait
on? `CZ_WAIT_TRACE=1` + the thread census + one `perf sched` window. If the 61.7%
thread is mostly spinning on the 76.7% one (or on us), that is not "8.8 ms of work" and
the floor is lower than measured — say so with a number. Kill: none — measurement.

### Item 5 — the sub-threshold bundle, re-measured on the new baseline (1 h)
`CZ_VK_SCOPED_SHARED_ZERO=1 CZ_VK_TEXMEMO=1` together, 3×3. Report; ship decision stays
the operator's.

### Item 6 — the honest answer (30 min)
With §4 filled: is 120 fps CPU-side reachable on this machine, and what would it take?
One paragraph, numbers only, in `docs/part116-kickoff.md`. If the answer is no, say the
ceiling and the largest remaining term.

## §3. What NOT to do
- No renderer-side items: the pump is byte-bound and every ≥0.5 ms lead is closed.
- No threading of the pump (B1 null, B2 predicted dead, B3 refuted).
- No item without its kill rule written here first. No means. No single runs.

## §4. Results (filled during the session)

(empty — the profile table goes here first)
