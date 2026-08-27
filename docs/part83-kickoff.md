# Part 83 kickoff — the release programme is running; A is complete, D.1 is done and it moved D

> **THIS IS THE LIVE HAND-OFF**, superseding `part82-kickoff.md`.
>
> **The subject is THE RELEASE.** The operator's instruction opening part 82: *"Do the
> release plan."* `docs/release-plan.md` is the programme and it now carries its own
> execution record in §9 — read that before anything else in this file, because it is where
> every measurement lives.
>
> | document | what it is |
> |---|---|
> | **`docs/release-plan.md` §9** | **what part 82 built, what each gate measured, and §9.2 what is OWED** |
> | `docs/release-plan.md` §1.4 | the retraction: the vertex half of the disc shader claim was wrong, and why |
> | `docs/release-plan.md` §3.D | milestone D, RE-PLANNED around D.1 |
> | `docs/part82-kickoff.md` | performance, which is still parked. Its §0 still stands |
>
> Lessons: gotchas **484-489**.

---

## 0. THE ONE THING A NEW SESSION MUST KNOW

**There is a Linux release artifact now, and it works.** `dist/CaseZeroRecomp`, 16 MB
compressed, and it runs in a container with none of this machine's development packages.

```
tools/build_ffmpeg_lgpl.sh                 # once — LGPL, xma1+xma2 only, 120 deps -> 3
tools/build_sdl2.sh                        # once — REAL SDL2, not Fedora's sdl2-compat shim
cmake -S runtime -B runtime/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCZ_FFMPEG_PREFIX=$PWD/thirdparty/ffmpeg-lgpl -DCZ_SDL2_PREFIX=$PWD/thirdparty/sdl2
cmake --build runtime/build-release -j$(nproc)
tools/release_package_linux.sh             # -> dist/CaseZeroRecomp + the .tar.zst
tools/release_gate_clean_container.sh      # A.4's gate. Must say GATE PASSED
```

**The two part-81 changes are still shipping ON BY DEFAULT with their milliseconds never
measured** (`part82-kickoff.md` §0). If a picture complaint arrives, `CZ_VK_NO_BIND_BATCH=1`
and `CZ_VK_NO_DEVICE_PFN=1` are still the first bisection — and a release is the first time a
stranger runs them.

## 1. WHAT PART 82 ESTABLISHED

* **Milestone A is complete and gated.** Paths anchored to the executable and printed once;
  an honest first-run refusal; a `Release` build type whose `.text` is byte-identical to the
  dev build's; and a bundle proven in a clean container. Details and gates in §9.1.
* **D.1 is done and it decoded the container.** `microcodeStart = u32@0x04 + u32@(u32@0x18)`,
  length = objectLength - start, 423 of 423, with an exact free gate (every extracted blob
  must FNV-1a to a name in the cache).
* **AND IT RETRACTED THE PLAN'S §1.4.** Pixel shaders **343 of 345** recoverable from the
  disc byte-for-byte; vertex shaders **0 of 104**. The old 98.6% was a 48-byte head probe
  matching a shared vertex-shader prologue. The title patches vertex FETCH instructions at
  load, so a vertex shader cannot be pre-translated from the disc at all.
* **The disc holds 1,265 distinct pixel shaders** against the 345 this project accumulated
  over 25 parts and eleven operator sessions.

## 2. WHAT TO DO NEXT, IN ORDER

1. **D.2 — in-process translation.** It was a nicety in the plan and D.1 promoted it to a
   hard prerequisite: without it a shipped build has no vertex shader at all. Link XenosRecomp
   (MIT, sibling checkout, already patched) and embed DXC. **Gate: build the cache the new way
   and diff the SPIR-V against the 449 modules on disk — byte-identical, and a disagreement
   names the shader.**
2. **D.4 then D.3**, in that order and not the plan's. D.4 (translate on first sight of an
   unknown hash) is what makes the runtime able to draw; D.3 (the first-run pass over the
   1,265 disc pixel shaders) is what makes it not stutter. The standing gate for D.4 is free:
   start with an EMPTY vertex half and confirm the crowd route reaches 8,000 draws with
   `no translated shader` = 0.
3. **The five things §9.2 says are owed** — the unmeasured ffmpeg/SDL2 swap, the missing
   `nasm`, the glibc floor, the missing AppImage, and `build_shader_spv.sh` not being
   shippable (which D.2 fixes).
4. **B (Windows) and C (macOS) are blocked on hardware this machine does not have.** B needs a
   Windows toolchain; C needs a Mac and its C.0 spike gates everything else in that milestone.
   Neither is a reason to stall — D is independent of both.

## 3. WHAT IS OWED TO THE OPERATOR

* **Nothing measured.** Part 82 launched no game and asked for no verdict.
* **One decision with a fee attached, flagged early rather than at packaging time:** C.4, the
  paid Apple Developer account. Without notarisation macOS refuses the app and the bug report
  is "it doesn't open".
* **One free improvement to the shipped artifact:** `sudo dnf install nasm`, then re-run
  `tools/build_ffmpeg_lgpl.sh`. Without it the bundled decoder is built from C with no
  hand-written x86 assembly — correct, and slower on the audio path.

## 4. GATES PART 83 INHERITS

`--smoke` OK on every build, including the packaged and stripped one. The four-working-directory
path check. The six-tree first-run check. `.text` identity between the dev and release builds.
The clean-container bundle gate. `vo_extract_microcode.py --gate` at 343 of 345.
**Nothing in `gpu/`, `kernel/`, `cpu/` or the PM4 path changed except one include and two path
candidate lists in `vk_renderer.cpp`**, so part 74's A5 and `alu_const_gate` sweeps, part 75's
cache gates, part 78's barrier gates, part 80's PM4 boundary oracles and part 81's bind
verifier all stand.
