# Part 87 kickoff — PERFORMANCE RESUMES, seeded by Case West's leads

> **THIS IS THE LIVE HAND-OFF**, superseding `part86-kickoff.md` (which remains the
> record of part 86's five subjects — read its §0b-§0d before touching saves, skills,
> or the pump).
>
> **The subject is PERFORMANCE**, by the operator's instruction closing part 86: a new
> session with "some lead Case West got on performance improvement". Performance was
> parked at part 81; part 86 already imported CW's pump-side stack and measured it
> (+2.3% weighted, +0.5-0.9 ms at the crowd — `part86-kickoff.md` §0d), so this
> session starts from CW's REMAINING leads, not from zero.

---

## 0. WHERE THE CASE WEST LEADS LIVE

The sibling checkout `~/GithubRepo/Dead_Rising_2_Case_West_Xenon_Recomp` (another
session works it — treat it READ-ONLY). Its perf campaign is parts 6-7:

| CW document | what it holds |
|---|---|
| `docs/perf-part6-notes.md` | their regime, board and rules; what they refuted |
| `docs/perf-part7-notes.md` | the record restructure (2a secondaries, 2b tickets/capture-resolve, 2c worker flip) with verdicts |
| `docs/part8-kickoff.md` | their live board — whatever NEW leads the operator means are probably here or newer |
| git log | `git log --oneline` — perf commits are well-labelled |

**Already imported and measured here (do not re-buy):** the pump trio — mid-walk rptr
publication, eager tick, fast-retry backoff (`CZ_PM4_NO_{MIDWALK_RPTR,EAGER_TICK,FAST_HELD}`
arms). **Already refuted on BOTH ports (do not re-buy):** parallel record/worker
execution — CZ part 80 (251 ns/draw driver share vs a 1.5 ms kill) and CW's 2c
(correct-and-NULL, pump steals 65% of ranges).

**The known wave-2 candidate:** CW's 2a/2b record restructure (secondary command
buffers + tickets with capture-time stream resolution) — their finding 69 credited the
serial restructure itself with real milliseconds ("each stage of making recording
distributable also made it cheaper serial"). It CANNOT be module-dropped: the two
renderers have diverged ~10k lines (CZ carries RT and more). Porting it is a real
project; re-price it against CZ's own decomposition (§6ec: record 524 ns/draw, ours
273) before writing code.

## 1. THE MEASUREMENT RULES, AS PART 86 RELEARNED THEM

* The rig is `tools/part80_crowdroute.sh` — but **it does not arm any instrument
  itself**: pass `CZ_VK_FRAME_TRACE=<out>.trace` per run or the campaign measures
  nothing (part 86 burned six runs learning this — and burned four more when a hung
  process from the container gate made every run refuse silently; run campaigns LOUD,
  never `>/dev/null`, and check `pgrep cz_runtime` first).
* Read with `tools/part80_trace_band.py "a=<glob>" "b=<glob>"` — banded medians,
  frame-weighted delta, and the monotone check. The low bands (<3,000 draws) read ~0
  for any CPU item — that is the regime, not a null.
* Three runs an arm is the doctrine; part 86's 2v2 was the operator's explicit time
  budget and said so in the record.
* The verify-the-HEAD rule on czwin, and never `>nul` inside an ssh chain
  (`windows-build-setup.md`, three incidents).

## 2. ALSO ON THE BOARD (carried from part 86, in priority order)

1. **The bundle save-squatter hunt** (`part86-kickoff.md` §0b) — saves are relocated
   to the OS saved-games dir now (migration verified both platforms), but the Windows
   create-flow bug that ate three saves is UNFOUND; the trace repro is specified.
2. **A natural level-up check** closes the health-report question (§0b(b)).
3. The glibc floor / AppImage; the silent cold-boot pre-warm polish; macOS.
4. The combo bench remains unexercised against phantom card grants (§0c residual).

## 3. GATES INHERITED

Everything in part86-kickoff §4 (the release gate set), plus: the ring-latency arms
must show their engagement counters on the `[vkprof] ring latency arms:` line
(eager ~59% of ticks, ~30 rptr stores/frame, ~2 held-fast naps/frame at the crowd);
`truncated=0`; and any new perf default ships WITH its off-arm and its measured
milliseconds in the same commit.
