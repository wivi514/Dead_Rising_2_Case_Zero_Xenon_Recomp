# Release programme — Windows, Linux, macOS (Apple Silicon)

> **THE DECISION, taken 2026-08-27.** *"We'll make it already recompiled for release to make
> it easiest for the player. Just drop in the game."* — **shape (A)**: ship a compiled
> `cz_runtime` per platform containing the recompiled image; the player supplies their own
> STFS package and nothing else.
>
> **STATUS: NOTHING BUILT YET.** This is the programme of work. Every number in §1 was
> measured today with the tool named beside it; everything else is an estimate and says so.
>
> The predecessor of this file was a decision document weighing three release shapes. That
> comparison is gone now the decision is made — it survives in git history and in
> `docs/part82-kickoff.md`.

---

## §1 THE EVIDENCE THIS PLAN IS BUILT ON — measured today, not assumed

Four things were checked before the plan was ordered, because three of them could have
changed it and one of them did.

**1.1 The runtime's entire non-portable surface is five files.** A census of platform headers
across `runtime/`:

```
kernel/memory.cpp        <unistd.h> <sys/mman.h>   the 4 GB map + three aliased physical views
cpu/crash_report.cpp     <unistd.h> <dlfcn.h>      sigaction handlers, dladdr symbolisation
cpu/guest_thread.cpp     <unistd.h>
cpu/debug_tunables.cpp   <sys/stat.h>
gpu/vk_renderer.cpp      <unistd.h>                one readlink("/proc/self/exe")
```

Everything else is `std::thread`, `std::filesystem`, `std::atomic` and SDL.

**1.2 The ARM64 architecture risk is retired, and most of it was already handled upstream.**
This was written into the previous plan as step 1's pre-registered kill — *"if the vector unit
falls back to scalar, macOS is a different project."* It does not.

* **SIMDe lowers every VMX operation the build calls "not optional" to NEON.** Compiled both
  targets freestanding (no sysroot needed) and disassembled:

  | op | aarch64 instrs | verdict | | op | aarch64 instrs | verdict |
  |---|---|---|---|---|---|---|
  | `min_epu32` | 2 | NEON | | `packs_epi16` | 3 | NEON |
  | `max_epu32` | 2 | NEON | | `packus_epi16` | 3 | NEON |
  | `blendv_ps` | 3 | NEON | | `packs_epi32` | 3 | NEON |
  | `shuffle_epi8` | 4 | NEON | | `cvtps_epi32` | 8 | NEON |
  | `dp_ps` | 5 | NEON | | `movemask_ps` | 9 | NEON |

  **Ten of ten, zero scalar fallbacks.** The reproducer is
  `/tmp/.../simde_spike.cpp` in the session scratchpad; it is ten one-line functions and
  should be re-run on the Mac itself as part of §3.C.
* **The generated code contains no raw x86 intrinsics at all.** Census over all 229 files in
  `ppc/`: `__builtin_ia32` 0, `immintrin` 0, `x86intrin` 0, and every `_mm_*` / `__m128*`
  token is `simde_`-prefixed. The only `__x86_64__` guard in the whole image is in
  `ppc_context.h`.
* **And that guard already has an aarch64 arm.** `ppc_context.h:260` handles the one thing
  SIMDe does not — denormal and rounding-mode control — with an `#elif defined(__aarch64__)`
  branch driving ARM's FPCR (`RoundShift 22`, FZ/FZ16 at bits 19/24). Likewise
  `cpu/timebase.h` is already `#if defined(__x86_64__)`-guarded and `cpu/timebase.cpp`
  already carries `mrs %0, cntvct_el0` for aarch64.

  **So the macOS risk is the OS, not the architecture** — 16 KB pages, `shm_open`, MoltenVK
  and notarisation. That is a different and smaller list than the plan assumed this morning.
  *(Upstream nit, harmless: the aarch64 `GuestToHost` comment reads "Nearest, Zero,
  -Infinity, -Infinity"; the code is right and the third entry is +Infinity.)*

**1.3 What could NOT be settled here, stated so nobody quotes a rigged result.** A full
generated TU compiled cleanly for x86_64 in 0.96 s but **could not be compiled for aarch64 on
this machine** — there is no aarch64 sysroot, and borrowing the x86_64 glibc headers produced
a cascade of `uint64_t`-is-`unsigned long long`-not-`unsigned long` type errors that are
artifacts of the hack and say nothing about the code. **That half of the spike needs a real
Mac or a real sysroot and is item §3.C.0.**

**1.4 The shader cache is derivable from the player's own disc — finding 6 is retracted.**
`tools/vo_microcode_probe.py` over the 1,571 shader objects in the three prologue banks,
against the 449-blob ucode oracle:

```
PS: 335 of 335 verbatim (100.0%)
VS:  81 of 103 verbatim + 16 tail-matched (head patched at load), 4 partial, 2 absent
RECOVERABLE: 432 of 438 = 98.6%
```

The retraction and the artifact that hid it for a year are in `xenia-capture-analysis.md` §6.
**The disc holds 1,571 shaders against the 449 accumulated over 25 parts and eleven operator
sessions — ~3.5x more than has ever been seen in play.**

---

## §2 WHAT SHIPS

### 2.1 The artifacts

| platform | artifact | built on | notes |
|---|---|---|---|
| Linux x86_64 | `CaseZeroRecomp-linux-x86_64.AppImage` + `.tar.zst` | this machine | AppImage bundles SDL2 + ffmpeg; Vulkan **loader stays the host's** |
| Windows x86_64 | `CaseZeroRecomp-windows-x86_64.zip` | this machine, clang-cl cross or a Windows box | vcpkg SDL2 + ffmpeg beside the exe |
| macOS arm64 | `CaseZeroRecomp-macos-arm64.dmg` | a Mac (unavoidable — notarisation) | signed + notarised `.app`, MoltenVK inside |

**No `assets/` content in any artifact.** With the first-run shader build (§3.D) that includes
no `.spv` either, so the only game-derived thing shipped is the recompiled image inside the
executable — which §1 of `part82-kickoff.md` states plainly rather than leaving implied.

### 2.2 The layout the player unpacks

```
CaseZeroRecomp/
  cz_runtime(.exe)              the host runtime + the recompiled image
  lib/                          bundled SDL2, ffmpeg (LGPL build), MoltenVK on macOS
  tools/                        extract_stfs.py + the image dumper (for the drop-in step)
  assets/
    package/PUT_YOUR_GAME_HERE.txt
    game/       (created by the first-run extract)
    shader_spv/ (created by the first-run shader build)
    save/       (created on first run)
  README.md  THIRD_PARTY.md  LICENSE
```

Every empty directory carries a `PUT_YOUR_GAME_HERE.txt` naming what goes in it and where it
comes from. **An empty directory in a zip is easy to lose; a directory with a file in it is
not.**

### 2.3 The player's first run, end to end

1. Drop the STFS package into `assets/package/`.
2. Launch. The runtime finds no `assets/game/`, runs the STFS extract itself (it is 825 MB in,
   832 MB out, ~30 s), and reports progress.
3. It finds no shader cache and builds one from the disc banks — **"Preparing shaders",
   ~1,571 of them, parallel across cores** (§3.D).
4. It pre-warms the pipeline cache in the same screen. Part 71 measured a *missing* pipeline
   cache at **17.8 s of stutter spread through play**; this moves that into the progress bar
   where a player expects it.
5. Game starts. Subsequent launches skip 2-4 entirely.

**If any step cannot proceed it must say what is missing, where it goes, and which command
produces it — then exit.** A black screen with one skipped-shader log line is the failure this
project has spent parts of its life diagnosing, and a shipped build must not hand that to a
stranger.

---

## §3 THE WORK, IN ORDER

Five milestones. A, B and E are required; C is macOS; D is required for the first-run
experience and is the biggest single item. Estimates are working days and are estimates.

### MILESTONE A — make the tree shippable at all (~3 days)

**A.1 `HostPaths` — anchor everything to the executable, not the CWD.** *(1 day)*
Today the VFS resolves `game:` to `../../assets/game` and the shader cache tries three
`../..`-relative candidates, which is why every documented command begins with
`cd runtime/build`. Replace with one helper: `/proc/self/exe` (Linux), `GetModuleFileNameW`
(Windows), `_NSGetExecutablePath` (macOS), plus a `CZ_ROOT` override.
**Gate:** every existing headless recipe passes from three different working directories.

**A.2 The first-run detector and its refusal messages.** *(1 day)*
Check package → extracted game → shader cache, in order; each missing one prints what and
where and exits non-zero. **Gate:** four runs — nothing present, package only, game only,
all present — each producing the right message. Break each check on purpose and confirm it
fires (gotcha 30).

**A.3 A release build type that is not `RelWithDebInfo`.** *(0.5 day)*
The default exists for `addr2line` on the recompiled image and is right for development.
A release wants `-O2`, no `-g`, stripped, with a separate `.debug` artifact kept locally so a
player's crash report is still resolvable.
**Gate:** `--smoke`, then the crowd route, then `part80_trace_band.py` says the frame is in
the same regime as the dev build — **a build-type change is a performance change until
measured.**

**A.4 Bundle the runtime deps and prove the bundle is what loads.** *(0.5 day)*
**Gate:** `ldd`/`otool -L`/Dependency Walker on the packaged binary in a clean container or
VM with no dev packages installed. A dependency that silently resolves to a system copy on
the build machine is the classic packaging defect.

### MILESTONE B — Windows (~5 days)

**B.1 `kernel/memory.cpp`.** *(2 days — the only genuinely fiddly port item)*
Today: a 4 GB `MAP_NORESERVE` reservation, then a `memfd_create` 512 MB region mapped
`MAP_FIXED` at **three** addresses (`0xA0000000`, `0xC0000000`, `0xE0000000`) so the guest's
three views of one physical range alias. Windows: `VirtualAlloc2` with
`MEM_RESERVE_PLACEHOLDER`, split the placeholders, then `MapViewOfFile3` a
`CreateFileMapping(INVALID_HANDLE_VALUE)` section three times with `MEM_REPLACE_PLACEHOLDER`.
Requires Windows 10 1803+. **Xenia does exactly this, so the pattern is proven.**
**Gate:** write through `0xA0000000`, read back through `0xC0000000` and `0xE0000000` — the
aliasing is the whole point and it is one unit test. Then `--smoke`, then the crowd route.

**B.2 `cpu/crash_report.cpp`.** *(1.5 days)*
`sigaction` → `AddVectoredExceptionHandler`; `dladdr` → DbgHelp `SymFromAddr`. **Keep the
guest-state report identical** — its host `pc` is the field that is never stale and is what
makes a fault diagnosable at all. **Gate:** fault on purpose in a known guest function and
confirm the report names it.

**B.3 The other three files + build.** *(1 day)* `readlink` → A.1's helper; `stat` →
`std::filesystem`. **clang-cl with the MSVC ABI**, not MinGW, so vcpkg's SDL2/ffmpeg link.
Clang is not optional: `ppc_context.h` uses `__builtin_assume` and the CMakeLists says every
one of the 57,822 generated bodies fails under GCC.

**B.4 Gates.** *(0.5 day)* `--smoke`; the headless DebugJump route reaches the crowd;
`part80_trace_band.py` against the Linux build. **A platform port is a same-binary A/B against
itself on another OS, and this project already owns the reader for it.**

### MILESTONE C — macOS on Apple Silicon (~5 days, plus procurement)

**C.0 Finish the spike on real hardware — do this first, it is half a day.**
§1.3 says what could not be answered here. On a Mac: build `ppc_image` alone for arm64 with no
SSE flags, re-run the ten-op SIMDe disassembly natively, and time a full 228-TU build.
**Nothing else in this milestone starts until that builds.**

**C.1 16 KB pages.** *(0.5 day)* Apple Silicon's page size is 16384 and `memory.cpp` does
`mprotect(base, 0x1000, …)` for the guest null page. A 4 KB request either fails or
over-protects three pages the guest expects writable. **This is a concrete bug waiting at
first run and it would present as a mystery.** Use `sysconf(_SC_PAGESIZE)` and decide
deliberately what "the null page" means when it is 16 KB wide.

**C.2 `memfd_create` → `shm_open`/`ftruncate`/`shm_unlink`.** *(0.5 day)* Same three aliased
views; `MAP_FIXED` over an anonymous reservation works on macOS. Same aliasing unit test as
B.1.

**C.3 The MoltenVK spike — the real unknown.** *(2 days, and it can force a redesign)*
The renderer uses **dynamic rendering** (`vkCmdBeginRendering`, core 1.3), a **bindless,
update-after-bind descriptor array** for the guest texture heap (~1,400 images), and **ray
query** for the parked RT path. MoltenVK covers the first two to a degree; **Metal's argument
buffer limits are the thing to check before promising anything**, and `VK_KHR_ray_query` is
simply absent.
**Answer three questions in this order:** does the bindless heap fit; does dynamic rendering
work as used; does the RT path compile out cleanly (it is already off by default and parked,
so this is a build-flag question, not a feature loss).
**Pre-registered kill: if the texture heap does not fit Metal's argument buffer limits,
macOS needs a descriptor redesign and is its own milestone — say so rather than absorbing it.**

**C.4 Signing and notarisation.** *(1 day + procurement)* Without notarisation Gatekeeper
refuses the app and the bug report is "it doesn't open". **Needs a paid Apple Developer
account — a decision with a fee attached, flagged now rather than at release time.**

**C.5 Gates.** *(0.5 day)* The same three as B.4.

### MILESTONE D — the first-run shader build (~6 days, the biggest item)

**D.1 Decode the `.vo`/`.po` container.** *(2 days)*
Where the microcode *begins* is not yet known as a rule, only as a search: the start offset
appears as a plain big-endian u32 in the object's first 0x80 bytes for only **34 of 416**
objects, so there is a real table to work out.
**The gate is free and exact: 416 known `(object, offset, length)` triples, and every
extracted blob must FNV-1a hash to a name already in the cache — the renderer already computes
that hash and already logs it (`[imload] VS va=… hash=… size=…`). 416 of 416 or the decoder is
wrong.**
**Fallback if the container resists:** the containment search that produced §1.4 already
works, and is gated identically. Ship that rather than block.

**D.2 In-process translation.** *(2 days)* Link XenosRecomp (MIT, sibling checkout, already
patched) and embed DXC. **Gate:** build the cache the new way and **diff the SPIR-V against
the 449 modules already on disk** — byte-identical is a stronger check than any picture test,
and a disagreement names the shader.

**D.3 The first-run pass and its progress UI.** *(1 day)* Both stages of §2.3, parallel across
cores, resumable, keyed so a partial run is detected and finished rather than restarted.

**D.4 The runtime fallback.** *(1 day)* 6 of 438 known blobs are not recoverable verbatim —
4 partial and **2 absent from the disc entirely** (96 B and 108 B, presumably
engine-synthesised) — plus 16 whose head the title patches at load. First sight of an unknown
hash translates in-process and adds to the cache. **This is the safety net, not an
optimisation:** it also covers anything the disc scan mis-parses.
**Gate:** delete a shader from the cache, confirm the fallback rebuilds it and the picture is
unchanged.

**What D retires:** "the cache is complete" as a claim with a shelf life; the six variant
caches drifting apart; and the name-diff gate that exists only because of that drift — a
defect that once cost three parts when the play cache was ten modules short.

### MILESTONE E — packaging, CI and the release itself (~4 days)

**E.1 CI, honestly scoped.** *(1 day)* GitHub Actions on `ubuntu-latest`, `windows-latest`,
`macos-14` **cannot produce the release — no runner has an XEX.** What it can do is compile
the ~30 host sources on all three platforms on every push and run `--smoke` against a stub
image, which catches the overwhelming majority of what a port breaks. **Say that in the
workflow's own comment, or a green tick will later be read as "the release builds".**

**E.2 The local release script.** *(1 day)* One command per platform: configure → build →
strip → bundle → package → checksum, emitting the artifact names in §2.1 and a manifest.

**E.3 `THIRD_PARTY.md`, generated not written.** *(0.5 day)* See §4; generate the attribution
block from what is actually linked so it cannot drift.

**E.4 README and troubleshooting.** *(0.5 day)* The drop-in steps, the first-run explanation,
and the bisection knobs of §5.

**E.5 The release checklist run end to end on a clean machine.** *(1 day)* §5.

---

## §4 LICENSING — settle before the first artifact leaves the machine

Building for yourself and shipping to others are different questions, and this repo has never
had to answer the second.

| component | licence | what shipping requires |
|---|---|---|
| this repo | PolyForm Noncommercial 1.0.0 | fine — but see ffmpeg; a GPL bundle would conflict |
| XenonRecomp / XenosRecomp | MIT | attribution |
| SDL2 | zlib | attribution |
| **ffmpeg (libavcodec/libavutil)** | LGPL, **or GPL if `--enable-gpl`** | **this machine's ffmpeg is `--enable-gpl`.** Ship an LGPL build, dynamically linked, notice + build recipe published |
| MoltenVK | Apache 2.0 | attribution (macOS) |
| o1heap, simde | MIT | attribution |
| DXC (embedded in D.2) | Apache 2.0 with LLVM Exceptions | attribution |
| UnleashedRecomp | **GPLv3 — structural reference only, no code** | nothing to ship; keep it that way |

---

## §5 THE RELEASE CHECKLIST — every artifact, every time

1. `cz_runtime --smoke` on the packaged binary, on a clean machine with no dev packages.
2. `ldd` / `otool -L` / Dependency Walker: nothing resolves outside the bundle except the
   Vulkan loader and system libraries.
3. First-run flow from nothing: drop package → extract → shader build → game starts.
4. The headless crowd route reaches ≥8,000 draws; `part80_trace_band.py` puts the frame in the
   same regime as the reference build.
5. `CZ_VK_VALIDATION=1`: **6 `topology-08773` and nothing else** — the standing baseline.
6. `CZ_VK_VERIFY_BIND_BATCH=1`: **0 disagreements**.
7. Shader cache: `shader_dim_census.py` clean; `rt_world_xform_census.py` exit 0.
8. **The part-81 bisection knobs are in the README's troubleshooting section**:
   `CZ_VK_NO_BIND_BATCH=1` and `CZ_VK_NO_DEVICE_PFN=1` are live by default with their
   milliseconds never measured, and a release is the first time a stranger runs them
   (`docs/part82-kickoff.md` §0). **Either price them before the first release, or document
   them as the first thing to try.**

---

## §6 RISKS, AND WHAT WOULD REFUTE EACH

| risk | severity | what settles it | when |
|---|---|---|---|
| **MoltenVK cannot host the bindless texture heap** | **high — could make macOS its own project** | C.3's three questions | before any macOS promise |
| The `.vo` container does not decode cleanly | medium | D.1's 416-of-416 gate; fallback is the search that already works | D.1 |
| Windows placeholder mapping misbehaves under a debugger or with EAF/ACG mitigations | medium | B.1's aliasing unit test, plus a run with Exploit Protection on | B.1 |
| A release build type changes the frame | low-medium | A.3's regime check | A.3 |
| ffmpeg bundling drags GPL in | low, but legally sharp | build LGPL explicitly and check `--enable-gpl` is absent | before E.2 |
| The two unpriced part-81 changes misbehave for a stranger | low | price them, or document the knobs | before release |
| Apple notarisation blocks at the last moment | low | C.4, done early, not at packaging time | C.4 |

---

## §7 OUT OF SCOPE

* **Intel Macs / x86 macOS.** The instruction is M1 and up.
* **32-bit anything; Steam Deck and handheld packaging.** Later, if ever.
* **Shipping the game DATA.** The package and everything extracted from it stay the player's
  to supply. Note that with shape (A) the recompiled image inside the executable is
  game-derived even so — stated plainly rather than left implied.
* **Performance work.** Parked since part 81. The one exception is the §5.8 checklist item,
  which is a release-safety question rather than a performance one.
* **Auto-update.** Not for a first release.

---

## §8 ORDER AND ROUGH TOTAL

```
A  shippable tree      3 d   ── required, blocks everything
B  Windows             5 d   ── independent of C
C  macOS               5 d   ── C.0 first; C.3 can force a redesign
D  first-run shaders   6 d   ── independent of B and C; the biggest item
E  packaging + CI      4 d   ── needs A; final step needs B, C, D
                      ─────
                      23 d   working days, sequential-worst-case
```

**A → (B ∥ C ∥ D) → E.** B, C and D touch disjoint files, so if any of them can run in
parallel the critical path is A + max(B, C, D) + E ≈ 13 days.

**Start with A.1 and A.2.** They are needed by every other milestone, they are the reason the
current build only runs from `runtime/build/`, and they turn "it doesn't work" into a sentence
that says why.
