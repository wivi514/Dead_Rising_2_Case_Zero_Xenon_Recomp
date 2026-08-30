# Part 86 kickoff — MILESTONE E IS COMPLETE; the release programme is A+B+D+E done

> **THIS IS THE LIVE HAND-OFF**, superseding `part85-kickoff.md`.
>
> **The subject is THE RELEASE.** `docs/release-plan.md` is the programme; its **§9 is the
> execution record** and **§9.8 is part 85** — every measurement in this hand-off lives
> there in full.
>
> | document | what it is |
> |---|---|
> | **`release-plan.md` §9.8** | **part 85: milestone E — both artifacts, every §5 gate, CI** |
> | `release-plan.md` §3.C | milestone C, macOS — the one milestone left, blocked on hardware |
> | `tools/release/README.md` | what a player reads; ships in both bundles |

---

## 0. THE ONE THING A NEW SESSION MUST KNOW

**The release exists.** `dist/CaseZeroRecomp-linux-x86_64.tar.zst` (26 MB) and
`dist/CaseZeroRecomp-windows-x86_64.zip` (21 MB) are built, and the §5 checklist ran end
to end for exactly this state — clean-container gate (which now also runs the WHOLE
first-run flow and a DXC translation inside the container), `.text` identity on matched
configures, validation at the standing 6 `topology-08773` and nothing else, bind-batch
verify 0 of 87 M, both censuses, the disc gate.

A player's first run is now the §2.3 flow for real: drop the package in, double-click,
and a progress window ("PREPARING FIRST RUN", one bar) covers the in-process STFS
extract (part 85, byte-identical to the Python reference) and the disc shader build;
`cz_defaults.env` beside the executable turns the renderer on without touching any dev
recipe or control arm. Three things it is easy to get wrong later:

* **The bundle must carry the DXC library** — the packaging scripts fail without it and
  the container gate proves the dlopen by translating a real shader with `HOME` unset.
  It was missing, and every static check passed anyway (ldd cannot see a dlopen).
* **`cz_defaults.env` is defaults only** — the environment always wins, each application
  prints a `[defaults]` line, and a dev tree (no file) is byte-for-byte unaffected.
* **DXC's licence is University of Illinois/NCSA**, not Apache-2.0-LLVM — §4 corrected
  in place, licence text vendored and shipped on both platforms.

**CI exists (E.1)** and its comment says exactly what a green tick means: host sources
compile + `--smoke` on a STUB image (`tools/gen_stub_ppc.py`), ubuntu + windows, from
upstream clones at pinned bases plus `tools/ci/*.patch` (37 local recompiler commits;
`tools/ci/regen_patches.sh` after ANY commit to either checkout). Linux leg green on its
first run.

## 0b. TWO GAMEPLAY DEFECTS FROM THE COMPLETION RUN — now the top of the list

The operator completed the whole game on the shipped Windows bundle and reported two
defects. **These outrank everything below: a broken save in the shipped artifact is a
release blocker.**

**(a) BUNDLE SAVING IS BROKEN ON WINDOWS.** They made THREE saves in different slots;
none exists anywhere on the disk (searched `C:\cz` and `C:\Users` for `*.DSF`). The
evidence that survives: the dev tree (where saving works — their 2026-08-21 completion)
holds the healthy shape, a container DIRECTORY `assets\save\DR2P000.DSF\` with the real
303 KB file inside; the bundle instead held a **1-byte FILE named `DR2P000.DSF` at the
top of save/**, created ~2 minutes into the first bundle sitting and never modified
through three sittings — a file squatting on the container directory's name would make
every subsequent `XamContentCreate`/`NtCreateFile(save:\DR2P000.DSF)` fail, silently if
the title ignores the error. What created a 1-byte FILE there is the question; the
Linux bundle-rooted boots derive the same save root correctly (`content: saves live in
<root>/assets/save`), so suspect the Windows path/create flow, not the root derivation.
**Repro without a playthrough**: bundle-rooted run on czwin, reach gameplay (DebugJump),
save once, with `CZ_FILE_TRACE=1` — the trace prints every create/write WITH ITS
DESTINATION (gotcha 267's rule) and will name the failing operation in one run.
**Part 85's repackage wipe destroyed the bundle's save dir before this was understood**,
so the 1-byte file itself is gone; the repro recreates the evidence.

**(b) LEVEL-UP REWARDS DID NOT APPLY.** Leveled 1 → 5 during the completion run and
never received the health increase. Unknown whether this is bundle-specific or
long-standing (ASK: did the 2026-08-21 dev-tree completion grant health on level-up?
— an unreported absence is not evidence of absence). Plausible coupling to (a): if the
title routes stat persistence through the profile/save subsystem, a failing save path
could swallow rewards — establish (a) first, then re-test (b) on the fixed build before
treating it as its own defect.

## 1. WHAT TO DO NEXT, IN ORDER

1. ~~**Check the Windows CI leg's first run**~~ — **DONE, still inside part 85: BOTH CI
   LEGS ARE GREEN.** Two real failures, neither in the dependency step: the MSVC
   RuntimeLibrary mismatch (CI now passes `MultiThreadedDLL` to XenonRecomp, as czwin
   does) and undeployed vcpkg DLLs killing `--smoke` at process load with no output
   (now copied beside the exe). Diagnosis ran through the `windows-logs` artifact the
   workflow now always uploads — the public API serves verdicts, not logs — fetched
   tokenlessly via nightly.link. Full trail: `release-plan.md` §9.8's owed list, updated
   in place.
2. ~~**The pre-warm pipeline-key file**~~ — **SHIPPED, still inside part 85.** The
   operator's bundle sitting doubled as the harvest (583 keys); the renderer seeds from
   `<exe>/prewarm.keys` only when no per-user file exists; both packaging scripts
   header-check it and the container gate requires it. Cross-platform proven by use
   (Windows-written keys pre-warmed 421 pipelines on Linux, 19.2 s moved out of play).
   §9.8's updated entry is the record.
3. ~~**An operator sitting on the SHIPPED bundle**~~ — **DONE, same sitting**: they
   played the Windows bundle end to end from a drop-in first run (extract, prebuild, 64
   first-sight vertex shaders, a save written) and called it "pretty close to the Linux
   build". Performance beyond that is the PARKED subject, not a release defect.
4. **The glibc floor on the Linux artifact** — now the only self-servable item: build on
   an old base image or produce the AppImage §2.1 names. The packaging script prints
   the limitation honestly meanwhile.
5. **Owed polish, small**: a true cold first boot runs the ~19 s pipeline pre-warm
   SILENTLY after the progress window closes (§2.3 item 4 wanted it under a visible
   screen). One-time, and only when no per-user key file exists.
6. **C — macOS**, still blocked on hardware; §1.2 already retired the ARM64 risk.

## 2. WHAT IS OWED TO THE OPERATOR

* Item 4 above is wholly theirs. Item 2 needs them once (a normal play session with
  `CZ_CAPTURE_KEY` — the recorder `tools/play_session.sh` already arms).
* Nothing else: every part-85 gate was run and recorded in §9.8.

## 3. THINGS PART 85 SETTLED THAT SHOULD NOT BE RE-DERIVED

* **The extract's byte identity**: 256 of 256 files identical to `tools/extract_stfs.py`
  over the real package. The Python stays as reference and SVOD fallback; the two are
  deliberate duplicates gated by `--extract-package` + `diff -r`, same contract as the
  shader translator pair.
* **`default.xex` is written LAST** on purpose — an interrupted extract must not
  counterfeit a complete tree. A truncated package fails naming the entry and offset.
* **The progress window is plain SDL, pre-Host_WindowInit, calling-thread-only** — the
  prebuild's progress callback fires only from the calling thread's worker because SDL
  draws only from the creating thread. Screenshot-verified mid-flow.
* **`.text` identity needs MATCHED configures** (prefixes + CZ_BUNDLE_RPATH + build type
  differing only in build type). Dev-vs-release compares different SDL/ffmpeg headers
  and a relocated image; the script explains this when it fails that way.
* **The stub-ppc scanner's two traps**: no word boundary exists inside `__imp__sub_`
  (underscore is a word character), and X-macro hook lists carry addresses as bare hex
  first arguments. Both cost a real link error before they were found.
* **A stray `cz_runtime` from a dead session filled /tmp with a 22 GB draw-trace log**
  and killed this session's shell mid-part (EDQUOT; /tmp is the RAM tmpfs). If bare
  `echo` starts failing, read the memory file `tmp-is-a-ram-tmpfs` FIRST: check
  `du -sh /tmp/*`, look for a still-running writer (`fuser`), kill by pid.

## 4. GATES PART 86 INHERITS

Everything part 85 inherited (part85-kickoff §4), plus:

* `tools/release_gate_clean_container.sh` — now REQUIRES the package and a ucode blob
  (`CZ_GATE_PACKAGE`/`CZ_GATE_UCODE` override), and fails unless the in-container
  first-run flow, the DXC translation, and every release file check pass.
* `--extract-package` vs the Python reference: `diff -r` clean over the real package.
* The fake-root first-run boot: package-only tree + `CZ_VKDRAW=1` → extract, prebuild
  `1265 translated, 0 failed`, `no translated shader` = 0.
* The defaults contract, both directions: `[defaults]` line fires from
  `cz_defaults.env`; `CZ_VKDRAW=0` in the environment suppresses it.
* `tools/release_package_windows.ps1` must end with the staged exe passing `--smoke`,
  and its `[shxlate]` line must name the STAGED dxcompiler.dll.
* CI stays honest: if the workflow grows a claim beyond "compiles + stub smoke", its
  comment must grow with it.
