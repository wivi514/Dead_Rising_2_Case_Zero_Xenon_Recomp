# Release programme — Windows, Linux, macOS (Apple Silicon)

> **THE DECISION, taken 2026-08-27.** *"We'll make it already recompiled for release to make
> it easiest for the player. Just drop in the game."* — **shape (A)**: ship a compiled
> `cz_runtime` per platform containing the recompiled image; the player supplies their own
> STFS package and nothing else.
>
> **STATUS as of 2026-08-27 (part 82): MILESTONE A IS COMPLETE AND GATED, AND D.1 IS DONE
> AND RETRACTS §1.4.** What was NOTHING BUILT YET is now a Linux artifact that runs in a
> clean container, refuses honestly when the game is missing, and carries no GPL. §9 at the
> bottom of this file is the execution record, item by item, with what each gate measured.
>
> Every number in §1 was measured with the tool named beside it — **except §1.4, whose
> vertex-shader half is RETRACTED in place below.** Everything else is an estimate and says
> so.
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

  **Ten of ten, zero scalar fallbacks.** The reproducer is `tools/arm64_spike/run.sh` —
  it compiles freestanding so it needs no aarch64 sysroot, exits 1 on any scalar fallback,
  and should be re-run natively on the Mac as item C.0. **It proves the LOWERING, not that
  the full image builds; those are different claims and §1.3 is the second one.**
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

**1.4 The PIXEL shader cache is derivable from the player's own disc. THE VERTEX HALF IS
NOT — the claim below is RETRACTED, and D.1 replaced the measurement.**

~~`tools/vo_microcode_probe.py` over the 1,571 shader objects in the three prologue banks,
against the 449-blob ucode oracle:~~

```
PS: 335 of 335 verbatim (100.0%)
VS:  81 of 103 verbatim + 16 tail-matched (head patched at load), 4 partial, 2 absent
RECOVERABLE: 432 of 438 = 98.6%          <- the VS line and the total are WRONG
```

**What is actually true, measured by FULL containment rather than by a 48-byte head probe
(`tools/vo_extract_microcode.py`, D.1):**

```
PS: 343 of 345 EXACT, byte-for-byte, reproduced from the disc by a decoded container
VS:   0 of 104 exact.  95 of 104 match by their last 48 bytes and differ in 3-35
                       SCATTERED bytes throughout
```

**Zero.** The old test's positive was too weak to mean what it was read to mean: a 48-byte
head is a *shared vertex-shader prologue*, and two different runtime shaders match the same
disc object at the same offset for exactly 48 bytes before diverging. Aligned by their tails,
the differing bytes come in groups of three dwords with whole fields zeroed on disc —

```
dw13  disc 00000A88   runtime 00393A88
dw14  disc 00000000   runtime 00000003
dw15  disc 05F82000   runtime 03F82000
```

— which is **the title patching vertex FETCH instructions at load out of the vertex
declaration**, standard Xbox 360 practice. Those fields are exactly what decides the vertex
format XenosRecomp emits, so a vertex shader **cannot be pre-translated from the disc at
all.** §3.D is re-planned around that below.

Finding 6's retraction — that the banks contain microcode — **stands**, and is in
`xenia-capture-analysis.md` §6. **The disc holds 1,265 DISTINCT PIXEL SHADERS against the
345 accumulated over 25 parts and eleven operator sessions**, so for the pixel half "the
cache is complete" stops being a claim with a shelf life. It holds 142 distinct vertex
shaders too, but as templates rather than as usable microcode.

---

## §2 WHAT SHIPS

### 2.1 The artifacts

| platform | artifact | built on | notes |
|---|---|---|---|
| Linux x86_64 | `CaseZeroRecomp-linux-x86_64.AppImage` + `.tar.zst` | this machine | AppImage bundles SDL2 + ffmpeg; Vulkan **loader stays the host's** |
| Windows x86_64 | `CaseZeroRecomp-windows-x86_64.zip` | this machine, clang-cl cross or a Windows box | vcpkg SDL2 + ffmpeg beside the exe |
| macOS arm64 | `CaseZeroRecomp-macos-arm64.dmg` | a Mac (unavoidable) | ad-hoc signed `.app`, MoltenVK inside. **Notarisation is optional and needs the $99/yr account — see C.4** |

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

### MILESTONE A — make the tree shippable at all — **COMPLETE (part 82); see §9.1**

**A.1 `HostPaths` — anchor everything to the executable, not the CWD. DONE (part 82).**
Today the VFS resolves `game:` to `../../assets/game` and the shader cache tries three
`../..`-relative candidates, which is why every documented command begins with
`cd runtime/build`. Replace with one helper: `/proc/self/exe` (Linux), `GetModuleFileNameW`
(Windows), `_NSGetExecutablePath` (macOS), plus a `CZ_ROOT` override.
**Gate:** every existing headless recipe passes from three different working directories.

**A.2 The first-run detector and its refusal messages. DONE (part 82).**
Check package → extracted game → shader cache, in order; each missing one prints what and
where and exits non-zero. **Gate:** four runs — nothing present, package only, game only,
all present — each producing the right message. Break each check on purpose and confirm it
fires (gotcha 30).

**A.3 A release build type that is not `RelWithDebInfo`. DONE (part 82).**
The default exists for `addr2line` on the recompiled image and is right for development.
A release wants `-O2`, no `-g`, stripped, with a separate `.debug` artifact kept locally so a
player's crash report is still resolvable.
**Gate:** `--smoke`, then the crowd route, then `part80_trace_band.py` says the frame is in
the same regime as the dev build — **a build-type change is a performance change until
measured.**

**A.4 Bundle the runtime deps and prove the bundle is what loads. DONE (part 82).**
**Gate:** `ldd`/`otool -L`/Dependency Walker on the packaged binary in a clean container or
VM with no dev packages installed. A dependency that silently resolves to a system copy on
the build machine is the classic packaging defect.

### MILESTONE B — Windows — **COMPLETE (part 83); see §9.4**

**B.1 `kernel/memory.cpp`. DONE (part 83)** — and it gained an aliasing self-test that runs on BOTH platforms.
Today: a 4 GB `MAP_NORESERVE` reservation, then a `memfd_create` 512 MB region mapped
`MAP_FIXED` at **three** addresses (`0xA0000000`, `0xC0000000`, `0xE0000000`) so the guest's
three views of one physical range alias. Windows: `VirtualAlloc2` with
`MEM_RESERVE_PLACEHOLDER`, split the placeholders, then `MapViewOfFile3` a
`CreateFileMapping(INVALID_HANDLE_VALUE)` section three times with `MEM_REPLACE_PLACEHOLDER`.
Requires Windows 10 1803+. **Xenia does exactly this, so the pattern is proven.**
**Gate:** write through `0xA0000000`, read back through `0xC0000000` and `0xE0000000` — the
aliasing is the whole point and it is one unit test. Then `--smoke`, then the crowd route.

**B.2 `cpu/crash_report.cpp`. DONE (part 83)** — and three bugs were found in the reporter itself, two latent on Linux.
`sigaction` → `AddVectoredExceptionHandler`; `dladdr` → DbgHelp `SymFromAddr`. **Keep the
guest-state report identical** — its host `pc` is the field that is never stale and is what
makes a fault diagnosable at all. **Gate:** fault on purpose in a known guest function and
confirm the report names it.

**B.3 The other three files + build. DONE (part 83)** — larger than three files; see §9.4. `readlink` → A.1's helper; `stat` →
`std::filesystem`. **clang-cl with the MSVC ABI**, not MinGW, so vcpkg's SDL2/ffmpeg link.
Clang is not optional: `ppc_context.h` uses `__builtin_assume` and the CMakeLists says every
one of the 57,822 generated bodies fails under GCC.

**B.4 Gates. DONE (part 83)** — and B.4 is what found every Windows defect worth having. `--smoke`; the headless DebugJump route reaches the crowd;
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

**C.4 Signing and notarisation — AND THE PAID ACCOUNT IS OPTIONAL, WHICH THIS FILE
PREVIOUSLY GOT WRONG.** *(0.5 day unsigned, or 1 day + procurement notarised)*

~~Without notarisation Gatekeeper refuses the app and the bug report is "it doesn't open".~~
**Retracted: Gatekeeper does not refuse it, it blocks it by default with an override path.**
And the App Store is not the deciding factor — the App Store is one distribution channel with
its own requirement, and this project is not using it. The thing that costs money is
**notarisation**, which needs a *Developer ID Application* certificate, which is issued only
to members of the paid Apple Developer Program ($99/year). There is no free route to a
Developer ID and no way to notarise without one.

Three tiers, and only the third has a fee:

| tier | what it needs | what the player sees on first launch |
|---|---|---|
| **ad-hoc signed** (`codesign -s -`) | nothing, free, local | "Apple could not verify … it may contain malware." Blocked, with an override |
| **Developer ID signed, not notarised** | the paid account | the same warning — signing alone buys nothing without notarisation |
| **Developer ID signed + notarised + stapled** | the paid account | it just opens |

**Ad-hoc signing is not optional even in tier 1.** Apple Silicon refuses to execute an arm64
binary with no signature at all, so `codesign --sign -` must run on every shipped Mach-O.
That is free and local. **It must run AFTER the strip step**, because A.3's `objcopy` pass
modifies the binary and any edit invalidates a signature — on macOS the packaging order is
build → strip → sign, not build → sign → strip.

**The override the player uses, stated exactly, because "right-click and Open" is stale
advice.** macOS Sequoia removed the right-click → Open bypass. The current sequence is: launch
it, get refused, then **System Settings → Privacy & Security → "Open Anyway"**, then launch
again and confirm. It is three steps and it must be in the README verbatim (E.4), with a
screenshot if possible — a wrong instruction here is indistinguishable from a broken build.
`xattr -dr com.apple.quarantine <app>` in a terminal is the one-line equivalent and is worth
giving as well, because the quarantine flag is what triggers all of this and a build the
player compiles themselves never carries it.

**Recommendation: start unsigned-beyond-ad-hoc and treat the $99 as a later decision.** The
audience for this build has already had to find and copy an Xbox 360 content package; a
Privacy & Security toggle is not what will stop them. Notarisation is worth buying when macOS
users are numerous enough that the support load exceeds the fee, and nothing about the
unsigned route has to be undone to add it later — it is one extra step in the packaging
script plus the certificate.

**C.5 Gates.** *(0.5 day)* The same three as B.4.

### MILESTONE D — the first-run shader build — **COMPLETE (parts 82 + 84); see §9.1 and §9.7**

**D.1 IS DONE (part 82), and it changed the shape of the rest of this milestone.**
`tools/vo_extract_microcode.py` decodes the container:

```
microcodeStart  = u32@0x04 + u32@( u32@0x18 )      big-endian, magic 0x102A110{0,1}
microcodeLength = objectLength - microcodeStart    the microcode is always the TAIL
```

423 of 423 against ground truth, and the gate — every extracted blob must FNV-1a hash to a
name already in `assets/shader_spv/`, which is exact and needs no interpretation — reads:

```
1571 objects -> 1279 pixel, 143 vertex, 149 refused (all .scv, a different container)
pixel : 343 of 345 cache entries reproduced BYTE-FOR-BYTE
vertex:   0 of 104  — and this is structural, not a decode bug
the disc also holds 922 pixel shaders NO RUN HAS EVER BOUND
```

**The two halves of the cache are now two different problems, and that is the finding.**

* **PIXEL — solved, and solved COMPLETELY.** 1,265 distinct pixel shaders on the disc against
  the 345 this project accumulated over 25 parts and eleven operator sessions. A first-run
  pass can translate all of them, which retires "the cache is complete" as a claim with a
  shelf life for the pixel half. The two the cache holds and the disc does not —
  `ps_438c2af84c78a133` (36 B) and `ps_a15c6c9c2d249375` (60 B) — are enumerated in the
  gate's threshold so it cannot pass by accident.
* **VERTEX — cannot be prebuilt at all.** §1.4's retraction: the title patches the fetch
  instructions at load from the vertex declaration, and those fields are what decide the
  vertex format XenosRecomp emits. **D.4 is therefore not a safety net, it is the PRIMARY
  path for every vertex shader**, and D.2 is a hard prerequisite for a shipped build rather
  than a nicety.

**~~D.2 In-process translation.~~ DONE (part 84, §9.7).** *(was: 2 days, REQUIRED)*
Link XenosRecomp (MIT, sibling checkout, already patched) and embed DXC. **Gate:** build the
cache the new way and diff the SPIR-V against the 449 modules already on disk — byte-identical
is a stronger check than any picture test, and a disagreement names the shader.
**Do this before D.3.** Without it there is no vertex shader at all, and the pixel prebuild is
a nice-to-have on top of a runtime that cannot draw.

**~~D.3 The first-run pass~~ DONE (part 84, §9.7) — except the graphical progress UI, which is console lines until milestone E gives it a window.** *(was: 1 day)* Both stages of §2.3, parallel across
cores, resumable, keyed so a partial run is detected and finished rather than restarted. It
now has an honest job description: **translate the 1,265 disc pixel shaders**, and pre-warm
the pipeline cache. Vertex shaders are not part of it.

**~~D.4 The runtime first-sight path.~~ DONE (part 84, §9.7).** *(was: 1 day)* On `[imload]` of a hash the cache does not
hold, translate in-process and add it. **This is where every vertex shader comes from**, plus
the two pixel shaders absent from the disc and anything the container scan mis-parses.
**Gate:** delete a shader from the cache, confirm it is rebuilt and the picture is unchanged.
The stronger gate is free and should be the standing one: start with an EMPTY vertex half and
confirm the run reaches the crowd with `no translated shader` = 0.

**~~D.1 Decode the `.vo`/`.po` container.~~ DONE** — `tools/vo_extract_microcode.py`, and the
route to it is in that file's docstring because it is the transferable part: the start offset
is not in the fixed header, it is one indirection away, and scanning for a dword with the
right value found a *spurious* perfect discriminator (the blob length — small blobs happen to
have no constant block) where dumping the structure found the real field.

**What D retires:** "the cache is complete" as a claim with a shelf life, for pixel shaders;
the six variant caches drifting apart; and the name-diff gate that exists only because of that
drift — a defect that once cost three parts when the play cache was ten modules short.

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
| SDL2 | zlib | attribution — and **it must be REAL SDL2, not Fedora's `sdl2-compat`** (`tools/build_sdl2.sh`); see §9.1 A.4 |
| **ffmpeg (libavcodec/libavutil)** | LGPL, **or GPL if `--enable-gpl`** | **SETTLED (part 82).** `tools/build_ffmpeg_lgpl.sh` builds LGPL, xma1+xma2 only, checked against configure's own `config.h` rather than the flags passed. Dynamically linked, notice and recipe in the generated `THIRD_PARTY.md` |
| MoltenVK | Apache 2.0 | attribution (macOS) |
| o1heap, simde | MIT | attribution |
| DXC (dlopen'd by D.2's translator) | ~~Apache 2.0 with LLVM Exceptions~~ **University of Illinois/NCSA** (corrected in part 85 — DXC forked LLVM 3.7, before the Apache relicense; upstream LICENSE.TXT verified) | ship the license text beside the library — `tools/licenses/LICENSE.DXC.txt`, copied into `lib/` by the packaging script |
| UnleashedRecomp | **GPLv3 — structural reference only, no code** | nothing to ship; keep it that way |

---

## §5 THE RELEASE CHECKLIST — every artifact, every time

1. `cz_runtime --smoke` on the packaged binary, on a clean machine with no dev packages.
   **Automated: `tools/release_gate_clean_container.sh` does 1, 2 and 3 in one podman run.**
2. `ldd` / `otool -L` / Dependency Walker: nothing resolves outside the bundle except the
   Vulkan loader and system libraries. **AND THEN RUN IT** — a `dlopen` is invisible to
   `ldd`, which is exactly how the first bundle passed this step and died on its first
   instruction (§9.1 A.4).
3. First-run flow from nothing: drop package → extract → shader build → game starts.
4. The headless crowd route reaches ≥8,000 draws; `part80_trace_band.py` puts the frame in the
   same regime as the reference build. **Cheaper first: `tools/release_text_identity.sh`.**
   If `.text` is byte-identical to the reference build, the build type cannot have changed
   the frame and the route run is a confirmation rather than the evidence.
5. `CZ_VK_VALIDATION=1`: **6 `topology-08773` and nothing else** — the standing baseline.
6. `CZ_VK_VERIFY_BIND_BATCH=1`: **0 disagreements**.
7. Shader cache: `shader_dim_census.py` clean; `rt_world_xform_census.py` exit 0;
   `vo_extract_microcode.py <objects> --gate` reproduces the pixel half from the disc.
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
| Apple notarisation blocks at the last moment | low, **and no longer a blocker** | C.4: the unsigned route ships without it, at the cost of a Privacy & Security toggle the player performs once | C.4 |

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

~~**Start with A.1 and A.2.**~~ **A AND B ARE COMPLETE and D.1 IS DONE (parts 82-83, §9).** The next
item is **D.2 — in-process translation — and D.1 promoted it from a nicety to a hard
prerequisite**: the disc supplies the pixel half of the cache completely and the vertex half
not at all, so a shipped build cannot draw a single vertex shader without it. **B is done** — there is a Windows build laptop now
(`docs/windows-build-setup.md`) and the game plays on it. **C remains blocked on a Mac.**

---

## §9 EXECUTION RECORD — what was built, and what each gate actually measured

Filed here rather than in a separate document, because a plan whose corrections live
somewhere else gets read without them.

### 9.1 Part 82 — MILESTONE A COMPLETE, and D.1

**A.1 — `runtime/host/host_paths.{h,cpp}`.** One root, decided once and printed once
(`[paths] root … (assets-walk), exe …` on the first line of every log). `$CZ_ROOT`, else a
walk of at most four levels from the executable's directory to the first one containing
`assets`, else the executable's directory. That one rule covers both layouts: the dev tree
(`runtime/build` -> repo root, two levels) and the shipped tree (exe dir, zero). **Nothing
falls back to the CWD** — a CWD fallback keeps the dev tree working while the shipped one
silently does not, which is the single failure mode that survives every test done on the
build machine. All three platform spellings written now (`readlink`, `GetModuleFileNameW`,
`_NSGetExecutablePath`) so milestone B's file list stays the five §1.1 censused.

*Gate:* every headless recipe from `runtime/build`, the repo root, `/tmp` and `$HOME` — same
root, same 449 shader modules, `no translated shader` = 0, `--smoke` OK from all four.

**A.2 — `runtime/host/first_run.{h,cpp}`.** Package -> unpacked game -> shader cache, in
order; the first one missing prints what it is, where it goes and the command that produces
it, then exits non-zero. A missing shader cache is fatal only when `CZ_VKDRAW` is set, because
every log-diff gate in this project runs with the renderer off and refusing those on a fresh
clone would take the gates offline to protect a player from a screen they will not see.
`CZ_NO_FIRST_RUN_CHECK=1` turns it off.

*Gate — six trees, and it found two defects in the first version by running the branches on
purpose (gotcha 30):*

* a 2 MB `.wav` dropped into `assets/package/` was **accepted as the game** and the player
  told to unpack it. The magic word was reported as a footnote rather than deciding, so the
  footnote was unreachable. Size and identity are two questions now.
* the printed extract command said `-o <root>/assets`, which would have produced
  `assets/default.xex`. `extract_stfs.py`'s `-o` is the directory the package's own tree goes
  into: `assets/game`.

**A.3 — the `Release` build type, and its gate answered for free.** `-O2 -g -DNDEBUG` then
`objcopy --only-keep-debug` / `--strip-debug` / `--add-gnu-debuglink`. **-O2 and not -O3**
because the whole performance corpus behind parts 47-81 was measured at -O2 and shipping -O3
would invalidate it for a saving nobody has measured; -O3 is a measurement to run, not a
default to inherit from CMake. **-g kept then split** because the plan's "no -g, stripped,
with a separate `.debug`" is two halves in tension — a binary built without `-g` has no debug
info to keep. Verified: `addr2line` on the STRIPPED binary resolves `main` to
`runtime/main.cpp:161` through the debuglink.

*Gate — `tools/release_text_identity.sh`, and this is the transferable part.* The plan proposed
the crowd route, which on this workload is three runs an arm and an hour (gotcha 229). But
`-g` does not affect code generation and `--strip-debug` does not touch `.text`, so the claim
to test is **byte identity**, not a frame time:

```
.text  35,651,455 bytes, sha256 IDENTICAL between RelWithDebInfo and Release
```

The build type therefore **cannot** have changed the frame. Decisive where an A/B is
statistical. Positive control: exit 1 on two genuinely different binaries.

It also found something not predicted: **`CZ_BUNDLE_RPATH` moves `.text`**. It is a link
option, but the RUNPATH string lives in `.dynstr`, which sits *before* `.text`, so adding one
lengthens an earlier section and relocates the image — every address-bearing byte differs
while every instruction is the same instruction. Held matched rather than carved out as an
exception, because an exception is where a real difference would hide.

**A.4 — the bundle, and the defect only RUNNING it could find.**

`tools/build_ffmpeg_lgpl.sh`. Fedora's ffmpeg is `--enable-gpl` (a conflict with this repo's
PolyForm licence the moment they ship together) and its closure is **120 shared objects** —
x264, x265, SvtAv1Enc, librsvg, cairo, pango, OpenCL, VA-API — against the **fourteen** ffmpeg
functions this runtime calls, censused rather than assumed. Purpose-built: libavcodec +
libavutil, xma1+xma2 only, 1.4 MB, linking libm and libc and nothing else. **120 -> 3.**
Version pinned to 8.1.2, the one this machine develops against, so the headers are not a
second variable. The licence check reads configure's own `config.h`, not the flags we passed.
Verified decoding: 143,616 float samples, ch=2, 48 kHz, `fltp`, rms 0.0218.

`tools/build_sdl2.sh`, **and this is the finding worth keeping.** The first bundle passed its
`ldd` check completely — every library resolved inside the bundle — and then died on its first
instruction in a clean container with **`Failed loading SDL3 library.`** Fedora's
`libSDL2-2.0.so.0` is `sdl2-compat`, a shim that `dlopen()`s `libSDL3.so.0`. **A `dlopen` is
invisible to `ldd`, the very tool this plan's A.4 specifies for the job**, and it worked on
the build machine because SDL3 was installed there. Bundling SDL3 as well would not fix it
either: the shim dlopens by soname and has no RUNPATH. So the script builds real SDL2
(2.32.10, zlib), which links libm and libc and dlopens X11/Wayland the way every shipped Linux
game does, and asserts the result mentions `libSDL3` by **no** route.

`tools/release_package_linux.sh` assembles §2.2's layout. Libraries come from the binary's own
`ldd`, not a hand-written list that drifts. `$ORIGIN/lib` RPATH rather than a launcher script,
because a launcher is a thing a player can bypass and then the bundle silently is not what
loads. `THIRD_PARTY.md` generated from what is actually linked. **16 MB compressed.**

`tools/release_gate_clean_container.sh` is A.4's gate: podman + `fedora-minimal`, bundle
read-only, the Vulkan loader installed because that is the one library a player's driver
supplies and its absence would force an exception exactly where a real missing library could
hide. **PASSED**: every bundled dependency resolves inside the bundle *including libstdc++
with a system copy present* — which makes it a real RPATH-precedence test — `--smoke` passes
in the packaged binary, and the first-run refusal prints correctly from a container with no
game.

**The gate's own first run printed `GATE PASSED` having executed nothing**: `podman` without
`-i` does not attach stdin, so `sh -s` read EOF and exited 0. Gotcha 483's shape reproduced in
a tool written the same afternoon that gotcha was read. It now requires four marker lines and
the smoke sentence as evidence its body ran, not just an exit code.

### 9.2 What part 82 leaves OWED, stated rather than smoothed over

1. **The ffmpeg and SDL2 swaps are real `.text` changes and their cost is unmeasured.**
   Confined to the audio decode path and the window/present seam, neither of which is the
   render hot path — but that is a reason to expect a null, not a measurement of one.
2. **The shipped ffmpeg has no hand-written x86 assembly.** This machine has no `nasm` and no
   sudo. `tools/build_ffmpeg_lgpl.sh` says so loudly and falls back rather than shipping a
   slower decoder quietly. `sudo dnf install nasm` and re-run before cutting a real artifact.
3. **The artifact inherits this machine's glibc floor** (2.43) and will refuse to start on
   anything older. The packaging script prints this. The real fixes are an old build base or
   an AppImage runtime, and both are E.2.
4. **No AppImage yet** — §2.1 names one and part 82 shipped the `.tar.zst` half only.
5. **`tools/extract_stfs.py` is shipped but `build_shader_spv.sh` is not**, because it needs
   XenosRecomp and DXC. That is exactly what D.2 moves in-process; until it lands, a shipped
   build needs a shader cache supplied alongside it.

### 9.3 Part 83 — the Windows build box exists, and milestone B is scoped BY MEASUREMENT

**The environment (`docs/windows-build-setup.md` is the verified runbook).** A Windows 11
laptop reachable as `czwin` over SSH with a dedicated passphrase-less key, tested with
`env -u SSH_AUTH_SOCK` so it is durable rather than agent-dependent. i7-12700H (20 threads),
15.7 GB, **RTX 3070 Ti** — the discrete GPU is what makes B.4's renderer gates runnable
there rather than `--smoke` only.

Everything builds: MSVC 19.44 / clang 22.1.8 / CMake 4.4.3 / Ninja 1.13.2, Vulkan SDK
1.4.350.0, SDL2 and ffmpeg from the **same source tarballs as Linux** so the two platforms
differ in toolchain and nothing else. **XenonRecomp compiles**, and **`ppc/` regenerates in
8 seconds: 228 TUs, 152.7 MB, zero errors.** The recompilation is fully platform-independent
— which was the single largest unknown in milestone B and is now answered.

**Two CMakeLists changes shipped** (`b885ad8`, `3c387c9`), both gated on Linux with `.text`
still byte-identical:

* `CZ_FFMPEG_PREFIX` now finds ffmpeg with `find_path`/`find_library` instead of requiring
  pkg-config, which Windows has not got. It also sidesteps a problem that was next in line:
  an MSYS2-built `.pc` names `/c/cz/…`, a path a Windows CMake cannot open.
* XenonRecomp's static libraries are named via `CMAKE_STATIC_LIBRARY_{PREFIX,SUFFIX}`
  rather than hardcoded `.a`.

**B, MEASURED.** `cmake --build … -- -k 0` — keep-going, so one run enumerates everything —
gives **4 missing headers and 64 errors, 18 distinct, across 9 files.** Smaller than the plan
assumed, and differently shaped:

| item | what the compiler actually said |
|---|---|
| **B.1** `kernel/memory.cpp` | `sys/mman.h` not found. As planned: the `VirtualAlloc2`/`MapViewOfFile3` triple alias. Still the only fiddly item |
| **B.2** `cpu/crash_report.cpp` | `dlfcn.h` not found → vectored exception handler + DbgHelp |
| **B.3** | **bigger than "the other three files + build"**, and this is the new information |
| | `unistd.h` in `cpu/guest_thread.cpp` and `gpu/vk_renderer.cpp` |
| | **`<windows.h>` macro pollution — the largest single class.** Our guest `ERROR_ALREADY_EXISTS`, `ERROR_IO_PENDING`, `ERROR_NO_SUCH_USER`, `ERROR_INVALID_PARAMETER` … collide with `winerror.h` macros across `kernel/imports.cpp`, `kernel/content.cpp`, `kernel/file_imports.cpp` and `gpu/vd.cpp`. A macro beats a `constexpr`, and the diagnostics land as `expected unqualified-id` rather than anything naming the cause |
| | `HRESULT` redefined / ambiguous — we declare our own |
| | `CLOCK_MONOTONIC`; `fseeko`/`ftello` → `_fseeki64`/`_ftelli64` |
| | a `_m_prefetch` builtin redefinition **inside SDL2's own `SDL_endian.h`** under clang-cl |
| | CMake: `-include timebase.h` is the GNU-driver spelling and clang-cl reads the path as a second SOURCE FILE — `cannot specify '/Fo…' when compiling multiple source files`. **This alone stopped all 228 ppc TUs**, and the message names the output rather than the flag (gotcha 494). `/FI` is the fix |

**What is NOT a problem, checked rather than assumed:** the recompiled image itself, the
switch tables, the Vulkan/SDL/ffmpeg discovery, and `-msse4.1 -mavx`, which clang-cl accepts
unchanged.

**Lessons:** gotchas **490-494**.

### 9.4 Part 83 — MILESTONE B IS COMPLETE, THE GAME PLAYS ON WINDOWS, AND THE STUTTER IS FIXED

**The headline: `cz_runtime.exe` builds, links, boots, renders and PLAYS.** The operator
played Still Creek on the Windows laptop at 2560x1440 with sound. Milestone B's three code
items are done and every one of them was gated by running the thing rather than by
compiling it.

**B.1 — `kernel/memory.cpp`.** `VirtualAlloc2` + `MapViewOfFile3` placeholder mapping
replaces the `memfd_create` triple alias. The three views turn out to be contiguous and to
end exactly at 4 GB, which is now three `static_assert`s. Windows commits the low 2.5 GB up
front because it has no `MAP_NORESERVE` for a writable region — stated, not discovered.
**And the aliasing is now SELF-TESTED on both platforms**: `CheckPhysicalAliasing` writes a
distinct magic through each view and reads it back through all three at startup, with
`CZ_MEM_POISON_ALIAS=1` as its positive control. Nothing had ever checked that property,
and a broken alias fails hours later inside an allocator.

**B.2 — `cpu/crash_report.cpp`.** Report() is platform-neutral now; each OS supplies
`(sig, faultAddr, hostPc)` and the ~200 lines of guest-state reporting are identical.
Windows uses `SetUnhandledExceptionFilter` and DbgHelp. **Three bugs were found in the
reporter itself, two of which are latent on Linux too:**

* it was an `AddVectoredExceptionHandler`, which sees FIRST-CHANCE exceptions — it caught a
  benign one and killed a healthy process;
* it could **smash its own stack**: 21 sites of `n += snprintf(b + n, sizeof b - n, ...)`,
  where a report longer than the buffer underflows `sizeof b - n` to ~2^64. `/GS` catches
  that and `__fastfail`s, which bypasses SEH entirely — so the process vanished at
  0xC0000409 with no output at all. **A crash reporter that can crash while reporting
  replaces a diagnosable fault with an undiagnosable one**, and it did exactly that twice;
* the `host pc` line was guarded on `hostPc != 0`, so it printed nothing for a null
  indirect call — the case it exists for.

**B.3 — the build.** `host/win_compat.h`, force-included into the runtime's C++ sources
only, carries the `windows.h` collisions (`ERROR_*`, `E_FAIL` via `_HRESULT_TYPEDEF_`, and
`far`, which is still `#define`d from the 16-bit memory model and turned a local variable
into a syntax error), plus `fseeko`/`ftello` and a `clock_gettime` over
QueryPerformanceCounter. Guest constants that collide with Win32 macros of the same value
were RENAMED (`kGuestMemReserve`, `kGuestGenericWrite`) rather than the host's spelling
undefined, wherever anything on our side wants the Win32 meaning. Links `onecore`
(VirtualAlloc2), `dbghelp` (SymFromAddr) and **clang-rt builtins** — clang-cl does not link
its own builtins on Windows, so the recompiled image's `__int128` division came out as an
undefined `__udivti3`. XenonRecomp was patched to stop forcing `/MT`.

**THE ONE THAT COST ALL 228 TUs:** `-include` is the GNU driver's spelling and clang-cl
reads the path as a second SOURCE FILE, failing with "cannot specify '/Fo…' when compiling
multiple source files" — an error naming the output rather than the flag. `/FI` is the fix.

**Windows platform defects the operator found by PLAYING:**

| symptom | cause |
|---|---|
| VFS mounted on the CWD; guest faulted ~300 ms later | `find_last_of('/')` — no forward slash exists in a Windows path |
| resolution list capped at 1600x900 on a 1440p screen | no DPI awareness, so every display query returns the desktop divided by the 160% scale |
| thread budget sized off a guess | `CountPhysicalCores` reads /sys; 10 assumed against a real 14 on a 6P+8E part |
| the warm pipeline cache could silently vanish | its directory came from `HOME`, which Git/MSYS2 set and nothing else does |

### 9.5 THE STUTTER — found, fixed, and it was never a Windows bug

**The diagnosis.** `CZ_VK_FRAME_TRACE` plus the operator's F7 marks:

```
frame 6696   396 ms wall = 372 ms in GetPipeline + 24 ms for everything else
```

Record, textures, constants, streams and GPU all normal. Four of five F7 marks land within
45 frames of one of these and nowhere else. **Pipelines were being created lazily, on the
frame that first needed each one, on the frame thread** — 534 in three minutes, at 1-200 ms
each.

**Not a platform defect.** Linux creates 122 at 0.11 ms each because its cache is 29 MB
built over eighty sessions. Windows creates 534 at 1-200 ms because its cache is two
sessions old. **Every new player gets the Windows experience on every platform**; the Linux
machine only looked smooth because of a file no player will have.

**The fix: record the keys, replay them at load.** `PipelineKey` is a 56-byte padding-free
POD already carrying `vsHash`/`psHash`, so the key alone identifies a pipeline. Every key
seen is written beside the `VkPipelineCache` blob and rebuilt at the next start, before the
guest draws. `CZ_VK_NO_PREWARM=1` is the control arm.

**Result, across three operator sessions:**

| | worst pipeline spike | spikes > 20 ms |
|---|---|---|
| before | **372 ms** | 6 |
| pre-warm reading only 72 of 527 keys | 249 ms | 7 |
| **pre-warm building 527 of 527** | **173 ms** | **2** in 10,429 frames |

**Three bugs inside the fix, each found by the operator still stuttering:** keys were saved
only at exit (a crash threw the session away); the temp-and-rename failed on Windows every
time; and the pre-warm **truncated the key file it was reading**, stopping at exactly 72
every time — see gotchas 497 and 498.

**What remains is the tail and it converges.** A first-time compile costs 120-170 ms on
that driver and the pre-warm can only build what a previous session saw. 550 new pipelines
were created in the last session by reaching new content. For a release, shipping a key
file from a full playthrough would give players a warm start on day one.

### 9.6 WHAT PART 83 GOT WRONG, because the corrections are the reusable part

* **A laptop power profile is a measurement variable.** Every Windows number was taken in
  "quiet" mode. High performance is **+34% frame rate, -30% record cost** at matched draws —
  most of an unexplained "Windows is 1.8x slower" that had already cost a Vulkan-layer
  census and a CPU-affinity A/B. Gotcha 496.
* **`CZ_VK_FRAME_STATS` costs 8.8-15.5 ms a frame at 1440p**, up to 59% of the window. A
  cross-platform comparison was run and reported with it armed. The runtime printed its own
  bill in the same log. Gotcha 499.
* **`CZ_VK_FRAME_TRACE` was gated behind `CZ_FPS_LOG`** and silently recorded nothing —
  costing an operator play session, with a "0 rows" check run and read past. Gotcha 495.
* **The trace printed wrapped negatives** after every profiler window, which sorted to the
  TOP of every worst-frame list.
* **Four Windows A/Bs were invalidated by the harness**, not the subject: a
  `Stop-Process -Force` that skipped the save handler, a cache directory that moved, a
  rename that never worked, and a `Select -First 1` that read the wrong line. Twice a check
  was reported as failing when the CHECKING SCRIPT was wrong.
* **Refuted honestly:** seven implicit Vulkan layers (Steam, GOG, Epic, OBS, RivaTuner) are
  a **null** — 1037 vs 1033 ns/draw. P-core affinity is worth ~7.6% at matched draw counts,
  not the -27% an unmatched comparison suggested.

### 9.7 Part 84 — MILESTONE D COMPLETE: the runtime translates its own shaders

**The whole of milestone D landed in one part** — D.2, then D.4, then D.3, in the order the
part-84 kickoff prescribed — and every gate below was run before its commit. A shipped build
no longer needs Python, a shell, the XenosRecomp executable or the dxc CLI to draw: it builds
the pixel half of its cache from the player's own disc in nine seconds, and every vertex
shader (plus the two pixel shaders the disc lacks) arrives through first-sight translation at
run time.

**D.2 — in-process translation (`gpu/shader_translator.{h,cpp}`).** XenosRecomp's own
`shader_recompiler.cpp` (MIT, the sibling checkout, compiled into `cz_runtime` against
`gpu/xenos_pch.h` — upstream's pch minus smol-v/zstd/xxhash/dxcapi), C++ ports of
`synth_shader_container.py`'s ucode analysis + container synthesis and
`alu_const_census.py`'s HLSL census, a JSON writer replicating Python `json.dump(indent=1)`
byte for byte, and DXC through its C API (dlopen'd: `CZ_DXC_LIB`, then `<exe>/lib`, then the
sibling checkout). `shader_common.h` is EMBEDDED at configure time so the HLSL prologue
cannot drift from what built the existing caches.

* **The design was probed before it was built**: `IDxcCompiler3` with the CLI's argument
  list produces SPIR-V **byte-identical** to the `dxc` executable, source name or not.
* **The gate, as the plan wrote it**: `cz_runtime --translate-shaders` over the 449 dumps —
  **449 of 449, all 898 files byte-identical** to the Python/shell pipeline's output, in
  **2.6 s wall against the shell's 51 s**. Positive control: a deliberate census defect
  (drop one aluConst) fires the gate in the sidecar diff. A single blind-flipped ucode bit
  was **semantically dead** and moved nothing — the implementation poison is the control
  that counts (gotcha 501).
* **Cross-platform**: the same translation on the Windows laptop (dxcompiler.dll) is
  byte-identical to Linux (libdxcompiler.so) — 348 of 348 dump-built modules, then
  **1,265 of 1,265 disc-built modules**. **A shader cache is fully portable between
  platforms**, so D.3's first-run output is one artifact, not one per OS.

**D.4 — translate on first sight.** `BindShader` calls `VkRenderer_OnShaderBind` once per
distinct hash (inside its announce-once block); a miss enqueues the bytes to ONE worker;
the draw-path miss branch drains finished modules — enqueue and drain are both on the pump
thread, so the shader tables are never touched off-thread. Translations persist into the
cache directory, so the next run starts warm. The in-flight skip is its own counter
(`draw: shader translating`); **`no translated shader` keeps meaning "ended up missing"**.
`CZ_VK_NO_SHADER_JIT=1` is the same-binary arm.

* **The plan's stronger standing gate, run first try**: EMPTY vertex half, DebugJump crowd
  route — crowd at **8,110 draws**, `no translated shader` **= 0**, **45** vertex shaders
  translated at first sight (**18-70 ms each, median 23**), 0 failures, 18,125 draws
  transiently skipped and honestly counted. Every persisted module and sidecar is
  byte-identical to the canonical cache entry for the same hash.
* **Controls**: off-arm (same cache, JIT off) restores the old behaviour — 29
  `no translated shader`, 0 first-sight — so the arm engages and the gate can fail. Null
  (full cache, JIT on): 0 first-sight, 0 skips — the standard path never sees the feature.
  `CZ_VK_VALIDATION` over the JIT path: only the standing topology-08773 class.

**D.3 — the first-run disc pass (`gpu/shader_prebuild.{h,cpp}`).** The `.big` index (LE,
stride from `names_offset`) and D.1's `.po` container rule, ported with every bound checked
so a malformed player-supplied object is skipped BY NAME. Dedupe by FNV-1a, skip what the
cache holds (which IS the resume mechanism), translate the rest on a whole-machine pool.
`cz_runtime --build-shader-cache` by hand; a renderer boot runs it automatically when the
cache is missing, empty, or carries a `disc_prebuild.started` marker without its `.done` —
and NEVER against a populated developer cache, which has neither marker.

* **1,265 of 1,265 distinct pixel shaders, 0 refused, 0 failed, 9.0 s wall** (§2.3 guessed
  ~30 s). All 343 canonical overlaps byte-identical, sidecars included.
* **Resume**: killed at 433 → re-run translates the remaining 832 and only then writes the
  done marker; a third run is a no-op.
* **The crown gate — the shipped first-run story end to end**: disc-built cache only, no
  vertex half → 1,265 modules loaded, crowd at **8,154 draws**, `no translated shader` = 0,
  47 first-sight translations — **and the pixel first-sight list is EXACTLY the two hashes
  D.1 enumerated as absent from the disc** (`ps_438c2af84c78a133`, `ps_a15c6c9c2d249375`).
* **The simulated player tree** (CZ_ROOT at a root with the game and no cache): the boot
  hook fired, built all 1,265 before the guest started, and the renderer loaded them. A
  dev-tree boot shows 0 prebuild lines and the canonical cache stays at 898 files.

**Windows.** All of it builds with clang-cl and runs: `--smoke` OK, the disc prebuild
produces the byte-identical 1,265. Three portability defects found and fixed on the way:
`WIN32_LEAN_AND_MEAN` excludes the COM types dxcapi.h needs (`IUnknown`/`IStream` pulled in
by name); `win_compat.h`'s `#undef far` leaves `FAR` expanding to a stray identifier in
every COM prototype (`expected ')'` across combaseapi.h — re-pointed at nothing for the one
TU); and `E_FAIL` is `#undef`'d for guest code, so it is spelled by value. The runbook
gained the XenosRecomp tree: `C:\cz\XenosRecomp` (a 19 MB tarball subset — sources,
dxc headers, the two dxcompiler libraries), `-DXENOS_ROOT=C:/cz/XenosRecomp` at configure,
`dxcompiler.dll` beside the exe.

**A false claim, corrected in-session (gotcha 502):** the first two "Windows builds D.4"
statements were made against a STALE tree — a chained `git pull >nul 2>&1 &&` had failed
silently, the build had nothing to do, and the empty error grep read as success while
`--smoke` exercised the previous binary. The tell was `--build-shader-cache` falling
through to the first-run refusal: the flag did not exist in the binary that ran. Verify the
pulled HEAD, not the absence of error text.

**What part 84 leaves owed:**
* the graphical "Preparing shaders" progress screen (§2.3 step 3-4) — progress is console
  lines until milestone E gives the first run a window;
* the in-process STFS extract (§2.3 step 2) — `extract_stfs.py` is still the documented
  step, and `first_run` still refuses with the command rather than running it;
* the shipped pre-warm pipeline-key file (part 84 kickoff item 3) — waits on a full
  playthrough's key set and on E having an artifact to ship it in;
* the JIT persists into the STOCK cache only; the six variant arm caches do not gain
  first-sight entries and will read `no translated shader` for a shader born at run time —
  acceptable for dev arms, worth one line here so it is never diagnosed as a defect.

### 9.8 Part 85 — MILESTONE E: the release is packaged, gated, and CI exists

**E is the milestone where every earlier "owed" line either shipped or is named below.**
Both §2.1 primary artifacts now exist and are gated: `CaseZeroRecomp-linux-x86_64.tar.zst`
(26 MB) and `CaseZeroRecomp-windows-x86_64.zip` (21 MB). The §5 checklist ran end to end
for this state: clean-container gate PASSED (items 1–3), `.text` identity between matched
RelWithDebInfo/Release configures (item 4's cheap form, 35,915,250 bytes, same sha256),
validation exactly the standing **6 `topology-08773` and nothing else** at 7,676 draws
(item 5), bind-batch verify **0 of 87,357,139 triples** (item 6), both shader censuses and
the disc gate 343-of-345 (item 7), and the bisection knobs are in the shipped README
(item 8).

**The in-process STFS extract (§2.3 step 2) — SHIPPED.** `host/stfs_extract.{h,cpp}`,
byte-identical to the Python reference over the real package (256 of 256 files,
859,007,897 bytes), bounds-checked and traversal-checked because the package is
player-supplied input, `default.xex` written LAST so an interrupted extraction cannot
counterfeit a complete tree. Negative controls: a truncated package fails naming the
entry and offset; the presence-check file is never written. `--extract-package` is the
gate verb; `CZ_NO_STFS_EXTRACT=1` restores the refusal.

**The whole §2.3 first-run flow now actually happens, and was run three ways.** A fake
root holding ONLY the package, booted with the renderer: auto-extract → disc prebuild
(1265 translated, 0 failed, marker written) → live gameplay with 32 first-sight vertex
JITs and `no translated shader` = 0. The same flow inside the clean container (no dev
packages, no GPU — the renderer leg is the host run's) via the gate's new fourth section.
And windowed, where the new **first-run progress window** (Host_Progress*, plain SDL,
the shared glyphs, one bar) was screenshotted mid-flow showing `PREPARING SHADERS - 996
OF 1265`. The §9.2/§9.7 "graphical progress screen" debt is paid.

**Two packaging holes only RUNNING things could catch, both closed before any player:**

* `libdxcompiler.so` was **not in the bundle** — every static check passed while a
  shipped build could not have translated a single shader (the sdl2-compat shape again:
  a dlopen is invisible to ldd). The bundle carries it now, and the container gate RUNS
  a translation inside the container with `HOME` unset so the dev-checkout fallback
  cannot rescue a broken bundle. Windows: the staged exe built all 1,265 disc shaders
  and its `[shxlate]` line names the STAGED dll, not the dev tree's.
* A player double-clicking `cz_runtime` got the **deliberately blank window** — CZ_VKDRAW
  is opt-in everywhere in this repo. `cz_defaults.env` beside the executable now carries
  release defaults, applied only for variables the environment leaves unset (data, not
  code — `.text` identity re-verified; the environment always wins; dev trees have no
  such file). Verified in both directions.

**The DXC licence row in §4 was WRONG and is corrected in place**: DXC is University of
Illinois/NCSA (an LLVM 3.7 fork, pre-relicense), not Apache-2.0-with-LLVM-exceptions;
upstream LICENSE.TXT verified, vendored, and shipped beside the library on both
platforms.

**E.1 CI exists and its green tick says exactly what it means** (the workflow's own
comment): host sources compile and `--smoke` passes against a STUB image
(`tools/gen_stub_ppc.py`) on ubuntu and windows — never "the release builds". CI clones
upstream hedge-dev recompilers at pinned merge-bases and applies `tools/ci/*.patch`
(13 + 23 local commits change the interfaces the runtime compiles against; an unpatched
clone does not build it — `tools/ci/regen_patches.sh` is the maintenance verb). The
Linux leg was verified end to end LOCALLY before the YAML existed and its first real run
is green; the stub generator paid for two scanner lessons (no word boundary inside
`__imp__sub_`; X-macro lists carry addresses as bare hex). macOS is absent on purpose
until milestone C.

**E.2's Windows script** (`tools/release_package_windows.ps1`): DLLs beside the exe
because that is the search path Windows has, MSVC runtime from the toolchain's redist
(never System32), the gate RUNS the staged exe. **E.3** THIRD_PARTY.md generated on both
platforms, now with DXC and the MSVC runtime. **E.4** `tools/release/README.md` ships in
both bundles — drop-in steps, first-run, §5-item-8 bisection knobs; the keyboard map is
deliberately NOT enumerated (the runtime prints its own; window.cpp predicted a README
map would rot).

**Still owed after E** (the §9.2-style honest list):
* the **glibc floor** on the Linux artifact (build on an old base or AppImage) — the
  packaging script still prints it as a known limitation;
* ~~the **pre-warm key file**~~ — **SHIPPED, same part.** The operator's sitting on the
  shipped Windows bundle (which itself verified the first-run flow in the wild:
  in-process extract, disc prebuild, 64 vertex shaders born at first sight, a save
  written — and their verdict, "felt good, pretty close to the Linux build") doubled as
  the harvest: the part-83 recorder saves keys periodically, so 583 keys (32,660 bytes)
  came out of `%LOCALAPPDATA%\cz-recomp` when they quit. The renderer now seeds from
  `<exe>/prewarm.keys` ONLY when no per-user key file exists — session one, exactly the
  session part 83 could not reach — and the per-user file takes over from session two.
  Verified both ways and cross-platform BY USE: a cold-cache Linux boot created 421 of
  583 pipelines up front (19.2 s at 45.71 ms each moved out of gameplay) from keys the
  WINDOWS build wrote; a warm cache reads it zero times. Both packaging scripts
  header-check the seed and the container gate requires it. The 162 skips are keys
  whose vertex shader had not yet been born at first sight — coverage completes by
  launch two, and the log counts it. **Refreshed the same night from a second operator
  sitting: 860 keys (48,172 bytes), cold-boot seed 550 of 860 in 24.1 s, both artifacts
  repackaged and re-gated.** **New owed polish, stated rather than smoothed:
  that one-time ~20 s runs silently after the progress window closes on a true cold
  first boot; §2.3 item 4 wanted it under a visible screen.**
  **And then the operator COMPLETED THE WHOLE GAME ON THE SHIPPED BUNDLE** — the
  strongest verdict a release artifact can receive. The completion sitting grew the
  seed to its final form, **1,365 keys** (cold boot: 757 up front, 24.0 s at 31.8 ms
  each), bore first-sight vertex shaders up to **102 of the ~104 this port has ever
  seen**, and produced **ZERO new pixel shaders** beyond the disc's 1,265 + the two
  known absentees — §9.7's completeness claim, demonstrated by a full playthrough in
  the wild. ~~One open question: either the operator never saved or bundle saving is
  broken~~ — **ANSWERED BY THE OPERATOR, and it is the worse one: they made THREE saves
  and none exists anywhere on the disk. BUNDLE SAVING IS BROKEN ON WINDOWS and it is
  part 86's TOP ITEM as a release blocker**, together with a second report from the
  same run (level-up 1→5 granted no health increase). Forensic shape, repro recipe and
  the coupling hypothesis: `part86-kickoff.md` §0b. **And one tooling defect the sitting exposed: a repackage
  wiped the played stage** — an out-of-band move failed silently in ssh quoting and the
  script's unconditional wipe did the rest; both packaging scripts now preserve player
  assets THEMSELVES and were gated on it (a probe file survived a repackage and the
  shipped archive carries zero player files);
* ~~the **Windows CI leg's first live run**~~ — **RESOLVED in the same part, three
  iterations after close, and BOTH LEGS ARE GREEN** (`--smoke`: 208 stub entries, every
  symbol resolved). The predicted "failure will be in the dependency step" was wrong both
  times, honestly: vcpkg worked, and the two real failures were (1) an lld-link
  `/failifmismatch` on RuntimeLibrary — CI's XenonUtils defaulted to the upstream `/MT`
  while the runtime is `/MD`; the vendored patch exists to make it overridable and CI now
  passes `MultiThreadedDLL` like czwin does — and (2) a one-second, zero-output smoke
  death: the exe links vcpkg import libraries but nothing deploys the DLLs without the
  vcpkg toolchain file, so the process never loaded; they are now copied beside the exe,
  the release-bundle rule. Diagnosed through a new `windows-logs` artifact (tee'd
  configure/build/smoke), because the public API serves step verdicts but not step logs —
  that artifact is now standing equipment. `ACTIONS_RESULTS_URL` is exported too (cache
  service v2), which took vcpkg from ~80 to 28 minutes;
* **macOS entirely** (milestone C, blocked on hardware).
