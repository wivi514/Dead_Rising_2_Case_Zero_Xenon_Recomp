# Part 84 kickoff — Windows is DONE and the game plays on it; the release needs D.2 next

> **THIS IS THE LIVE HAND-OFF**, superseding `part83-kickoff.md`.
>
> **The subject is THE RELEASE.** `docs/release-plan.md` is the programme; its **§9 is the
> execution record** and is where every measurement lives.
>
> | document | what it is |
> |---|---|
> | **`release-plan.md` §9.4-9.6** | **part 83: milestone B complete, the stutter fixed, and what part 83 got WRONG** |
> | `docs/windows-build-setup.md` | the verified Windows runbook — read before touching the laptop |
> | `release-plan.md` §1.4, §3.D | D.1's retraction and the re-planned milestone D |
>
> Lessons: gotchas **495-500**.

---

## 0. THE ONE THING A NEW SESSION MUST KNOW

**The game builds, runs and PLAYS on Windows.** The operator has played Still Creek at
2560x1440 with sound. Milestones A and B of the release plan are complete; D.1 is done.

`ssh czwin` reaches the build laptop. **Read `docs/windows-build-setup.md` before doing
anything there** — it carries the non-obvious parts (which of three compilers per target,
the `administrators_authorized_keys` ownership rule, `-EncodedCommand` to escape three
quoting layers, `cmd /c` so PowerShell stops turning stderr into CLIXML).

**git is the ONLY way source moves between the two machines.** Edit and commit on Linux,
push, pull there.

## 1. WHAT PART 83 ESTABLISHED

* **Milestone B, all four items**, each gated by running rather than compiling. The memory
  map gained an aliasing self-test that guards the POSIX path too; the crash reporter is
  platform-neutral and three bugs were found inside it.
* **The stutter is fixed and it was never a Windows bug.** Pipelines were created lazily on
  the frame thread — 372 ms in one frame, four of five F7 marks within 45 frames of one.
  Pre-warming from a recorded key set took the worst spike **372 -> 173 ms** and the count
  of >20 ms spikes **6 -> 2 in 10,429 frames**. Every new player would have had this on
  every platform; the Linux machine only looked smooth because of a 29 MB cache no player
  will have.
* **Four Windows platform defects the operator found by PLAYING** — the VFS mounting on the
  CWD, a resolution list capped by DPI virtualisation, a thread budget sized off a guess,
  and a pipeline cache that could silently move. None of these was reachable by any gate
  this project owns.

## 2. WHAT TO DO NEXT, IN ORDER

1. **D.2 — in-process shader translation.** Still the hard prerequisite D.1 made it: the
   disc supplies the PIXEL half of the cache completely and the vertex half not at all, so
   a shipped build cannot draw a vertex shader without it. Link XenonRecomp (MIT, sibling
   checkout) and embed DXC. **Gate: build the cache the new way and diff the SPIR-V against
   the 449 modules on disk — byte-identical, and a disagreement names the shader.**
2. **D.4 then D.3**, in that order. D.4 (translate on first sight) is what makes the
   runtime able to draw at all; D.3 (the first-run pass over the 1,265 disc pixel shaders)
   is what makes it not stutter.
3. **Ship a pre-warm key file with the release.** The stutter tail is genuinely-new
   pipelines at 120-170 ms each, which no past session can predict. A key file from a full
   playthrough gives players a warm start on day one, and it is a few tens of KB.
4. **E — packaging**, once D lands. A Windows artifact is now buildable.
5. **C — macOS** remains blocked on hardware. C.4 is smaller than the plan assumed: the
   Apple account is OPTIONAL (release-plan C.4 carries the retraction).

## 3. WHAT IS OWED TO THE OPERATOR

* **Nothing measured.** Everything part 83 asked for was collected.
* **Their laptop's power profile matters**: quiet mode costs **34% frame rate**. Any future
  Windows measurement must state which profile it was taken in (gotcha 496).
* **A remaining crowd dip on Main Street** at ~7,000 draws is NOT a bug and not a
  regression: record 21 ms for 7,000 draws with the GPU comfortably underneath. That is
  this project's long-standing CPU-bound draw submission, and `perf-plan-part81.md` is the
  plan for it. It became noticeable only because the stutters that masked it are gone.

## 4. GATES PART 84 INHERITS

`--smoke` on both platforms including the packaged and stripped binary; the four
working-directory path check; the six-tree first-run check; `.text` identity between dev and
release; the clean-container bundle gate; `vo_extract_microcode.py --gate` at 343 of 345;
**the physical-aliasing self-test with its poison**; `CZ_VK_VALIDATION=1` at the standing
6 `topology-08773`; and part 81's bind verifier.

**Windows-specific standing checks:** the pre-warm must report `N of N`, not `N of M` — a
short count means something is truncating the key file (gotcha 497). And
`CZ_VK_FRAME_TRACE` must print its `-> open` line, or it is not recording (gotcha 495).
