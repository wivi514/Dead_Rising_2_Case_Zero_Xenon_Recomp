# Part 107 kickoff — the hand-off from part 106 (2026-09-08)

**THE LIVE HAND-OFF.** It supersedes `part106-kickoff.md` on "where the port is"; that
file's §1 item 0 (the Windows compile) was done in part 105 and its §1b item 0 (rebuild
v1.0.2 or call it v1.0.3) is now joined by a much larger reason to rebuild.

## §0 What part 106 established — read this before anything else

**The operator re-opened performance with a target:** *"this game should be atleast
playable 60fps locked at 1080p on gtx 1060."* `docs/perf-plan-part106.md` is the plan
and the record; `phase5-notes.md` §6ew the narrative; gotchas 530-532 and the in-place
retraction of 363.

1. **Half of the GPU frame was the GPU fetching its geometry over PCIe.** Both the
   per-frame arena and the 1 GB cross-frame stream store were in system RAM since they
   were built. Decomposed with three new arms and a pipeline-statistics census (all in
   `docs/instruments.md`): at the crowd at 1080p 2x on the RTX 3070, 8.84 ms of GPU =
   ~4.5 fetch + 1.1 fragments + ~1.1 per-draw front end + ~0.6 vertex shading + resolves,
   post and the headless readback.
2. **The fix is SHIPPED, ON BY DEFAULT: a device-local mirror of the store**
   (commit "the cross-frame stream store gets a DEVICE-LOCAL MIRROR"). The CPU writes the
   host store as before; last frame's written ranges are copied host → VRAM at the top of
   each frame's command buffer; a persist hit binds the mirror once its copy is queued
   ahead of it. **Crowd GPU 8.84 → 4.00-4.11, light load 5.00 → 3.25; wall −22 to −24%
   in the GPU-bound bands and a small gain at the CPU-bound crowd.** Validation clean,
   sync validation 0 hazards, three runs against seven baselines and control runs.
   `CZ_VK_NO_STORE_MIRROR=1` is the control arm and the FIRST bisection for any picture
   complaint from here on (ahead of `CZ_VK_MSAA=0`).
3. **Part 73's "geometry in VRAM is wrong for a recompiler" (gotcha 363) is retracted in
   part.** Its arena half stands (the per-draw constants stay in RAM); its store half
   was a wall-time verdict in a CPU-bound regime whose GPU column was never read.
4. **On the project's own scaling a GTX 1060 goes from ~34 to ~14-15 ms at the crowd and
   ~11 in an empty street** — the GPU half of the target is met on paper. **The CPU half
   is not:** the crowd is 10.6 ms of pump time on this Ryzen 7 5700 at 1080p, and a 1060
   owner's CPU is typically 1.3-1.6x slower per thread.
5. **The pass extent census was blind from part 89 to part 106** (gotcha 530) and is
   fixed; the `pass: shadow cascade` class exists now.

## §0b Added 2026-09-09, before part 107 starts — THE CPU PLAN, THE CLOCK CAP, THE WORKER FLOOR

- **`docs/perf-plan-part107.md` IS THE PLAN FOR PART 107**: 60 fps minimum on a Ryzen 3
  3100-class CPU (the operator's instruction). Its §1 is the stand-in (`taskset -c
  0-3,8-11` + a 3.2 GHz cap), §2 the items in 4-core order, §3 the gates. Item 0 is a
  fresh decomposition under the stand-in; nothing is priced before it.
- **THE OPERATOR HAS CAPPED THIS BOX'S CPU AT 3200 MHz** (`sudo cpupower frequency-set -u
  3200MHz`) for the stand-in. Every number taken on the box until further notice is at
  3.2 GHz; the part-106 baselines were at stock (4654 MHz). **THE LAST ACTION OF PART
  107 IS TO REMIND THE OPERATOR TO RESTORE IT: `sudo cpupower frequency-set -u 4654MHz`**
  — they asked for that reminder explicitly. Put it in the closing message.
- **The thread budget's floor now gives a 4-physical-core machine THREE workers** (was
  two; operator instruction; `runtime/cpu/thread_budget.cpp`). `CZ_WORKERS=N` overrides.
  Item 1 of the plan measures whether the third worker on an SMT sibling is a gain on
  the stand-in.

## §1 Part 107 — autonomous, in order

0. **The mirror's remaining gates.** (a) An operator session at the crowd is OWED before
   it ships in a release — a wrong generation stamp is a one-frame stale or torn mesh
   that no headless number can see (gotcha 254's shape); ask them to soak the heaviest
   crowd and watch for any mesh that flickers or lags a frame, and hand them
   `CZ_VK_NO_STORE_MIRROR=1` as the A/B. (b) czamd's GPU column with the mirror
   (`tools/czamd/p104_full.ps1` with `CZ_VK_GPU_PASSES=1`): the RX 6600 is the nearest
   GPU-bound box the project can reach, and its 20.1 → ~8.5 ms prediction is the number
   to confirm. (c) A run on a card WITHOUT a 1 GB device-local quarter (a 3 GB 1060, a
   4 GB RX 570): the mirror halves until it fits and slots past its end stay on the host
   path — that path has never executed; `CZ_VK_PERSIST_MB=1024` on a box with a small
   heap, or a temporary cap, exercises it.
1. **The CPU half of the target, re-baselined at 1080p.** The crowd is 10.6 ms wall
   here (CPU-bound, fence 0.00). `part91-kickoff.md` §0d is the board: the PM4 walk
   (~3.1 ms serial, never decomposed), `UploadStream`'s resolve half (162 ns/draw), the
   record path (431 ns/draw). Every CPU millisecond converts 1:1 at the crowd on this
   box and on any 1060 owner's. Profile first (`CZ_VK_PROFILE`, mechanism only), then
   price, then build — the part-80 rule.
2. **The GPU items left, in order** (`perf-plan-part106.md` §5): the per-draw front end
   (~1.1 ms; pipeline switches 30% of draws, push constants 1.0/draw), the MSAA resolve
   (0.7), the shadow cascade (0.86), the post chain (0.4). None is a factor of two.
3. **The release.** `dist/` is at 482b47f and carries neither part 105's log file nor
   this. A build with the mirror is the one a 1060 owner needs; the operator decides
   v1.0.2-rebuilt vs v1.0.3, and the notes need a "Changed" line for the mirror with
   its control arm, plus the Deck lines from part 105.

## §1b Part 107 — the operator's

0. The mirror's eye test at the crowd (§1 item 0a).
1. The rebuild-or-v1.0.3 decision (part106-kickoff §1b item 0), now with the mirror in.
2. A GTX 1060, if one can be borrowed: `--diag`, then `CZ_VK_GPU_PASSES=1 CZ_FPS_LOG=10`
   at the heaviest crowd, both arms of `CZ_VK_NO_STORE_MIRROR`. Nothing in this plan is a
   measurement of a 1060; everything is a scaling of two other cards.
3. The Steam Deck items from part 106's kickoff stand (the RADV live-USB test, the
   first Deck report).

## §2 Instruments and arms this part added

`CZ_VK_GPU_STATS`, `CZ_VK_NULL_PS`, `CZ_VK_SCISSOR_1PX`, `CZ_VK_TRI1`, `CZ_VK_VRAM_STORE`
(measurement arms; `docs/instruments.md` "Part 106"), `CZ_VK_NO_STORE_MIRROR` (the
control of the shipped change). The campaign scripts and every trace and log are in
`~/DR2CZ-troubleshooting/part106-1080/` (`baseline.sh`, `arms.sh`, `arms2-5.sh`).
