# Release plan — GitHub releases for Windows, Linux and macOS (Apple Silicon)

> **STATUS: PLAN ONLY. Nothing here is built yet.** Written 2026-08-27, immediately after
> part 81 parked performance. The goal set by the operator: *"getting release for windows,
> linux and macos (for m1 and up) … that will not include game assets (user has to provide
> them themselves; we'll just make the assets folder empty and they'll have to put it in)."*
>
> Every claim about the current build below was measured or read out of the tree today, not
> remembered. Where a number has a shelf life it says so.

---

## §0-DECIDED THE SHAPE IS (A): SHIP IT RECOMPILED, THE PLAYER DROPS IN THE GAME

Operator instruction, 2026-08-27: *"We'll make it already recompiled for release to make it
easiest for the player. Just drop in the game."* **Shape (A) below is chosen.** The
comparison that follows is kept because it is what the decision was made against, and
because §0b's shader argument turned out to matter for a different reason than distribution.

**What that settles:** the release ships a compiled `cz_runtime` per platform containing the
recompiled image; the user supplies their STFS package and nothing else. No user-side
compiler, no image build, no module ABI. **§5 step 7 (`ppc_image` as a loadable module) is
therefore DROPPED**, and with it the argument for shape (C).

**What it does not settle, and what you should know you are choosing:** the artifacts are
derivative works of the game executable, and **the release cannot be built by public CI** —
no GitHub Actions runner has an XEX, so every artifact is built on your machine and
uploaded. §5 is re-ordered accordingly: CI still earns its place cross-compiling and
gate-testing the *host* sources on all three platforms, which is most of what breaks, but
the final artifact is a local build.

---

## §0 THE ORIGINAL DECISION — "no game assets" is not the whole boundary

The instruction is to ship no game assets and let the user supply them. That is right, and it
is **not sufficient**, because two things in a working build are derived from the user's game
executable and neither of them lives in `assets/`:

| artifact | size | what it is derived from | how it gets used |
|---|---|---|---|
| **`ppc/`** | **155 MB of C++, 228 TUs, 57,822 functions** | `assets/game/default.xex` run through XenonRecomp | compiled into `libppc_image.a` (**162 MB**) and linked into `cz_runtime` |
| **`assets/shader_spv/`** | **449 SPIR-V modules, 13 MB** | the title's own Xenos shader microcode, translated by XenosRecomp + DXC | **loaded from disk at run time.** There is no runtime translation path — a shader missing from the cache prints one line and its draws are skipped |

So **a prebuilt `cz_runtime` binary contains the whole game's translated code**, and the
shader cache is the game's own shaders translated. Handing someone a binary and an empty
`assets/` folder does not make the release asset-free; it moves the game from `assets/` into
the executable, where it is less visible rather than less present.

This is a real fork and the whole plan hangs on it, so it is stated before any work.

### The three shapes a release can take

**(A) Conventional binary release.** Ship compiled `cz_runtime` per platform; the user
supplies only their STFS package. This is what the recomp scene does (UnleashedRecomp,
Zelda64Recomp). *Cost:* almost none beyond the platform ports. *Consequence:* the artifacts
are derivative works of the game executable and of its shaders. Also: **the release cannot be
built in public CI**, because CI has no XEX — every artifact would have to be built on your
machine and uploaded by hand, for three platforms, forever.

**(B) Build-from-your-own-copy release.** Ship source plus a one-shot bootstrap; the user's
machine does STFS extract → XEX image dump → XenonRecomp → compile 228 TUs → build the shader
cache. *Consequence:* no game-derived byte ever leaves your machine. *Cost:* the user needs a
full C++ toolchain and ~15-30 minutes of compiling, which on Windows is a real barrier — and
**the shader cache cannot be built this way at all today** (see §0b).

**(C) Split release — RECOMMENDED.** Ship the **host runtime** prebuilt (it contains no
game-derived code) and have the user's machine produce **only the game-derived part**, loaded
at run time as a module rather than linked in.

`runtime/CMakeLists.txt` already builds the recompiled image as its own static library
(`add_library(ppc_image STATIC …)`, "kept separate from the executable so that a runtime-only
edit does not risk a rebuild of 227 TUs"). **Turning that static library into a dynamically
loaded module is the enabling change for the whole release**, and it buys three things at
once:

1. the GitHub release artifacts contain **zero game-derived bytes**;
2. **the release can be built by GitHub Actions** on all three platforms, from a public repo,
   with no game data on the runner — which is what makes this sustainable rather than a
   manual chore three times per release;
3. a runtime-only fix ships as a small download instead of a rebuild of the user's image.

*Cost:* the user still needs a compiler for the one-time image build, and the module boundary
has to be designed (§3).

**Recommendation: (C), with (A) as the fallback if you decide the licence position is one you
are comfortable with.** The platform work in §1 and §2 is required by all three shapes, so it
can start before the fork is decided — and that is how this plan is ordered.

### §0b THE SHADER CACHE — finding 6 IS RETRACTED AND THE DISC HAS THE SHADERS

**This section said the opposite this morning.** It said the cache could not be rebuilt from
the user's copy, on the strength of finding 6 — that the disc's `.vo` shader banks are "not
usable microcode". **Finding 6 is now retracted**, and the retraction is in place in
`docs/xenia-capture-analysis.md` §6 with the artifact explained.

`tools/vo_microcode_probe.py`, against the 449-blob ucode oracle over the 1,571 shader
objects in the three prologue banks:

```
VS: 103 blobs -> verbatim 81 (78.6%), tail-only/patched 16, partial 4, absent 2
PS: 335 blobs -> verbatim 335 (100.0%)
RECOVERABLE (verbatim or tail-matched): 432 of 438 (98.6%)
```

**Every pixel shader this project has ever seen the guest submit is on the disc, byte for
byte.** The original test compared *aligned* 8-byte n-grams against whole payloads; the
microcode is a sub-range starting at one of 163 distinct offsets, **86 of which are not
8-byte aligned**, so the test was blind to over half the population before it ran.

**The number that reframes the whole item: the disc holds 1,571 shader objects against the
449 this project accumulated over 25 parts and eleven operator sessions — about 3.5x more
than has ever been observed in play.**

So a first-run shader build is not a workaround for a distribution problem — with shape (A)
there is no distribution problem, since a 13 MB cache next to a 162 MB recompiled image
changes nothing. **It is a correctness win**, and that is now the reason to do it (§4).

## §1 WHAT EACH PLATFORM ACTUALLY NEEDS — measured, per file

The good news, and it is better than expected: **the whole runtime's POSIX surface is five
files.** A census of platform headers across `runtime/`:

```
kernel/memory.cpp        <unistd.h> <sys/mman.h>
cpu/crash_report.cpp     <unistd.h> <dlfcn.h>
cpu/guest_thread.cpp     <unistd.h>
cpu/debug_tunables.cpp   <sys/stat.h>
gpu/vk_renderer.cpp      <unistd.h>          (one readlink("/proc/self/exe"))
```

Everything else is already `std::thread`, `std::filesystem`, `std::atomic` and SDL.

### 1.1 Linux — nearly done; the work is packaging, not porting

* **Ports needed: none.** This is the development platform.
* **Asset paths are CWD-relative** — the VFS resolves `game:` to `../../assets/game` and the
  shader cache tries `../../assets/shader_spv`, `../assets/shader_spv`, `assets/shader_spv`
  in turn. That works from `runtime/build/` and breaks from anywhere else. A release needs
  paths **anchored to the executable**, which the renderer already half-does via
  `readlink("/proc/self/exe")` — generalise that into one `HostPaths` helper (§3.2) and use
  it everywhere.
* **Bundle or depend?** SDL2, Vulkan loader and libavcodec are hard `find_package`
  requirements at configure time, deliberately, so a silently-missing one is a build message
  rather than a mystery in the game. For a release: ship an **AppImage** (bundles SDL2 and
  ffmpeg, keeps the system Vulkan loader and ICD, which is correct — the loader must be the
  host's) plus a plain tarball for distro users.
* **ffmpeg licence: this box's ffmpeg is `--enable-gpl`.** Shipping that makes the whole
  bundle GPL, which collides with this repo's PolyForm Noncommercial licence. The release
  must bundle an **LGPL** ffmpeg built with only what is needed (`libavcodec`, `libavutil`,
  the XMA2 decoder), dynamically linked, with the LGPL notice and the build recipe published.

### 1.2 Windows — five files, plus one genuinely fiddly mapping

* **`kernel/memory.cpp` — the hard part, and it is a known emulator pattern.** Today: a 4 GB
  `MAP_NORESERVE` anonymous reservation, then a `memfd_create` shared 512 MB region mapped
  `MAP_FIXED` at **three** addresses (`0xA0000000`, `0xC0000000`, `0xE0000000`) so the guest's
  three views of one physical range alias each other. Windows equivalent: reserve with
  `VirtualAlloc2(… MEM_RESERVE_PLACEHOLDER)`, split placeholders, and
  `MapViewOfFile3(… MEM_REPLACE_PLACEHOLDER)` a `CreateFileMapping(INVALID_HANDLE_VALUE)`
  section three times. Requires Windows 10 1803+. Xenia does exactly this, so the pattern is
  proven.
* **`cpu/crash_report.cpp`** — `sigaction(SIGSEGV/SIGBUS/SIGILL/SIGTRAP)` →
  `AddVectoredExceptionHandler`; `dladdr` → DbgHelp `SymFromAddr`. Keep the guest-state
  report identical: it is the thing that makes a fault diagnosable, and its host `pc` field
  is the one that is never stale.
* **`gpu/vk_renderer.cpp`** — `readlink("/proc/self/exe")` → `GetModuleFileNameW`. Folded into
  the `HostPaths` helper anyway.
* **`cpu/guest_thread.cpp`, `cpu/debug_tunables.cpp`** — trivial (`stat` → `std::filesystem`).
* **Toolchain: clang is not optional.** `ppc_context.h` uses `__builtin_assume`, and the
  CMakeLists says in as many words that every one of the 57,822 generated bodies fails under
  GCC. Use **clang-cl** with the MSVC ABI (so vcpkg's SDL2/ffmpeg binaries link) rather than
  MinGW.
* **Dependencies** via **vcpkg** (`sdl2`, `ffmpeg[avcodec,avutil]`, `vulkan-headers`); the
  Vulkan **loader** comes from the user's driver, as on Linux.
* **Risk: none identified that is worse than tedious.** No inline asm, no Linux-only threading.

### 1.3 macOS on Apple Silicon — the two real unknowns

**The biggest risk is already retired.** `ppc_context.h` maps the guest's VMX unit onto
**SIMDe** (`simde__m128`, `simde_mm_*`), not raw SSE intrinsics — so the recompiled code is
portable to ARM64 by construction, and SIMDe lowers it to NEON. Had it been raw `<xmmintrin.h>`
this platform would have been a rewrite rather than a port.

Concrete work:

* **`-msse4.1 -mavx` are hardcoded** on `ppc_image` and must become conditional on the target
  architecture. Note the CMakeLists' warning that without SSE4.1 "simde silently falls back to
  scalar emulation, which is correct but very slow" — the ARM64 equivalent question is whether
  SIMDe's NEON lowering covers the packs and `min/max_epu32` the saturating vector arithmetic
  uses, or falls back to scalar. **That is a measurable spike, not a guess** (§5 step 1).
* **16 KB pages.** Apple Silicon's page size is 16384, and `memory.cpp` does
  `mprotect(base, 0x1000, …)` for the guest null page. A 4 KB mprotect on a 16 KB page host
  fails, or over-protects three pages that the guest expects to be writable. This is a
  concrete bug waiting at first run, and it is the kind that presents as a mystery.
* **`memfd_create` → `shm_open`/`ftruncate`/`shm_unlink`** for the three aliased physical
  views. `MAP_FIXED` over an existing anonymous reservation works on macOS.
* **`dlfcn.h`, `sigaction` and the rest are native.** `SIGBUS` matters more here than on Linux.
* **Vulkan is MoltenVK, and this is the genuine unknown.** The renderer uses **dynamic
  rendering** (`vkCmdBeginRendering`, core 1.3), a **bindless, update-after-bind descriptor
  array** for the guest texture heap (~1,400 images), and **ray query** for the parked RT
  path. MoltenVK supports dynamic rendering and descriptor indexing, but Metal's argument
  buffer limits are the thing to check before promising anything, and `VK_KHR_ray_query` is
  simply absent — the RT path must compile out cleanly (it is already off by default and
  parked, so this is a build-flag question, not a feature loss).
* **Distribution:** arm64-only per the instruction (M1 and up). Ship a signed and notarised
  `.app` in a `.dmg`; without notarisation Gatekeeper will refuse it and the bug report will
  be "it doesn't open". Needs an Apple Developer account — **a decision with a fee attached,
  flagged now rather than at release time.**

---

## §2 THIRD-PARTY LICENSING FOR REDISTRIBUTION — do this before the first artifact

Building for yourself and shipping to others are different licence questions, and this repo
has never had to answer the second one.

| component | licence | what shipping requires |
|---|---|---|
| this repo | **PolyForm Noncommercial 1.0.0** | fine, but see the ffmpeg row — a GPL bundle would conflict |
| XenonRecomp / XenosRecomp | MIT | attribution in the release notes |
| SDL2 | zlib | attribution |
| **ffmpeg (libavcodec/libavutil)** | LGPL, **or GPL if built `--enable-gpl`** | **the local build is `--enable-gpl`.** Ship an LGPL build, dynamically linked, with notice + build recipe |
| MoltenVK | Apache 2.0 | attribution (macOS only) |
| o1heap | MIT | attribution |
| simde | MIT | attribution |
| DXC (if embedded for §4) | LLVM Exceptions to Apache 2.0 | attribution |
| UnleashedRecomp | **GPLv3 — structural reference only, no code** | nothing to ship; keep it that way |

**Add a `THIRD_PARTY.md` and generate the release's attribution block from it**, so it cannot
drift from what is actually linked.

---

## §3 THE RELEASE LAYOUT AND THE FIRST-RUN EXPERIENCE

### 3.1 What the user unpacks

```
CaseZeroRecomp/
  cz_runtime(.exe)            the host runtime — NO game-derived code
  lib/                        bundled SDL2, ffmpeg (LGPL), MoltenVK on macOS
  assets/
    package/    (EMPTY)       <- the user drops their STFS package here
    game/       (EMPTY)       <- tools/extract_stfs.py fills this
    shader_spv/ (EMPTY)       <- filled by §4, or by the bootstrap
    save/                     created on first run
  tools/                      extract_stfs.py, the image dumper, the image builder
  README.md  THIRD_PARTY.md  LICENSE
```

Each empty directory carries a `PUT_YOUR_GAME_HERE.txt` naming exactly what goes in it and
where it comes from. An empty directory in a zip is easy to lose; a directory with a file in
it is not.

### 3.2 One `HostPaths` helper, and it is a prerequisite for every platform

Today the game root and the shader cache are found by walking `../../assets/…` relative to the
**current directory**, which is why every documented command starts with `cd runtime/build`.
A shipped build must anchor to the **executable's** location:
`readlink("/proc/self/exe")` (Linux) / `GetModuleFileNameW` (Windows) /
`_NSGetExecutablePath` (macOS), with a `CZ_ROOT` override. Small, mechanical, and it removes
a whole class of "works here, not there" bug reports.

### 3.3 First run must refuse clearly, not fail mysteriously

The runtime's existing discipline is that a missing dependency is a **build** message rather
than a silent defect at play time. Extend that to missing user-supplied data: on start,
check for the package, the extracted game and the shader cache, and if any is missing print
what is missing, where it goes and which command produces it — then exit. **A black screen
with one skipped-shader log line is exactly the failure this project has spent parts of its
life diagnosing; a shipped build must not hand that to a stranger.**

---

## §4 THE SHADER PIPELINE — a first-run build, and it is the UE4/UE5 model exactly

The operator's question: *"I do not know if we could do like multiple UE5/UE4 title to just
do a shader compilation the first time you start the game if the game doesn't detect you
already did it."* **Yes — and after §0b it is the better design, not the fallback.**

### 4.1 Why it is worth doing even though we can just ship the cache

With shape (A) we *could* ship `assets/shader_spv/` and be done. The reason not to:

* **The shipped cache is 449 modules. The disc has 1,571.** Ours is the set that eleven
  operator sessions happened to walk through. CLAUDE.md has warned for months that *"the
  cache is complete" is a claim with a shelf life* — trial mode, other save states, error
  screens are eras nobody has entered. A player who reaches one gets **one log line and
  skipped draws**, i.e. invisible objects, which is the single worst failure this renderer
  has because it looks like a game bug and reports as one.
* Building from the player's own disc makes that class **structurally impossible** rather
  than unlikely, and retires the six-variant cache drift and its name-diff gate along with
  it — a defect that once cost three parts when the play cache was ten modules short.
* It also drops 13 MB of game-derived shader data out of the download.

### 4.2 The design, which is two separate things people call "shader compilation"

**Stage 1 — TRANSLATE (microcode → SPIR-V). This is the one that needs the disc.**
On first run, if `assets/shader_spv/` is absent or stale: walk the disc banks, pull the
microcode out of each `.vo`/`.po`/`.scv`, run it through XenosRecomp (MIT, already a
sibling checkout, already patched) and DXC, and write the cache. ~1,571 shaders,
parallel across cores. Progress screen, one time, then never again.

**Stage 2 — PIPELINE CREATION (SPIR-V → your GPU's code). This is what a UE5 first-run
screen literally is,** and it is the one the player actually feels. Part 71 measured a
missing pipeline cache at **17.8 seconds of stutter**, found it, and shipped the cache —
so the mechanism already exists and is already warmed lazily during play. A first-run
pre-warm pass over the known pipeline states would move that cost into the same progress
screen where the player expects it.

**Doing both in one "Preparing shaders" step is the whole answer to the question**, and
stage 2 is the half that makes the game feel smooth on first launch.

**Stage 3 — the runtime fallback, which is small but not optional.** 6 of 438 known blobs
are not recoverable verbatim: 4 partial and **2 absent from the disc entirely** (96 B and
108 B — presumably engine-synthesised), plus 16 whose head the title patches at load. So
first sight of an unknown microcode hash must translate it in-process and add it to the
cache. That path also covers anything the disc scan mis-parses, which is why it is the
safety net rather than an optimisation.

### 4.3 The unsolved piece, and the gate that makes it safe

**Where inside a `.vo`/`.po` the microcode begins is not yet known as a rule** — only as a
search. The start offset appears as a plain big-endian u32 in the object's first 0x80 bytes
for just **34 of 416** objects, so the container has a real table to decode.

**That work has an unusually strong gate, and it is free:** 416 known
`(object, offset, length)` triples, and every blob a decoder extracts must FNV-1a hash to a
name already in `assets/shader_spv/` — the renderer already computes exactly that hash and
already logs it (`[imload] VS va=… hash=… size=…`). **416 of 416 or the decoder is wrong.**
Then the end-to-end gate: build the cache the new way and diff the SPIR-V against the 449
modules already on disk. Byte-identical output is a stronger check than any picture test,
and a disagreement names the shader.

**Fallback if the container resists decoding:** the search that produced the numbers above
already works — scan each object for the microcode's own structure and validate by hash.
Uglier, and gated identically.

## §5 ORDER OF WORK, AND WHY — re-ordered for shape (A)

Each step ends in something checkable.

1. **The macOS ARM64 spike — half a day, and it is first because it can refute the plan.**
   Compile `ppc_image` alone for arm64 with SIMDe and no SSE flags. Two questions: does it
   build, and does SIMDe lower the VMX packs and `min/max_epu32` to NEON or fall back to
   scalar? **Pre-registered kill: if the vector unit goes scalar, macOS is a different
   project** — say so before anything is promised. Nothing else is worth starting first.
2. **`HostPaths` + the first-run check (§3.2, §3.3).** Platform-independent, needed by every
   step after it, and it makes the current build runnable from outside `runtime/build/`.
3. **The Windows port** — the five files, clang-cl, vcpkg. Gates: `--smoke`, then the
   headless DebugJump route reaches the crowd, then `tools/part80_trace_band.py` says the
   frame sits in the same regime as Linux. **A platform port is a same-binary A/B against
   itself on another OS**, and this project already owns the reader for it.
4. **The macOS port** — the same five files plus 16 KB pages, `shm_open`, and the MoltenVK
   spike (bindless heap size, dynamic rendering, RT compiled out). Same three gates.
5. **The `.vo`/`.po` container decoder (§4.3)** — gated at **416 of 416** against the known
   triples. This is the step that unlocks the first-run build.
6. **The first-run shader build (§4.2)**, both stages, gated on the SPIR-V diff against the
   449 modules already on disk, then on a play session that reaches an era the 449 never
   covered.
7. ~~`ppc_image` as a loadable module~~ — **DROPPED.** Shape (A) is chosen; there is no
   user-side image build, so there is no module boundary to design.
8. **CI, honestly scoped.** GitHub Actions on `ubuntu-latest`, `windows-latest`, `macos-14`
   cannot produce the release — no runner has an XEX. What it *can* do is compile the ~30
   host sources on all three platforms on every push and run `--smoke` against a stub
   image, which catches the overwhelming majority of what a port breaks. **Say that in the
   workflow's own comment**, or someone will later assume the green tick means the release
   builds.
9. **Packaging + release automation** — AppImage / `.zip` / notarised `.dmg`, a tag → build
   → attach flow driven from a local build script, and `THIRD_PARTY.md` generated rather
   than hand-written.

**A pre-release checklist item that is not obvious:** part 81 left the **vertex bind batch**
and the **device command table** on by default with their milliseconds never measured. A
release is the first time a stranger runs those. Either price them first, or put
`CZ_VK_NO_BIND_BATCH=1` / `CZ_VK_NO_DEVICE_PFN=1` in the README's troubleshooting section as
the first bisection for any picture complaint (`docs/part82-kickoff.md` §0).

## §6 EXPLICITLY OUT OF SCOPE

* **x86 macOS and Intel Macs.** The instruction is M1 and up. Rosetta 2 would run an x86_64
  build but MoltenVK under translation is not a configuration worth supporting.
* **32-bit anything, and Steam Deck / handheld packaging.** Later, if ever.
* **Shipping the game DATA.** The STFS package and everything extracted from it stay the
  user's to supply — that is the instruction and it does not change. Note that with shape
  (A) the recompiled image inside the executable is game-derived even so; §0-DECIDED states
  that plainly rather than leaving it implied.
* **Performance work.** Parked as of part 81. Note that two changes are live and ON by
  default whose milliseconds were never measured (`docs/part82-kickoff.md` §0) — **a release
  is the first time a stranger runs those**, so the bind batch and the device command table
  should be on the pre-release checklist as the first bisection for any picture complaint.
