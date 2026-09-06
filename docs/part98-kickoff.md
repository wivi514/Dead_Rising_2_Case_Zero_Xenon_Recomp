# Part 98 kickoff — async pipeline creation: the public stutter reports, fixed in-tree; v1.0.1 IS OWED

**Supersedes `part97-kickoff.md` on "where the port is".** Read `CLAUDE.md` first as
always; the full record of this part is `phase5-notes.md` §6eq, the plan with its
predictions `docs/async-pipeline-plan.md`, and the transferable findings are gotchas
508 and 509.

## §0 What part 98 did (2026-09-06, overnight, operator instruction: "Do both of them but start with 1")

Players reported heavy stutter within a day of the public release (the operator's
summary of the Reddit release thread — the thread is bot-walled from this network;
if an issue lands, ask the reporter: **does it fade by your second session?**).
Part 98 reproduced it in one run and fixed it, three commits, defaults ON:

1. **55a9d4e — async pipeline creation on miss.** `GetPipeline`'s build body is now
   pure (`BuildPipelineObject`); a miss hands the key + `ShaderMeta` copies to one
   worker; draws skip under their own counter while it builds (the shaderjit visual
   contract, one level up). Boot pre-warm stays synchronous.
2. **0c50a4d — the pre-warm chain.** Session one pre-warms **0 of 1,365** (gotcha
   508: every key names a first-sight-only vertex shader), so parked keys now build
   the moment `shaderjit::Drain` delivers their shader — ahead of the first draw,
   including ~850 for areas the session never visits.
3. **49c895c — two-tier FIFO queue.** The first promotion design was LIFO under
   burst (gotcha 509). Urgent tier (a draw is skipping on it) drains before the
   speculative tier, FIFO within each.

**The numbers (session-one simulation, DebugJump crowd route, one run per arm, all
structural counters or >10x effects):** sync arm = 8.9 s frame-thread compiling,
worst frames 3.7 s / 2.6 s, four 250-519 ms outdoor hitches, 35 frames >33 ms.
Shipping arm = **0 frame-thread creates, zero outdoor frames >100 ms, 5 frames
>33 ms**, 757 k skipped draws (materials pop in late at area arrivals, once per
install — the deliberate trade). **Session two: 1,083 of 1,083 pre-warm at boot in
101 ms, zero skips** — the loop self-heals. Validation: only the standing
topology-08773 class. Arms: `CZ_VK_SYNC_PIPELINE=1` (whole part-83 behaviour),
`CZ_VK_NO_PREWARM_CHAIN=1` (chain only). The session-one simulation recipe and all
six runs: `~/DR2CZ-troubleshooting/part98/`, harness in the scratchpad's
`freshplayer_arm.sh` (three env vars: fresh `CZ_ROOT`, fresh `XDG_CACHE_HOME`,
`MESA_SHADER_CACHE_DISABLE=true`).

## §1 What is owed, in order

0. **THE OPERATOR'S OWN VERDICT FIRST (their instruction, 2026-09-06): they will
   test the fix on a NEW PC** — a true session-one machine, the exact population the
   reports come from and better than any simulation here. Everything below waits on
   it; a headless number does not outrank their report.
1. **v1.0.1.** The fix is in the tree, NOT in the published artifacts. Rebuild both
   (linux here, windows via czwin — remember the standing note that the shipped
   Windows bundle must be built at head), re-gate (`release_text_identity.sh`,
   `release_package_linux.sh`, container gate, czwin ps1), write release notes with
   fresh SHA-256s, publish. **The release is frozen at the tag** — a rebuild
   refreshes the recorded hashes first. The operator may want to feel session one
   themselves first: wipe `~/.cache/cz-recomp` and run with
   `MESA_SHADER_CACHE_DISABLE=true`.
2. **Watch the thread/issues for the fingerprint.** A stutter report that does NOT
   fade by session two is a different defect — do not close it against part 98.
   The picture-complaint bisection order gains a step: `CZ_VK_SYNC_PIPELINE=1`
   FIRST for any "objects appear late / missing for a moment" report, then the
   part-90 order (`CZ_VK_NO_DEFERRED_CLEAR=1`, `CZ_VK_DEFER_FULL_RECT=1`,
   `CZ_VK_NO_PAR_RECORD=1`, `CZ_VK_NO_TEX_LRU=1`).
3. **Parked lever, priced:** a second async worker would roughly halve the
   session-one burst pop-in (757 k skips concentrate at area arrivals; one worker
   serializes 10-40 ms builds). Not spent because promotion already bounds hot
   waits and the thread-budget question (part 80 granted `record` zero threads) is
   an operator-rule conversation, not a unilateral one.
4. Everything in `part97-kickoff.md` §1 that part 98 did not touch still stands:
   repo topics, AppImage/glibc floor, macOS (milestone C), Case West next, the
   hair flicker, performance/RT parked.

## §2 For Case West

Lift the whole shape: `pipelinejit` + the chain + gotchas 508/509 transfer verbatim
(same first-sight-only vertex shader situation by construction), and the
session-one simulation is the release gate that was missing — run it BEFORE
shipping, not after the reports arrive.
