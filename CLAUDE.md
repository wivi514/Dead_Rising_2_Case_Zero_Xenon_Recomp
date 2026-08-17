# Dead Rising 2: Case Zero Xenon Recomp — project guide

Static recompilation of the Xbox 360 XBLA title **Dead Rising 2: Case Zero**
(Capcom / Blue Castle Games, 2010) using **XenonRecomp** + **XenosRecomp** (hedge-dev's
faithful recompiler pair, the ones UnleashedRecomp uses).

This is the **third** game ported with this pipeline in this workspace. The two before it
are the playbook, and most infrastructure and every hard-won gotcha transfers — **read
them before re-deriving anything**:

- `~/GithubRepo/Fable2XenonRecomp` — the original and the deepest (91k functions → a live
  rendered world). Its `CLAUDE.md` is the project journal; its `docs/` hold the reusable
  methodology.
- `~/GithubRepo/Asuras_Wrath_Xenon_Recomp` — the second port, which proved the template
  transfers and consolidated the gotchas into a numbered list.

**Dead Rising 2: Case West is planned next** (`~/GithubRepo/Dead_Rising_2_Case_West_Xenon_Recomp`,
package present, not started). It is the same engine and the same studio, so essentially
everything learned here should carry over — worth keeping that in mind when deciding
whether a finding belongs in a Case Zero doc or in a general one.

## Template status: what is inherited vs. what is new here

Inherited wholesale:
- Repo shape, gitignore policy (game data + generated `ppc/` untracked), tools, docs style.
- Runtime architecture: kernel HLE with *honest-failure* stubs, o1heap guest arenas,
  guest-thread bootstrap, PM4 command processor, Vulkan renderer on XenosRecomp SPIR-V,
  XMA via ffmpeg, SDL window/input. UnleashedRecomp is GPLv3 → **structural reference
  only**; guest structs from XenonRecomp's `XenonUtils/xbox.h` (MIT).
- Ground-truth discipline: Xenia text logs, `.xtr` GPU stream traces,
  `--trace_function_data` coverage, and A/B methodology with same-binary arms.

**New here, because this is the first XBLA (not disc) title in the workspace** — this is
the part a future Case West port will reuse verbatim:
- `tools/extract_stfs.py` — STFS/SVOD container reader. There is no ISO and no
  `extract-xiso` step; the game is a single hash-interleaved block filesystem.
- `tools/xex_image_dump.cpp` + `build_xex_image_dump.sh` — offline image via XenonRecomp's
  own loader, because this XEX is LZX-compressed and `decrypt_xex.py` cannot read it.
- `tools/find_save_restore.py` — structural ladder scan, replacing the hand-encoded
  byte-pattern greps the earlier ports used.
- A XenonRecomp patch for the **devkit AES key** (`docs/xenonrecomp-upstream-bugs.md`).

## Transferable gotchas

**THE FULL NUMBERED LEDGER IS `docs/gotchas.md` — 338 entries, and every "gotcha N"
reference in this repo and in the docs resolves there.** It was split out of this file
on 2026-08-08, when this file reached 308 KB and was being loaded into every session
whole. Read it **before making a measurement claim, adding an instrument, believing a
zero, or trusting a number an earlier session wrote down**; those four situations
produced almost all of it.

The ten that bite most often, as one-liners. Each is a summary, not the entry:

3.  **A zero is a detection failure, not a fact.** XenonAnalyse found zero jump tables
    here; our scanner found 234. Applies to every number a detector prints.
5.  **Kernel stubs must fail honestly, never fake success** — and a stub that returns an
    error but leaves its OUT-PARAMETER untouched is worse than no stub. See also 59 and
    201: when a return value is a predicate or a computed value, "fail honestly" has no
    spelling and implementing it is the only correct option.
7.  **A probe expensive enough to stall the game manufactures the stability it reports.**
    Every instrument needs its own control — and see 151, an arm with no counter cannot
    be shown to have engaged, and 223, an instrument on a hot path can cancel the effect
    it is measuring exactly.
13. **A capture request, a plan, and your own status note all have a shelf life.**
    Re-read them against the current ledger before believing their conclusions.
25. **A grep that cannot match is not a clean result.** Check the emitter before
    believing a zero — and 109, a capped or thinned log line is not a count.
30. **A test that has never failed has not been shown capable of failing.** Break the
    implementation on purpose and confirm the test screams. Applies to diagnostics and
    to instruments as much as to tests (94, 158).
50/51/86. **A rate measured once is a fact about that afternoon, and the control is the
    old binary run NOW** — not its remembered numbers. 159: a bimodal arm makes every
    single-run claim a coin flip.
133/127. **One frame of an animated scene is ONE SAMPLE**, and that applies to LOOKING,
    not just to measuring.
172/268. **A retirement is only as good as the ORACLE it was measured on** — and YOUR OWN
    STUB IS AN ORACLE. Re-ask your earlier A/Bs whenever an upstream defect is fixed. A
    three-configuration same-binary arm retired "the prologue waits on audio" in part 16;
    it was true, and a real decoder proved it in phase A/V.
267. **A guest structure handed to a DMA device holds PHYSICAL addresses**, and in a flat
    recompiler map those are not the ones the CPU uses. Cost this port its whole audio
    subsystem for 28 parts. Print the DESTINATION on every file-IO trace.
190/103. **A gate that needs a human is a capture request in disguise** — extend the arm
    until it is not. 222 is the performance form: ask what the WORST case renders and
    whether your harness can reach it.
237/238. **A MEAN frame time measures this title's vblank pacing floor, not your change**
    — read medians and the share of frames pinned to a 16 ms multiple, which is the far
    more sensitive statistic (10% -> 97% where the mean moved 1.7%). And **a profiler
    column that falls to zero is not a saving until you find where the replacement work
    got charged**: `streams` read 0.0% while the guard that replaced it doubled `record`.

## Inherited from the Fable 2 port: shared-decode cross-checks, and a do-not-chase list

Everything below is **hardware-level decode**, not title-specific, so a defect found in
one port is a defect in the other unless this one was written differently. Checking is
minutes; diagnosing the symptom is days. **Both of the first two were checked in
session 21 and this repo is correct on both** — recorded so the next session does not
re-check them, and so Case West can check them in one grep.

| shared decode | correct form | this repo |
|---|---|---|
| fetch-constant SIZE field — the endian bits occupy the low 2 bits of `fdw1`, so `fdw1 & 0x7FFFFF` swallows them (reads ~4x too large, permits reads past the buffer, and *under*-reports past ~2^21 dwords) | `(fdw1 >> 2) & 0xFFFFFF` | ✓ `gpu/xenos.h:125`, and gotcha 110 is the same finding arrived at independently |
| `num_format_all` INTEGER semantics — a fetch declaring unsigned/integer on `k_8_8_8_8` bound as `R8G8B8A8_UNORM` delivers 0..1 where the guest asked 0..255 (typically packed bone indices, TEXCOORD-wrapped as 360 titles do) | deliver the integer as its own value into a FLOAT input | ✓ via `USCALED`/`SSCALED` in `XenosVertexFormat`, gotcha 122 |

On the second: Fable 2 fixes it by emitting `* (2^bits − 1)` at translation time and
warns *against* rebinding to a UINT format. `USCALED`/`SSCALED` is not that rebind — it
is the same semantic (an integer delivered as its own value into a float input) obtained
without touching the emitter or the vertex input signatures. Both are correct; ours
costs nothing in XenosRecomp, which matters because that recompiler is shared.

**Confirmed NON-issues — do not chase these.** Measured over ~860 shader blobs and 971
vertex fetches in the **Fable 2** bank, so the provenance is another title and the
census has not been repeated here (one pass over our 336 shaders would settle it, and
`tools/gdis.py` plus the meta sidecars are enough to do it):

- the guest requests **8-in-32 endian on 100%** of fetches;
- **`exp_adjust` is declared but zero everywhere**;
- the Xenos compiler emits a **`yxwz`-shaped destination swizzle on ~87% of 16-bit
  fetches** (identity on 32-bit) that compensates the 8-in-32 pair transposition, and
  XenosRecomp already honours it on both the declared and `XeVfetchDep` paths.

**That third one was a live lead here and PART 37 CONFIRMED IT — it was the whole
striped-material class (item 0s).** The shader's destination swizzle IS the complete
correction: our `CopySwapped` leaves 16-bit pairs in exactly the state the real fetch
pipe hands the shader, so `g_SwappedTexcoords` was compensating A SECOND TIME, and
lightmap UVs (16_16 TEXCOORD2 fetches) arrived transposed — baked prop shadows painted
as hard-edged black blotches on the tanker, Dick's far LOD and the pawnshop boards.
§6n's frame-wide null was honest and blind: the damage is localized to lightmapped
props. The mask now defaults to ZERO; `CZ_VK_TEXCOORD_SWAP=1` is the control arm.
`docs/phase5-notes.md` §6bo, gotchas 291-292. For Case West: publish NO texcoord swap
mask; trust the microcode's own swizzles.

## Layout

- `config/CaseZero.toml` — XenonRecomp main config: helper addresses, plus 139 function
  overrides from three sources that **merge, never replace** each other — switch-tail
  repairs (`tools/fix_switch_function_bounds.py`), coverage-recovered entry points
  (`tools/coverage_to_function_overrides.py`), and truncated-function widenings
  (`tools/find_dropped_branches.py --widen`). Regenerating any of them from a stale
  `ppc/` silently under-reports; always rebuild `ppc/` from the committed config first.
- `config/CaseZero_switch_tables.toml` — 232 jump tables (105 absolute, 85 offset8,
  42 offset16, 6,114 labels) from `tools/find_jumptables.py`. **XenonAnalyse finds zero
  here** — see gotcha 3.
- `assets/package/` — the XBLA STFS package as delivered (gitignored; copyrighted).
- `assets/game/` — what `tools/extract_stfs.py` unpacked out of it: `default.xex` +
  `data/` (gitignored).
- `assets/game/default_image.bin` (+ `.sections`) — the loaded image for offline
  analysis, from `tools/xex_image_dump`.
- `ppc/` — generated C++ (gitignored; 156 MB, 57,822 functions, regeneratable).
- `tools/` — analysis scripts. Several copied from the earlier ports; provenance in
  their headers. `gdis.py` is the guest disassembler and is usually the right first
  stop for any question about what the title's own code does.
  `import_call_sites.py` is the one to reach for when implementing a kernel import:
  the capture has no return values, so the guest code that consumes the result is the
  specification (finding 29).
- `docs/` — the project's memory. **Read in this order for a new session:**
  - **`xenia-capture-analysis.md`** — the numbered findings ledger, and the authority on
    any measured number: where another doc disagrees with it, it wins.
  - **`gotchas.md`** — the 338-entry transferable ledger. Every "gotcha N" resolves here.
  - **`port-history.md`** (what each session established) and **`open-items.md`** (the
    backlog, in order) — both split out of this file on 2026-08-08.
  - **`d3d-translation-plan.md`** — the renderer-architecture pivot, its recon tables and
    licensing, plus the per-phase build-out records. **The first read before any renderer
    work.** `d3d-kickoff.md` and `d3d-phase-c{,2..26}-kickoff.md` are the per-part
    hand-offs, each superseding the last; the newest is the live one. A kickoff's most
    valuable section is its list of the parts of that phase that **already exist** and
    would otherwise be rewritten from the plan text — write that section for every phase.
  - `phase1-notes.md` / `phase3-notes.md` / **`phase5-notes.md`** — the per-phase records
    of what the runtime work found that neither the plan nor the kickoff predicted, with
    `phase{1,3,5}-kickoff.md` the matching hand-offs and `runtime-plan.md` the phase plan.
  - **`phase-av-notes.md`** (sound, the cinematic that was waiting for it, and part
    29's diagnosis of the loop that followed) with `phase-av-plan.md` the plan it
    executed, `phase-av-kickoff.md` the phase hand-off and `part29-kickoff.md` its
    successor. `d3d-phase-c28-kickoff.md` records how the white-surface chain
    was built, but **two of its eight steps are retired and its item 0 is answered** —
    read `phase5-notes.md` §6ba before following anything in it.
  - **THE LIVE HAND-OFF IS ALWAYS THE HIGHEST-NUMBERED `partNN-kickoff.md`**, and it
    supersedes every earlier kickoff on "where the port is". **It is currently
    `part51-kickoff.md`.** State the rule as well as the name, because this line said
    "`part32-kickoff.md` is the LIVE one" for nineteen parts after it stopped being true
    — a stale pointer in the file every session loads whole is the one documentation
    defect that misroutes a session before it has read anything else (gotcha 13).
  - **`perf-plan-part50.md` is the LIVE performance plan** — but its opening box lists
    four of its own numbers that part 50 measured and found wrong, and `phase5-notes.md`
    §6cg is the evidence; read the box before acting on any tier. `perf-cpu-plan.md`
    and `perf-plan-part{47,48}.md` are its executed predecessors.
  - `instruments.md` (every env var and arm), `measurement.md` (how to judge a change),
    ~~`perf-cpu-plan.md` (the live performance plan)~~ and `perf-plan-overnight.md` (its
    executed predecessor).
  - Formats and tooling: `big-archive-format.md` (the cracked `.big` container, **plus
    two part-27 retractions: the name table is NOT fixed-width outside the shader
    banks, and 1,671 of 12,481 entries ARE compressed** — read it with
    `tools/big_list.py` and `tools/big_decompress.cpp`),
    `xtr-decoder.md` (the GPU stream format + the determinism method),
    `xenonrecomp-upstream-bugs.md` (local recompiler patches),
    `bootstrap-2026-08-04.md` (day 1), `xenia-capture-requests.md` (unfulfilled
    requests), `reusability.md` (what transfers to Case West), `phase5-3d-plan.md`
    (superseded, but its Step 0 instrument and Step 1 findings survive).
- `Xenia logs/` — captures land here (gitignored); keep an index in
  `Xenia logs/Xenia_Run_Content.md`, which **is** tracked.
- `~/DR2CZ-troubleshooting/` — **outside the repo on purpose**: operator screenshots
  (the only evidence channel for "does it look right", and the one no instrument here
  can replace) and headless `CZ_VK_FRAME_DUMP` frames. Its `INDEX.md` says what every
  shot showed, INCLUDING the ones that were lost — Spectacle deletes its temp directory
  when the window closes, so most of the first gameplay session's screenshots survive
  only as descriptions. Save straight into it.
- `runtime/` — the host runtime. Phases 1 and 3 complete; **phase 4's command
  processor is live too, ahead of the plan's ordering** — do not read the plan's phase
  numbers as the state of the code. There is a window, a present seam and real input;
  there is **no renderer** (phase 5), so the window is blank on purpose. Target is
  `cz_runtime`; the phase 0.2 link gate survives as `cz_runtime --smoke`.
  - `CMakeLists.txt` — **selects clang++ before `project()` (gotcha 31)**, and
    enables C for exactly one file (o1heap) so the .c source is not silently ignored.
  - `main.cpp` — image load → header publish → data-import resolution → guest entry,
    plus the `--smoke` gate.
  - `cpu/timebase.{h,cpp}` — the 49.875 MHz guest timebase, force-included over
    `ppc/` only (gotchas 1 and 32). `kernel/imports.cpp` shares `CZ_TIMEBASE_HZ` so
    `KeQueryPerformanceFrequency` and `mftb` cannot drift apart.
  - `cpu/guest_thread.{h,cpp}` — PCR/TLS/TEB block + guest stack. Both constants are
    from this XEX's header as A1 prints it: 64 TLS slots, 0x40000 stack.
  - `kernel/{memory,heap}.*` — the flat 4 GB map and the four arenas; the layout is
    checked against A1's own allocations, not inherited (`docs/phase1-notes.md` §3).
  - `kernel/{kobject,guestcall,klog}.*` — handles, the import marshalling seam, and
    the `[kcall]` trace the gate diffs.
  - `kernel/xex_imports.*` — publishes the XEX headers into guest memory and resolves
    the 244 IAT slots + 13 kernel variables.
  - `kernel/imports.cpp` — the kernel HLE, written in A1's call order.
  - `kernel/audio.{h,cpp}` — the XAudio render-driver client (a guest-thread pump
    calling the title's callback at 5.333 ms/frame, measured at 187.4-187.6/s against
    the 187.5 that 48 kHz needs), the XMA context array + its MMIO register file, and
    **the XMA DECODE WALK**. Finding 36; every structural claim is quoted from the
    guest function that states it, and the context layout is checked against the
    guest's own arithmetic rather than against Xenia's struct (`dw[8] - dw[7] = 6400
    = 25 blocks x 256`). **Read the physical-address note before touching a context
    pointer** — that one thing was the whole audio defect.
  - `audio/{xma_decoder,audio_out}.{h,cpp}` — phase A/V. ffmpeg's `AV_CODEC_ID_XMA2`
    (lifted from the Fable 2 port, so the licence is ours) and the SDL device.
    Deliberately dumb: the title's own mixer sums every voice into the one 5.1 frame
    it submits, so anything wrong with the SOUND is upstream of `audio_out.cpp` and
    `CZ_AUDIO_TRACE`'s peak is what tells the two apart.
  - `kernel/content.{h,cpp}` — the save-data layer: the content enumerators, the
    XAM enumerate message (app 0xFE, message 0x0002000E) and the mount that makes
    `save:` a host directory. Its header comment is the derivation of the whole
    protocol out of the title's own statically-linked `XamEnumerate` — read it before
    changing anything here, and lift it for Case West (gotchas 104-106).
  - `kernel/{vfs,file_imports}.*` — the file layer. In phase 1 rather than phase 2
    because A1's 22nd distinct kernel call is already an `NtCreateFile` (finding 16).
  - `kernel/import_stubs.cpp` — generated; honest-failure returns, not aborts.
  - `cpu/crash_report.cpp` — the guest state on any fault. Its host pc is the one
    field that is never stale (gotcha 57); `addr2line` it.
  - `cpu/guest_probe.cpp` — argument probes on named guest functions via the alias
    seam, behind `CZ_ARG_PROBE`. Kept as the worked example of tracing a bad value
    back to its producer; it is what closed finding 27.
  - `gpu/vk_renderer.{h,cpp}` + `gpu/xenos.h` — **phase 5: the renderer.** Inert
    unless `CZ_VKDRAW=1`. `xenos.h` holds the register indices and format codes with
    each field layout written next to it, because every one of them is a magic number
    whose wrong value is silent. The header comment of `vk_renderer.cpp` transcribes
    the interface the translated shaders present (push constants, the five descriptor
    spaces, the shared-constants offsets) out of the generated HLSL — read that, not
    this, if the two ever disagree.
  - `host/window.{h,cpp}` — phase 3: the SDL window, the event loop, the present
    seam and the pad, deliberately in **one** module because in SDL they are one
    thread. Everything except `Host_Present` (called from the PM4 executor) and
    `Host_PadState` (called from whichever guest thread polls `XamInputGetState`)
    runs on the thread that created the window, which is the process's main thread —
    which is why `main.cpp` now runs the guest entry on a spawned thread
    (gotcha 99). Compiles to honest stubs without `CZ_HAVE_SDL`.
- Recompiler TOOL at `~/GithubRepo/XenonRecomp` (built at `build/`; carries local
  patches — see `docs/xenonrecomp-upstream-bugs.md`). Shader translator at
  `~/GithubRepo/XenosRecomp` (also patched; Case Zero inherits those fixes for free).

## Commands

Unpack the game (once):
```
python3 tools/extract_stfs.py "assets/package/58410A8D/000D0000/<hash>" -o assets/game
./tools/build_xex_image_dump.sh
./tools/xex_image_dump assets/game/default.xex assets/game/default_image.bin
```

Regenerate the recompiled C++ (from repo root; `ppc/` must exist or XenonRecomp
segfaults in `fwrite`):
```
mkdir -p ppc && cd config && ~/GithubRepo/XenonRecomp/build/XenonRecomp/XenonRecomp \
    CaseZero.toml ~/GithubRepo/XenonRecomp/XenonUtils/ppc_context.h
```

Regenerate the switch tables — **use our scanner, not XenonAnalyse**:
```
python3 tools/find_jumptables.py assets/game/default_image.bin \
    -o config/CaseZero_switch_tables.toml
```

Repair function bounds after any switch-table change, then re-run the recompiler and
confirm the log has zero `jump outside function` lines:
```
python3 tools/fix_switch_function_bounds.py --apply
```

Check for silently dropped direct branches — **this is not optional after any change to
the function list**, and it is the only thing that catches the coverage oracle's
loop-header splits (gotcha 28). Regenerate `ppc/` between each step:
```
python3 tools/find_dropped_branches.py            # report both classes
python3 tools/find_dropped_branches.py --prune    # backward: remove spurious starts
python3 tools/find_dropped_branches.py --widen    # forward: widen truncated functions
```

Then check that every switch-shaped `bctr` was actually lowered — the gate for the
defect class that leaks a callee's non-volatiles into its caller (gotchas 53-55).
**Exit 1 = a real defect; run it last, and after any config change:**
```
python3 tools/find_unlowered_switches.py          # 0 defects expected
python3 tools/find_unlowered_switches.py --all    # also list the benign tail-call thunks
```

Build the SPIR-V shader cache. **`assets/shader_spv/` is gitignored, so a fresh clone
needs this before `CZ_VKDRAW=1` does anything.** Two sources, and they merge: the
captures' shaders (which reach gameplay, where our runtime cannot yet go) and our own
dump (which is the authority on the byte range, because the cache key is a hash of
it — gotcha 115). **Our dump run must go as DEEP as the runtime can go, not just to the
title screen** — the plain boot ends at the title and the prologue loads a shader
neither capture contains, which the renderer then declines to draw with (28,718 draws a
run, one line in the log and nothing else):
```
python3 tools/xenia_ucode_to_cache.py \
    "Xenia logs/A1_boot_title_fullgame/shaders" \
    "Xenia logs/A2_gameplay_stillcreek/shaders" /tmp/ucode      # 335 distinct
(cd runtime/build && CZ_NO_WINDOW=1 CZ_SHADER_DUMP=/tmp/ucode \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A timeout 300 ./cz_runtime)  # +2
tools/build_shader_spv.sh /tmp/ucode assets/shader_spv          # 337, zero failures
```
The check that costs nothing, and the only thing that reports this at all — a shader the
cache lacks is one log line and a silent counter, not a failure:
```
grep -c "no translated shader" run.log         # must be 0
```
**AND THAT CHECK ONLY FIRES FOR A SHADER A RUN ACTUALLY BINDS.** Part 27 found
`ps_7d6044e7dcaea1f2` sitting in `~/DR2CZ-troubleshooting/ucode-dumps` and MISSING from
the built cache — microcode we have held for sessions and never translated. No run had
bound it, so the counter read 0 every time, and the module COUNT could not show it either
because the cache also carries `ps_926c15dd20571cf1`, whose microcode is lost: 410 dumps,
410 modules, different sets. **The gate is to rebuild from the dumps and compare the
NAMES, not the count** — it is free and it is two lines:
```
tools/build_shader_spv.sh ~/DR2CZ-troubleshooting/ucode-dumps /tmp/spv_check
diff <(ls /tmp/spv_check/*.spv | xargs -n1 basename) \
     <(ls assets/shader_spv/*.spv | xargs -n1 basename)   # only the lost-microcode entry may differ
```
And the gate on the sidecars themselves, which is **two-sided by construction** — the
per-slot texture dimension is derivable both from our ucode parse and from DXC's
`OpDecorate ... DescriptorSet` words, so a disagreement means one of the two decodes is
wrong. Run it after any cache rebuild; exit 1 is a real defect:
```
python3 tools/shader_dim_census.py             # 310 modules 2D, 94 cube, 0 disagreements
```
It also names the sidecars carrying no `tfetchDims` at all — cache entries built before
part 25 whose microcode is gone. **Keep ucode dumps in `~/DR2CZ-troubleshooting/ucode-dumps`,
not in `/tmp`**, which is a tmpfs: eleven entries were lost that way and two operator runs
(the military arrival, then Still Creek end to end) recovered TEN of the eleven. The last,
`ps_926c15dd20571cf1`, samples only sets 0 and 3 — an ordinary 2D shader, so nothing
depends on it. A lost dump is a location nobody has replayed, not a permanent loss.
**The cache is 430 and it has grown on EVERY session that reached new ground.** 335 from
the captures, 337 with our own dump, then 339, 353, 370, 371, 391, 394, 397, 402, 409, 411,
419, 424, 430 — 23 of those from two operator play sessions on 2026-08-08 alone, and
**11 more from part 39's operator evening, of which THREE were never reported missing by
any run and only the name-diff gate found them**, once the whole-frame black
stopped hiding the parts of the map nobody had visited. **Treat "the cache is complete"
as a claim with a shelf life** (gotcha 13): every era of this game that no run has
entered is a shader gap nobody has counted, and the counter is one log line.
A1 stops at the title screen,
A2 is gameplay, and the prologue and Still Creek each loaded a shader neither capture
nor our own dump contained. Any run that reaches new ground should carry
`CZ_SHADER_DUMP` so the blobs are captured for free — including an OPERATOR run, which
is the only way this port reaches most of the game.

**And if a run finds a missing shader without `CZ_SHADER_DUMP` set, the blobs are not
lost — recover them from the LIVE process.** `[imload] VS va=%08X hash=%016llx size=%u`
prints the guest address and the dword count, `runtime: guest memory at 0x...` prints
the host base, and the renderer's own hash is a self-check on the result:
```
gdb -p <pid> -batch -ex "dump binary memory vs_<hash>.ucode <base+va> <base+va+size*4>"
python3 -c "..."   # FNV-1a over the bytes must equal <hash>
tools/build_shader_spv.sh <dir> assets/shader_spv
```
Both of Still Creek's were recovered this way and both hashed EXACTLY, which is what
makes it a measurement rather than a hopeful memory read — a stale or reused buffer
would fail the hash rather than produce a plausible wrong shader. **Part 19 recovered
TWENTY this way from a single operator session, all twenty hashing exactly**, which
takes this from a trick that worked once to the standard recovery.

**Use `process_vm_readv`, not `gdb`, when the operator is still playing.** It reads
another process's memory without ptrace-stopping it, where a `gdb` attach freezes the
game for a second — which during a load test contaminates the very thing being tested.
Same permission model, no interruption; ~30 lines of `ctypes` and the FNV-1a check is
identical.

Look inside the game's `.big` archives — 146 of them, 12,481 entries:
```
python3 tools/big_list.py --all --find cc_          # search every archive by entry name
python3 tools/big_list.py <a.big> --extract <name> --out DIR
./tools/build_big_decompress.sh                     # once; links XenonRecomp's own LZX
tools/big_decompress <extracted> <out.bct>          # 1,671 entries are compressed
```
**`--extract` writes the STORED bytes**, which for a compressed entry is the compressed
stream and not the asset — it says so when that happens. `big_decompress` checks its own
output against an oracle (every loose `.bct` on disc opens `05 01 01 E2`) rather than
asking anyone to eyeball it. Both tools NAME what they could not parse instead of skipping
it: the first version of `big_list` silently dropped 95 of 146 archives, which would have
answered "is this asset here" with a confident no.

Build the runtime (needs `clang++`, **SDL2, Vulkan and ffmpeg**; ~90 s on 16 cores for a
cold image build). All three are required rather than optional-with-a-fallback, and for
one reason: **each of them, if it silently went missing, would present as a defect in the
game rather than as a build problem.** A lost window looks like input that stopped
working; a lost renderer looks like a black screen; a lost `libavcodec` looks like the
mute game this port shipped for 28 parts. Requiring them at CONFIGURE time is how those
stay build messages. It is safe to require all three because each is off at RUN time
unless asked for — `CZ_VKDRAW=1` for the renderer, `CZ_NO_AUDIO_OUT=1`/`CZ_NO_XMA_DECODE=1`
to switch audio back off — and `-DCZ_WINDOW=OFF` is how you say "headless on purpose" out
loud. On Fedora: `sudo dnf install SDL2-devel vulkan-loader-devel vulkan-headers
ffmpeg-devel`.
```
python3 tools/gen_import_stubs.py                 # after any change to the import set
cmake -S runtime -B runtime/build -G Ninja
cmake --build runtime/build -j$(nproc)
./runtime/build/cz_runtime --smoke                # the phase 0.2 link gate, still live
```

Reach live GAMEPLAY headlessly. Until part 16 this needed an operator, so every
gameplay claim was a report with no reproduction (gotcha 190). **START skips a
cinematic**, and the Zombrex tutorial's second page needs D-pad LEFT to open the watch
and B to leave it — without those two the run parks on the card forever:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,NONE \
  CZ_VK_FRAME_STATS=/tmp/gp.txt timeout 330 ./cz_runtime > /tmp/gp.log 2>&1)
tail -200 /tmp/gp.txt | awk '{print $5}' | sort -u | wc -l     # 200 = the camera moves
```
Arrives at ~185 s and reaches file **#184**, ~1,860 draws a frame. **Check the
camera-distinctness number before trusting anything measured off it**: every step is a
fixed 8 s interval against a boot whose depth in fixed wall time has always been a
distribution (gotcha 75), so the press counts will drift with load or frame rate. It
MANUFACTURES progress, so it is never a gate configuration (gotcha 78).

**REACH THE OUTDOOR WORLD AND A CROWD VIA THE TITLE'S OWN DEBUGJUMP SCREEN. This is the
recipe to use** — it is the operator's route, it is anchored to an EVENT rather than to a
wall clock, and it lands Chuck by the military camp in a full crowd at **7,431 draws** in
under half a minute of menu work:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE \
  CZ_VK_FRAME_STATS=/tmp/out.txt timeout 420 ./cz_runtime > /tmp/out.log 2>&1)
awk 'NR>1 && $2>m {m=$2} END {print m}' /tmp/out.txt      # >= 6,000 = it got there
grep -E "WAITJUMP|requested DebugJump" /tmp/out.log       # the four lines that prove it
```
`F2` opens the shipped DebugJump screen (needs `CZ_DEBUG_MENU=1`); `DOWN` once selects
`Case 0-2`, which spawns outside. **`WAITJUMP` is the load-bearing part**: the DebugJump
request is HELD until the frontend exists and lands whenever it lands (27 s here, 131 s on
another boot), so the barrier parks the sequence — repeating the preceding entry, which is
why `START` precedes it — until the screen is actually up, then starts the remaining
intervals from that moment. That is what makes this reproducible where a fixed-time recipe
is a fit to one afternoon (gotchas 75, 251).

**FULLY UNATTENDED, with the AI driving and the map handled** — this is the form to use
for texture coverage or any long roam:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER \
  CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE \
  CZ_SHADER_DUMP=~/DR2CZ-troubleshooting/ucode-dumps timeout 600 ./cz_runtime > /tmp/o.log 2>&1)
grep -E "WAITJUMP|EXPLORER engaged|changed the state away|pressing B" /tmp/o.log
```
Three things it handles that took a session to find, each measured rather than reasoned:
**the title's AI rewrites the state you set** (we wrote ITEM PICKER twice; a live read of
the running process found MISSION MASTER), so `CZ_AUTOCHUCK` re-asserts and counts the
overrides — 3 in an 8,657-frame run. **The AI opens the MAP by itself** about two minutes
into a roam and parks the run on it — not our input (a BACK-delivered counter reads 0, and
`CZ_FAKE_START_MS` bypasses the real pad entirely), so it is closed by hash
(`06903E1A`/`890DF3E5`, found with `CZ_SCREEN_TRACE=1`). **The close is B, not BACK** —
BACK is what opens the map. See `docs/instruments.md` for all four variables.

**Why this replaces the stick recipe below for anything comparative:** two arms of a
picture A/B are only comparable where their `drawFingerprint` and `cameraFingerprint`
agree, and with the stick recipe that filter yields 13-44 frames of ~300, every one under
1,800 draws — i.e. the outdoor era was never comparable at all (gotcha 247). Add
`AutoChuck` from the debug menu for coverage, or leave the character standing still, which
should hold the camera fingerprint matched across arms for long stretches.

The older stick recipe, kept because it walks a DIFFERENT route (out of the safehouse into
Still Creek, with a camera sweep) and because several recorded measurements were taken on
it — Chuck walks out of the safehouse into
Still Creek and the camera sweeps. It parks in the safehouse at
~1,900 draws, which is against the title's own two-vblank cap where a CPU saving
measures as exactly zero (`docs/perf-cpu-plan.md` item 0, which this closes). The extra
`B` is the safehouse door; a stick entry HOLDS for its whole interval where a button
taps for 150 ms, which is why walking needs `LSUP` and not `A`:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,LSUP,LSUP,B,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,LSUP,LSUP,LSUP,RSRIGHT,RSRIGHT,LSUP,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,LSUP,LSUP,RSLEFT,RSLEFT,RSLEFT,RSLEFT,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,NONE \
  CZ_VK_FRAME_STATS=/tmp/out.txt timeout 620 ./cz_runtime > /tmp/out.log 2>&1)
awk 'NR>1 && $2>m {m=$2} END {print m}' /tmp/out.txt      # >= 6,000 = it got there
```
Reaches the junkyard behind the safehouse by ~300 s at **6,400-8,100 draws a frame**,
past the operator's 6,592. Same warning as above and more so: it is 57 fixed 8 s steps
against a drifting boot, so **check the draw count before trusting anything measured off
it** rather than assuming the run went where the last one did.

Run the guest and gate it against hardware. **Both captures, always** — A1 is the
authority for the boot sequence, A5 for the synchronisation surface, and A5 is *not* a
superset of A1 (gotcha 45):
```
(cd runtime/build && ./cz_runtime > /tmp/run.log 2>&1)      # ^C or timeout; it parks
python3 tools/kernel_call_diff.py \
    --xenia "Xenia logs/A1_boot_title_fullgame/cz_run1.log" --ours /tmp/run.log
python3 tools/kernel_call_diff.py \
    --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" --ours /tmp/run.log \
    --include-high-frequency
```

Runtime instruments and measurement arms: **the full catalogue is
`docs/instruments.md`** — every environment variable this runtime reads, what it
measures, and for the ARMS what they are the control for. All are off by default and
free when off. Split out of this file on 2026-08-08.

The ones reached for most often:

```
CZ_NO_WINDOW=1     headless; the control arm for every phase 3 claim
CZ_VKDRAW=1        the renderer. OFF by default, so the same binary is the control arm
CZ_VK_PROFILE=N    the frame's CPU time by phase, every N seconds, plus a `pump` line
                   splitting the graphics pump's own ticks/sleep/walk/`pm4`, plus a
                   packet census (packets/frame, ns/packet, register dwords/frame).
                   START HERE for any performance question — and read the GPU's clock
                   before believing the `submit` column (gotcha 219). **Every phase is
                   EXCLUSIVE of the ones nested inside it as of part 20; it was not
                   before, and numbers from earlier sessions overstate `record` by the
                   whole of `streams` (gotcha 228)**
CZ_VK_FRAME_STATS=file   one line per presented frame; input to tools/frame_compare.py,
                   tools/frame_determinism.py (is a matched comparison possible at
                   all?) and tools/frame_era_medians.py (the outdoor A/B)
CZ_VK_FRAME_DUMP=dir     every 64th frame as a PPM — the picture, self-servable
CZ_VK_SNAP_DUMP=dir      EVERY resolve snapshot of one frame: which PASS went wrong
CZ_RING_TRACE=1    the ring, the brake's health, and the GPU/CPU hand-off chain counted
                   link by link. `truncated=0` is a standing gate
CZ_FILE_TRACE=1    every open/read, including the not-founds
CZ_WAIT_TRACE=1    name any infinite wait that outlasts 5 s, with guest callers
CZ_FAKE_PRESS_SEQ=...    synthetic input. MANUFACTURES PROGRESS — never a gate run
CZ_GUEST_LOG=1     the engine's OWN debug printf (640 callers). The sink — pair it
                   with CZ_GUEST_DIAG or it prints only the ungated errors
CZ_GUEST_DIAG=1    **the switch.** One byte, `0x829EC974`, read by 2,013 sites and
                   written by none, shipped as 1: a release KILL SWITCH, not the
                   "flag per category a shipped build left at zero" that gotcha 215
                   guessed and gotcha 266 corrects. Clearing it (plus `0x82AC3EAD` so
                   the un-silenced asserts print rather than trap) takes the outdoor
                   route from **0 `[guest]` lines of 11,168 to 1,239** — null and
                   positive control in one pair of runs. It is what makes the streaming
                   and zone-LOD decisions readable. A DIAGNOSTIC ARM ONLY: 2,013
                   formatting sites on the frame path, so never quote a frame time
                   from a run with it set (gotcha 7)
CZ_NO_XMA_DECODE=1 **the control arm for sound.** Contexts allocate, the pump runs,
                   nothing decodes — the runtime as it was for 28 parts. It is also the
                   arm that showed the prologue cinematic was waiting on audio
CZ_NO_AUDIO_OUT=1  the whole pipeline, no device. Separates "the guest produced audio"
                   from "we played it", and it is what a headless gate run wants
CZ_XMA_DECODE_LOG=1  per-context decode activity, and REFUSED split from short
CZ_SHADER_DUMP=dir put this on any run that might reach new ground, including an
                   operator run: a missing shader is one log line and a silent counter.
                   **NEVER point it under /tmp** — that is a tmpfs and it is why eleven
                   cache entries have no microcode left. Use
                   `~/DR2CZ-troubleshooting/ucode-dumps`
CZ_VK_NO_CUBE_SNAPSHOT=1  decline the cube map the TITLE RENDERS ITSELF to the white
                   dummy — the part-25 renderer in the part-26 binary, and the control arm
                   for the cube snapshot path (35.9% of cube fetches on the outdoor route)
CZ_VK_NO_CUBE=1    bind cube fetches the pre-part-25 way (into the 2D array, so the shader
                   samples the white dummy). The same-binary control arm for the cube-map
                   work, and the one to hand an operator for a side-by-side
CZ_VK_CUBE_POISON=1  the cube dummy becomes MAGENTA — the positive control that showed the
                   cube sample reaches the presented image (80 of 110 frames, up to 72% of
                   a frame's pixels). Read it with a per-pixel diff against the unpoisoned
                   run, NOT by looking for magenta: the sample TINTS, and the semantic
                   detector read 0.24% where the diff read 80 of 110 (gotcha 248)
CZ_VK_DIM_CENSUS=1 where the DIMENSION lives in a texture fetch constant, answered by
                   partitioning fetches on the shader's independent answer (gotcha 244)
CZ_DEBUG_MENU=1    enables retained debug scaffolding; at the title menu press F2 to
                   open the shipped, operator-confirmed DebugJump testing screen; F4
                   opens the host-rendered Case Zero debug submenus (Left goes back)

```


`CZ_KCALL_WHO` is the companion to the phase gate: the gate says *that* our
first-occurrence order diverges, and the most informative divergences are imports we
call which hardware never calls at all. Only the call site explains those.

**The measurement and analysis recipes live in `docs/measurement.md`** — the renderer
A/B and its noise floor, the picture checks against capture E (`frame_signature.py`,
`frame_sharpness.py`), the guest disassembler `gdis.py`, the `.xtr` capture tools, the
bin-predication oracle and the two PM4 boundary oracles. **Run the PM4 oracles after any
command-processor change** (exit 1 = we would desync on a real stream), and never claim a
renderer improvement without a same-binary A/B aggregated over an era.

## The recompilation contract (identical to Fable 2 and Asura's Wrath)

- Every guest function → `PPC_FUNC_IMPL(__imp__sub_XXXXXXXX)` taking
  `(PPCContext& ctx, uint8_t* base)`.
- Guest 32-bit addresses index into `base`; `PPC_LOAD/STORE_*` swap endianness.
- Hooks: define a strong `PPC_FUNC(sub_X)` calling `__imp__sub_X(ctx, base)` pre/post.

## Game intel (established 2026-08-04)

- **Package**: XContent `LIVE`, content type `0x000D0000` (Arcade Title), STFS volume,
  256 files, 825 MB. Display name `DEAD RISING 2: CASE ZERO`. Title ID `58410A8D`.
- **XEX**: image base `0x82000000`, entry `0x825D9F30`, image size `0xB40000`,
  `.text 0x82150000 + 0x873564`. Encryption 1 (**devkit key**), compression 2 (**LZX**).
  244 imports.
- **Engine**: Blue Castle Games' in-house engine, shared with the full Dead Rising 2 —
  the image still carries DR2's zone names (`americana`, `atlantica`, `arena_stadium`,
  `boss_battle_*`) though Case Zero ships only the Still Creek content. This is why Case
  West should be cheap after this.
- **Middleware**: Havok physics (`hkp*`/`hkx*` RTTI), XMA audio, an in-house
  "CrowdEngine" for the zombie crowds. ~~Bink video.~~ **Retracted (finding 7)** — the
  image contains the strings `Bink_1`/`Bink_2`, which is what the day-1 inference was
  built on, but there is no Bink decoder in use and no `.bik` file in the package. The
  strings are a dead or renamed path. Middleware named in an image is evidence that a
  name exists, not that a codec runs.
- **Assets**: `.big` archive containers throughout, `.bct` textures, `.bcf` fonts. At
  least one path is constructed at runtime (`anm_%s.big`), so the VFS must handle
  arbitrary paths rather than a fixed manifest. Format not yet cracked; Fable 2's `.bnk`
  work is the closest model.
- **Shaders ship loose on disc**:
  `data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`. ~~If those hold raw Xenos
  microcode they feed XenosRecomp almost directly.~~ **Retracted (finding 6):** they are
  `.big` archives of `<hash>.vo` shader *objects* carrying build metadata (including
  `.updb` debug paths), and their payloads share only background-noise n-gram overlap
  with the microcode the guest actually submits. The renderer input instead comes from
  Xenia's `dump_shaders`: 455 microcode blob files = **335 distinct shaders** (A1's
  120 are a strict subset of A2's 335), all translated in phase 5.
- **No Bink** (finding 7). Grep `.big`, never `.bik`. **And "movie" is the wrong word:
  part 27 read the archives and a cinematic is an IN-ENGINE SCRIPTED SCENE** — 29 `.txt`
  scripts in `data/cinematics/cinematics.big` naming a camera animation, actor animations,
  particles, a HUD event and one audio event, with the animation data in
  `data/anim/cinematic/*.big`. There is no codec to write. `docs/phase-av-plan.md`.

## Ground truth in hand (round 1, delivered 2026-08-04 — COMPLETE)

**START HERE: `docs/xenia-capture-analysis.md`** — the numbered findings ledger. It is the
authority on measured numbers; where any other doc disagrees with it, it wins.

Index of what each capture is: `Xenia logs/Xenia_Run_Content.md` (written by the
operator). All runs are the **full game** (`license_mask = 1`) on the instrumented
canary fork, STFS package launched directly.

- **A1** boot→title at L3 (13.9 MB) + the Section D shader dump · **A2** gameplay
  (606 MB) + gameplay shaders · **A3** save round-trip + the physical save file ·
  **A4** 5-min title idle · **A5** A1's drive with high-frequency logging (231 MB).
- **B1/B1b** GPU `.xtr` boot→title + determinism repeat · **B2** GPU gameplay
  (**7.95 GiB** — the operator fixed Xenia's 2 GiB `.xtr` cliff at source to get it).
  Each with a same-run L3 correlation log.
- **C1/C2** function coverage: 12,278 boot→title, 17,118 gameplay, +4,840 gameplay-only.
- **E** five screenshots as the visual target.

**THERE IS NO OUTSTANDING CAPTURE REQUEST.** The only candidate is an optional A2b
(gameplay-era `.big` seek order), and finding 8 explains why it is probably unnecessary.

Highlights that change how we work:
- **The trial trap fired.** `license_mask` defaults to 0 → the game boots its **trial**,
  whose boot differs measurably (`chuckwalkietalkie.big` 1,164× vs 2×). Finding 1.
- **`NtReadFile` is `kHighFrequency`** — invisible at plain L3. **A5 is the `.big` read
  oracle**, not A1 or A2. Finding 2.
- **The `.big` container format is cracked** — `docs/big-archive-format.md`. Should
  transfer verbatim to Case West.
- **There is no Bink in this game.** Movies stream through an in-house player reading
  `.big` cinematic archives. Finding 7; the Bink phase is deleted from the runtime plan.
- **The disc shader banks are NOT usable microcode** (retraction, finding 6) — but
  Xenia's `dump_shaders` gave us 455 microcode blob files — **335 distinct** shaders,
  A1's 120 being a strict subset of A2's — which is XenosRecomp's input, so the
  renderer was unblocked anyway.

## Current status

**The full per-session trail is `docs/port-history.md`; the open backlog is
`docs/open-items.md`.** Both were split out of this file on 2026-08-08. The
authoritative per-subject records are `docs/xenia-capture-analysis.md` (the numbered
findings ledger — it wins on any measured number), `docs/phase1-notes.md`,
`docs/phase3-notes.md`, `docs/phase5-notes.md` and `docs/d3d-translation-plan.md`.

Where the port is, as of 2026-08-16 (part 50 CLOSED — **THE PLAN'S TOP TWO ITEMS WERE
BOTH REPRICED BY THE MEASUREMENT THAT PRECEDED THEM, AND ONE OF THEM WAS THE PROFILER
MEASURING ITSELF.** `docs/part51-kickoff.md` is the LIVE hand-off;
`docs/perf-plan-part50.md` is still the live plan, but **read `phase5-notes.md` §6cg
BEFORE it** — §6cg retires two of its items and corrects every number in its budget):

* **`CZ_VK_PROFILE` COSTS 2-4 ms A FRAME, 8-18%**, and every figure in the plan's budget
  — including the operator's whole-map lap — was read from a profiled run, because that
  is the only way to get a phase split. **The operator's 28.3 ms / 35.7 fps at 5,000-7,000
  draws is really ~25-26 ms / ~39-40 fps in play.** Rankings are unchanged (every phase
  is inflated, not one); the distance is not — the plan's 20 ms intermediate is ~3 ms
  closer than it believed. Three runs an arm, two of four draw bands outside their own
  noise floor. **Never quote a frame time from a profiled run without saying so**: this
  project did from part 30 to 49 and could not have noticed, because a 32 ms pacing floor
  absorbs an 8% inflation without moving.
* **`other`'s RESIDUAL WAS THIS PROFILER, and the plan called it "the highest-yield-per-
  hour item in the document".** A `ProfScope`'s constructor clock read falls inside the
  PARENT's interval and nothing subtracts it, and `other` is DoDraw's outermost scope —
  so it could never have been named by splitting, because it is not in the code being
  split. Confirmed by a control that could have refuted it: `CZ_VK_PROFILE_EXTRA_SCOPES=8`
  moved it **205 -> 397 ns**, 24.0 ns/scope against a 21.6 ns calibrated read, and
  DoDraw's ~8 DIRECT children are 94% of it. **Retired: there is no frame time there.**
* **ITEM 1a IS SHIPPED AND IS WORTH ~0.3-0.5 ms, NOT 1.5-2.** A share is not a shape:
  "28.7% of packets are type-2 filler" is equally consistent with one huge run and with
  23,000 isolated dwords. Measured mean run **2.24**, bimodal, and **0% at ring level** —
  it is the title's own indirect buffers, not driver ring padding. That moved the fix from
  the callee (57% of calls) to `ExecuteLinear`'s loop (100%, and free, because the header
  is already fetched). The plan's 20-30 ns/packet prediction is **refuted**: 4.0-6.5 ns
  against a 9.4% null floor, the sign held by 3/3 rounds and 12,267 calls a frame removed.
* **ITS BY-PRODUCT IS WORTH MORE THAN THE ITEM**: the difference prices one
  `ExecutePacket` call at **24-40 ns**, a LOWER bound, so item 1c's ceiling is **~2.2 ms**
  — measured, not estimated. But **1c's top candidate is refuted for free**: hoisting the
  wrap modulo is worth nothing, because `INDIRECT_BUFFER` is only **43-46 packets a
  frame**, so ~45 buffers carry all ~75,000 packets and every one is fetched with
  `wrapDwords == 0`. 1c has no single lever; inlining the walk is a refactor, not a
  tightening.
* **ITEM 2a IS UNDERSTOOD AND IS NOT WASTE.** The guard's 26 MB/frame all comes through
  one door: `needsExact`, **unbudgeted and permanent**, at 388-483 streams a frame — and
  **15,643 of 126,536 store entries have latched it, 12.4% and rising monotonically**,
  refuting part 46's expectation that it would be "the UI text buffers and almost nothing
  else". **But the obvious fix is refuted too**: always-copying proven streams is cheaper
  AND safer by a clean argument, and one counter killed it — only **11-13%** of proven
  observations find a change, so the guard saves the copy on ~88%. The real question is
  whether a large buffer's change can be detected without reading it (soft-dirty page
  tracking), and that is architectural work with a correctness risk, costed in the kickoff.
* **WHAT PART 50 ACTUALLY DELIVERED IS ~0.4 ms, and the rest is a CORRECTION rather than
  a speedup.** Both headline numbers move the reported frame time the same way and must
  not be banked together: item 1a is a real −0.4 ms (though its own frame-time A/B read
  **+0.0%** in every draw band — 0.4 ms is under this route's noise), while the profiler's
  2-4 ms is time **the player never paid**, because nobody plays with `CZ_VK_PROFILE` set.
  Part 51 starts from ~25.5 ms at 7,000 draws, of which part 50 earned 0.4 (gotcha 337).
* **THE NULL FLOOR IS NOW MEASURED, NOT ASSUMED: 9.4% on ns/packet and 8-18% on frame
  time by draw band.** An item worth under ~1 ms is invisible in frame time on this route
  and must be settled on a per-unit statistic. Budget for that before picking an item.
* **Gates at close: ALL CLEAN**, E3 best of five **+0.8820**, 4 of 5 agreeing on layout.

Where the port WAS, as of 2026-08-16 (part 49 CLOSED — **THE 30 fps CAP IS GONE AND IT
WAS THE TITLE'S OWN SETTING ALL ALONG.** The operator has played the whole map at
`CZ_FPS_CAP=60`. ~~`docs/part50-kickoff.md` is the LIVE hand-off~~ — superseded by
`part51-kickoff.md`; `docs/perf-plan-part50.md` is still the live plan, built on that lap
— **but every frame-time number in that lap is inflated 8-18% by the profiler that
recorded it, so the fps figures below are LOWER than what the operator actually played;
see part 50 above**):

* **60 fps IS NOW THE DEFAULT**, on the operator's instruction; `CZ_FPS_CAP=30` is
  the control arm and reproduces the shipped pacing exactly. A player-facing
  option to choose is later work.
* **THE WHOLE-MAP LAP IS THE HEADLINE — 16,788 frames on their machine.** 62.5 fps
  below 3,000 draws, 43.5 at 3-5k, **35.7 at 5-7k where 60% of their play is**, and
  only **3.6% of frames below 30 fps**. Their words: *"seems to be working pretty
  well"*, and on the question that could have invalidated all of it, *"the game plays
  perfectly"*.
* **THE 30 fps WAS THE TITLE'S OWN D3D PRESENT INTERVAL**, traced end to end: config
  `0x82A57ACC` -> `sub_823C8D20` -> `sub_827CBB00` -> `dev+13804` -> `sub_82841AD0`
  -> `sub_82841878` -> the vblank walker. Reading it BACKWARDS is the finding — the
  game's own "vsync 1" setting produces interval 1, so **60 fps is a configuration it
  already ships with**, not a defeat of its pacing (which §6am forbids).
* **THE CAP WORKS BY SHORTENING THE VBLANK PERIOD, NOT THE INTERVAL.** Presents are
  vblank-quantised, so at 16 ms the ladder is 16/32/48 ms with NOTHING between and any
  frame needing 17 ms falls to 31 fps. The operator found that in minutes — *"when it
  is 60 fps the game plays perfectly"* but *"when it drops it still goes back to
  30"*. An 8 ms period with the title's own interval of 2 gives the same ceiling and
  half the rung.
* **IT DOES NOT DOUBLE THE SIMULATION**: locomotion p90 **0.99x** against a registered
  2.00x prediction, with a 31-39% null control bounding what that can rule out.
* **TWO MORE CEILINGS APPEARED THE MOMENT THE FIRST LIFTED**, which is the transferable
  half (gotcha 333). The host's vsync had been throttling us for 48 parts behind the
  guest's own cap — **headless read 62.5 fps the whole time and nobody asked why
  windowed disagreed** (332) — and the vblank ladder above.
* **FRAME TIME IS A USABLE INSTRUMENT AGAIN, for the first time since part 30**, and
  **the GPU is IDLE** (`submit.gpu` 0.0% median over 22 windows). Gotchas 237/238's
  pinned share was a workaround for a ceiling that is gone.
* **What is left, all CPU, all on their numbers**: the PM4 walk at **81,106
  packets/frame x 100 ns = 8.1 ms** (28.7% of them type-2 filler doing NOTHING); the
  stream guard still hashing **63-72 MB every frame** inside `rec.vertex` (part 47 made
  that hash 4x faster and not SMALLER); and `other`'s residual at 206 ns/draw, still
  unnamed after two splits.

Where the port WAS, as of 2026-08-16 (part 48 CLOSED — **THE PERFORMANCE TARGET IS MET
ON THE OPERATOR'S OWN MACHINE**: 33.6 ms and 29.8 fps at the spot they name as worst,
against the 33 ms the plan set, with no staleness reported. **`docs/part49-kickoff.md`
is the LIVE hand-off**, and its first action is a QUESTION — the standing "performance
is the most important" instruction has been satisfied and should not be assumed to
still hold):

* **THREE ARMS ON THEIR MACHINE, SOAKED AT ONE SPOT, ARE THE HEADLINE.** The camera
  stationary at the gas station in every arm, so the band check reads **0.0% drift**
  across 6,800-7,500 draws — the cleanest comparison this project has had on their
  hardware. **33.6 ms / 29.8 fps** default; **40.5 ms** with part 47's guard fold
  undone; **38.1 ms** with part 48's PM4-walk `getenv` undone. Against part 47's
  42.8 ms and 23.4 fps at 7,010 draws: **−9.2 ms, +6.4 fps.** Their picture verdict
  on the guards: *"Some looked wrong but they are not new"* — second consecutive
  clean session.
* **BOTH OF PART 48'S WINS WERE FOUND BY SPLITTING A PROFILER PHASE, NEITHER BY
  READING CODE** — three items in two parts now (gotcha 327). Splitting `other`
  found a `getenv` on the per-draw path; applying the gotcha written for THAT to
  `pm4.cpp` found a `getenv` **on the PM4 walk, once per type-3 packet, ~29,000
  times a frame**, worth **4.5 ms** on their frame (136 -> 95 ns/packet).
* **`Pm4_OpcodeCount` HAD BEEN COUNTED ON EVERY PACKET SINCE PHASE 4 AND READ BY
  NOTHING.** Printing it showed **`SET_BIN_MASK_LO` is the most frequent packet in
  the stream** (a third of all type-3, half again as many as there are draws) and
  **28.7% of packets are type-2 ring filler** doing no work at all.
* **AN ITEM CAN BE PERFECT ON ITS OWN COUNTER AND A NET LOSS** (gotcha 330). Item
  2b stamped the stream cache instead of clearing it: 97.7% node reuse, **45x fewer
  allocations**, and `record` **8.5% slower** — the map grew 1,900 -> 7,000 entries
  and 22,000 lookups a frame paid for 1,800 cheaper inserts. Built, measured,
  reverted the same day.
* **A NULL-CONTROL ARM IS WHAT MAKES A PER-DRAW A/B READABLE** (gotcha 331). The
  3,000-8,000 draw band is NOT narrow enough: `record` varies 1,204 -> 1,033 ns/draw
  across it. Put an arm in every campaign that CANNOT move the statistic; whatever
  it reads is the floor. It read +1.5-4.9% while the fold read **+99.4%**.
* **What is left, on their numbers**: the stream guard still hashes **63-72 MB every
  frame** inside `rec.vertex` (part 47 made that hash 4x faster and not SMALLER — attack
  the bytes); `oth.begin` is **0.75 ms a frame** for once-per-frame work; and
  `oth.residual` is **1.4 ms and still unnamed after two splits**.

Where the port WAS, as of 2026-08-16 (part 47 CLOSED — the PERFORMANCE work executed
and **CONFIRMED BY THE OPERATOR ON THEIR OWN MACHINE**: 64.1 -> 42.8 ms, 15.6 -> 23.4
fps at matched draws, with the picture unchanged. `docs/part48-kickoff.md` was the
hand-off and `docs/perf-plan-part48.md` the performance plan, built on
their frame rather than on the headless route):

* **THE OPERATOR'S OWN TWO-ARM A/B IS THE HEADLINE.** One binary, their route,
  the gas-station spot they name as worst for frame rate, three minutes an arm,
  matched on draw count: **64.1 -> 42.8 ms, 15.6 -> 23.4 fps, `textures`
  25.19 -> 4.45 ms.** Their words: *"performance is way better, still far from
  perfect"*, *"pretty much 10 fps difference"*, and — the question the fix could
  have failed — *"games looks pretty much the same as last time"*, with **no
  stale texture reported**. Headlessly the crowd frame lands on the two-vblank
  floor: at 5,000-8,000 draws 42-46 ms -> 32 ms and the 16 ms-PINNED share
  5-13% -> 73-85%, plus an 8,000+ draw band the old binary never reached.
* **THE BIGGEST SINGLE ITEM WAS THE TEXTURE REVALIDATION GUARD** and the fix is a
  CADENCE change, not a mechanism change: once per frame per cache entry instead
  of once per texture fetch per draw. **93.4% of checks skipped, 15.1x less
  hashing.** The redundancy factor WAS the size of the item and had never been
  measured — an estimate off run totals said 2x (gotcha 323).
* **THE SECOND WAS HIDING IN THE WRONG PHASE.** Splitting `record` (which had no
  breakdown at all) showed its vertex section was 70% of it — and the work there
  was the cross-frame store's CONTENT GUARD, **81.65 MB hashed in one frame of
  their session**, charged to `record` because `g_prof.streams` wraps only the
  copy. **Gotcha 238 contains that exact example and it took nine parts to act on
  it** (gotcha 326). The fold was then found to be LATENCY-bound, not
  bandwidth-bound: four accumulators took it **9.0 -> 35.7 GB/s**, same bytes,
  with a single-bit sweep confirming 0 misses on both folds (gotcha 324).
* **THEIR WORKLOAD DIFFERS FROM OURS IN KIND, not just in size**: 144 ns per PM4
  packet against our 110-113, and **7.8 register dwords per packet against 9.4**.
  Part 47's bulk register path buys them less than it bought us, and per-PACKET
  cost dominates their walk — so quote walk changes as ns per packet, and rank
  against their budget.
* **Method, and it cost real time to learn**: a phase SHARE moves when any other
  phase does, so quote MILLISECONDS (320); pooling profile windows across a route
  measures the route and calls it noise (321); a gate that would pass whether or
  not your change is correct has not tested it, and the code you replaced is
  still compiled in and is the oracle (322); a counter nothing reads is not an
  instrument (325).
* **OWED into part 48**: the operator's confirmation of the guard fold (unmeasured
  on their machine); an isolated A/B of the vertex/index bind cache, which is the
  one part-47 change never measured alone and whose sign is consistently
  unfavourable; and item 1.1's registered claim, half-answered.

Where the port WAS, as of 2026-08-16 (part 47 mid-part — the plan's tiers 1 and 2
executed and its top item repriced):

* **THE TEXTURE REVALIDATION GUARD WAS NEARLY THE WHOLE TEXTURE PHASE, and the
  plan's own named first run proved it in one measurement.**
  `CZ_VK_NO_TEX_REVALIDATE=1` on the outdoor route: `textures` **15.9 ms with
  the guard and 2.3 ms without**, against the 8-11 ms the plan priced. In the
  5,000-8,000 draw bin the frame goes from **47-48 ms at 23-24% pinned to
  32-33 ms at 67-94% pinned** — onto the title's own two-vblank floor. That arm
  is the upper bound and not a configuration; it is the defect part 38 fixed.
* **THE FIX IS A CADENCE CHANGE and the A/B says it recovers essentially ALL of
  that**: the content guard runs **once per frame per cache entry** instead of
  once per texture fetch per draw. Three runs an arm, same binary, both negative
  controls reading exactly zero: **at 5,000-8,000 draws the frame goes 42-46 ms
  -> 32 ms and the 16 ms-PINNED SHARE goes 5-13% -> 73-85%.** The crowd frame
  stops being CPU-bound and becomes PACING-bound, and the binary reaches an
  8,000+ draw band the old one never got to, at 36-37 ms. `textures` 17.18 ->
  **2.47 ms** (the no-revalidate upper bound was 2.3); the guard reads 5.8-7.4
  MB/frame against 77.9-95.1. `CZ_VK_TEX_GUARD_EVERY_FETCH=1` is the arm.
* **Item 1.1's registered claim — "`changed` must not fall" — holds on the event
  rate and is UNRESOLVED on the distinct-address measure.** Per frame the fix
  detects slightly more (0.0739 against 0.0640, ranges overlapping). But the
  every-fetch arm sees more distinct addresses ever change (157 against 141)
  while the part-47 runs covered *more* ground, and the two arms do not visit
  the same places — so that is confounded and this route cannot settle it. It is
  the second question for the operator, not a closed item.
* **`outside` and `record` read slightly WORSE on the part-47 arm and that
  comparison is INADMISSIBLE**, not bad: the arms do not submit the same command
  stream and their packets-per-frame differ by 40%, so a matched DRAW band does
  not match a PM4 workload. The admissible statistic for the walk is cost per
  packet — **110-113 ns against 151-158, zero overlap over nine windows an
  arm**.
* **The PM4 walk writes register RUNS in bulk** — the two range questions asked
  once per run instead of once per dword — and it is verified against the
  per-dword code it replaced, because **both PM4 boundary oracles pass
  identically whether or not that rewrite is correct** (gotcha 322): **0
  mismatches over 152,020,384 dwords**, `CZ_PM4_VERIFY_POISON=1` first to show
  the check can fail, and **100.0% of dwords take the bulk path**.
* **The state cache now covers the VERTEX and INDEX binds.** Part 18 added the
  counters and deliberately did not act on them until the repeat rate justified
  it; it does — **51.0% / 39.4% over 16.17 M draws** on the operator's session.
* **Three ways a perf A/B on this title reads wrong, all of which bit in one
  afternoon**: a phase SHARE moves when the other phases do, so taking 13 ms out
  of `textures` made four other phases read 34-68% worse without moving (quote
  MILLISECONDS, gotcha 320); pooling profile windows across a route measures the
  ROUTE and calls it noise, a 58% "floor" that was a safehouse window averaged
  with a crowd window (use a matched draw band, 321); and `msec` is the LAST of
  the eighteen `.stats` columns. `tools/part47_perf_read.py` does all three.
* **What is OWED is the operator's own session** —
  `tools/part47_operator_session.sh`, two chained arms — because the headless
  route understates their draw path by ~2x, so a headless win here is not the
  conservative direction. Ask two things: is it faster, and does any texture
  ever look STALE.

Where the port WAS, as of 2026-08-16 (part 46 CLOSED — both of the operator's items
answered, one of them FIXED, and the performance work now has a plan built on the
operator's own profiled frame):

* **THE UI TEXT / HUD DEFECT IS FIXED AND OPERATOR-CONFIRMED** (open items
  00c/00k, first seen part 24). Their words: *"Ui stay good the whole time"*, then
  *"Hud stay good and all"* on the cheaper variant, against a control arm
  (`CZ_VK_NO_DYNAMIC_GUARD=1`) that broke in the same session. **"Raise the guard
  bound" was REFUTED by measurement** — at 256 KB the HUD still dropped out, and
  since part 45's unlimited arm fixed it the UI buffer is above 256 KB, where the
  size histogram prices exactness at 121+ MB/frame. **Size was the wrong
  discriminator.** The fix is exactness EARNED per stream: a stream the store
  catches CHANGING is hashed exactly, and one the cheap sampled guard is proved
  able to see is demoted back. `CZ_VK_GUARD_BUDGET` is the default;
  `CZ_VK_NO_GUARD_BUDGET=1` is the control. `phase5-notes.md` §6cc addendum.
* **THE TREE SHARDS ARE MISSING ALPHA-TO-MASK COVERAGE, and the setting to ship is
  `CZ_VK_A2M_MODE=1`.** Canopy draws are alpha test GREATER at `RB_ALPHA_REF =
  0.0` plus A2M over a DXT4/5 albedo: at ref 0 the alpha test keeps everything, so
  A2M does the whole cutout alone, and we declined to emulate it on an excuse that
  is false exactly at ref 0 (gotcha 317). Mode 1 (flat 0.5 threshold) removes the
  hard black plates with no screen door; mode 2 (the faithful per-sample dither)
  screen-doors, because the foliage is on a **2x MSAA** surface — confirmed on
  hardware's own traces — and our EDRAM stand-in is not at sample resolution. The
  operator's near-matched A/B: isolated-pixel share **0.71% (mode 1) vs 4.17%
  (mode 2)**, hardware 0.00%. §6ca + addendum, §6cc, open item 0t, gotchas 317-319.
* **THE PERFORMANCE PLAN IS `docs/perf-plan-part47.md`**, written against the
  operator's own frame — **61.7 ms at 7,231 draws, target 33 ms** (the 360 shipped
  this at 30 fps). Budget: **textures 26.5 ms, PM4 walk 14.2, record 10.9**, GPU
  34% utilised with `submit.gpu` 0.0, i.e. a pure CPU problem. **The headline is
  that the texture revalidation guard read 366 GB over one session — 92.9 MB/frame
  — to catch 986 real changes out of 26.8M checks (0.0037%)**, and its upper bound
  is knowable in ONE run (`CZ_VK_NO_TEX_REVALIDATE=1`). Tiers 1+2 are ~21 ms with
  no architectural change.
* **THE HEADLESS ROUTE UNDERSTATES THE OPERATOR'S DRAW PATH BY ~2x** (28.7 ms at
  5,241 draws against their 53.9 ms at 5,080). Part 46's three "exonerated"
  performance suspects — part 45's interpolant liveness, part 41's per-fetch
  samplers, part 44/45's mip uploads — are cleared on THAT route and not on the
  operator's. Wire `CZ_VK_PROFILE` and `CZ_VK_FRAME_STATS` into every operator
  launch; part 46's first session shipped without them and wasted itself.
* **The part-26 white-prop class is CLOSED on the operator's word** — newspaper
  boxes, register, gas-station sign, bathroom window all correct after part 45.
* Methods worth keeping: the DEFECTIVE-and-CORRECT-pixels draw-ID cut retires
  every per-draw input in one measurement (gotcha 318); prefer a repro that
  already has an oracle in the repo (319); and **read the pose before the
  picture** — two operator captures were compared per-pixel because they looked
  like the same view and their eyes were 250 units apart.

**Older per-part status blocks (parts 28-45, the superseded mid-part-44 closure
and the superseded MID-PART-46 block) moved to `docs/port-history.md`** — CLAUDE.md keeps
only the live part and one part back, per the 2026-08-08 split's rule.

* **The recompilation is clean and has been since phase 0**: 57,808 functions, 228 TUs,
  zero unrecognized instructions, zero dropped branches, zero unlowered switch
  dispatches, silent recompiler.
* **The game boots, renders and PLAYS.** A headless run reaches the title screen, the
  menus, the prologue and live gameplay with no operator (the recipe is in Commands).
  The operator has played Still Creek: combo weapons, the Zombrex tutorial, cinematics.
* **Gates, both arms:** `--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**;
  `truncated=0`; `no translated shader` = 0; deepest file on a no-input boot
  **#83 `cinezombie.big`**;
  both PM4 capture oracles clean; the picture matches capture E2 at **+0.9597**
  correlation, identity orientation. **A1's strict-prefix gate is BIMODAL** — the benign
  position-71 three-name interleave of gotcha 221 shows on some runs and not others, and
  `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` reduces it rather than removing it (part 18
  said "restores" on the strength of one run). Quote A5.
* ~~**Performance: ordinary gameplay is 31 fps and CLOSED** — that is the title's own
  two-vblank pacing and it will not go higher.~~ **RETRACTED IN PART 49.** It was true
  of the shipped CONFIGURATION and false of the title. The 30 fps is the title's own
  D3D present interval, traced end to end from its config global to its swap
  scheduler, and **a 60 fps mode is a configuration the game already ships with**
  (`CZ_FPS_CAP=60`). The operator has played the whole map on it: 62.5 fps below 3,000
  draws, 43.5 at 3-5k, 35.7 at 5-7k, and only 3.6% of 16,788 frames below 30 fps. The
  rest of this bullet stands. **Crowds are the open item and are
  CPU-bound in our runtime**: 75% of a crowd frame is the renderer's draw path and the
  PM4 walk. `docs/perf-cpu-plan.md` is the plan; item 0 is closed (the headless recipe
  reaches the outdoor world at 6,400-8,700 draws a frame) and **part 20 re-measured the
  rest, because the profiler was counting nested phases twice and the plan's ranking of
  §1 was built on the result** (gotcha 228). Corrected, at ~6,800 draws: draw path
  19.9 ms (`record` 6.7, `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3),
  PM4 walk 11.8 ms. Taking the instrumentation off the per-draw path is **−11.0% of a
  crowd frame**, three runs an arm with no overlap. **The PM4 walk is a register-write
  loop — 90,316 packets a frame carrying 815,020 register dwords at 15.3 ns each** — and
  it is the biggest untouched term.
  **The stream cache is CLOSED as of part 22** — the cross-frame store (§6av) takes
  `streams` from 11.1% of a crowd frame to **0.0%**, copied bytes from 61-66 MB/frame to
  0.23, for a net 3.3-4.0 ms after its content guard.
  **The noise floor here is 10-13% at one run a side** (gotcha 229): use
  `tools/frame_perf_bins.py`, three runs an arm, alternated, and run the null comparison
  first. A real A/B on this workload is an hour of wall time.
  **AND `frame_perf_bins.py` REPORTS MEANS, WHICH ON THIS TITLE MEASURE THE PACING FLOOR
  RATHER THAN YOUR CHANGE (gotcha 237).** It scored the store at +1.7% against a +1.3%
  null; read as medians the same data is **44 ms -> 32 ms at ~3,700 draws**, and the
  decisive statistic is neither — it is the share of frames within 1 ms of a 16 ms
  multiple, which goes **10% -> 97%**. A CPU saving converts to frame rate only where the
  frame is above one vblank floor and within reach of the next, so quote the pinned share.
  **DO NOT PIN THE GPU CLOCK — gotcha 219 is retracted in part.** The P8/210 MHz this
  project quoted for five sessions was an overnight session with the MONITOR ASLEEP.
  Awake, this workload governs itself to **P5, mean 524 MHz, 32% utilisation, 28.6 W**,
  which is where `vkcube` sits on the same machine. Sample and quote with
  `tools/gpu_clock_sample.py`. **The 32% is the real finding**: the GPU is idle 68% of
  every frame because `SubmitAndWait` blocks straight after submitting, so our CPU and
  GPU never overlap. Overlapping them is ~22 -> 36 fps at the SAME power and is now the
  largest item in the plan (gotcha 231, `docs/phase5-notes.md` §6ar).
* **The view-dependent whole-frame black is SOLVED** and was the renderer's per-frame
  bump arena overflowing at a fixed 128 MB against a 161 MB peak, which lost the whole
  post chain and presented black over a correctly rendered scene. 160 black frames of
  8,216 at 128 MB, zero at 512, and every one of the 160 the frame after an exhaustion.
  It grows now. `docs/phase5-notes.md` §6ap.
* **SAVE AND LOAD BOTH WORK — a closed round trip, and the first title state in this
  port that survives a process exit** (`docs/phase3-notes.md` finding 52). The file is
  303,104 bytes with bytes 4..31 identical to the real 360 save A3 shipped. Three
  defects, each of which alone was enough: `NtCreateFile` honoured no disposition,
  `NtWriteFile` was a stub, and the VFS cached negative lookups so a created file could
  never be re-opened. The LOAD needed a fourth — xam ordinal `0x271`,
  `XamContentCreateInternal`, which `kResolvable` refused because A1's list of resolves
  was captured with an empty save root.
* **CUBE MAPS ARE BOUND AS OF PART 25 — and that is about half the item by volume.**
  Found in part 23, built in part 25. 92 of the cache's 397 shaders sample set 2
  (`TextureCube[]`) and every one of them read descriptor index 0, the 1x1 white dummy, on
  every draw from phase 5 until now. The sidecar now carries each fetch slot's dimension,
  cube maps upload as six faces into a `VK_IMAGE_VIEW_TYPE_CUBE` view in set 2, and
  `CZ_VK_NO_CUBE=1` is the same-binary control arm. **The fetch constant's own dimension
  field was located by CENSUS (`CZ_VK_DIM_CENSUS=1`), not from memory, which had it one
  field off** — dword5 bits 9..10, cross-checked against dword2's stack depth reading 5 for
  every cube fetch (gotcha 244). `docs/open-items.md` item 00 and `phase5-notes.md` §6ay;
  three competing theories died in part 23's census and are recorded there.
  **THE SECOND HALF LANDED IN PART 26: the cube map the title RENDERS ITSELF is now
  assembled from its six resolve snapshots** — `06805000`, six faces at
  `base + i * 0x4000`, copied into a six-layer `VK_IMAGE_VIEW_TYPE_CUBE` image in set 2 and
  refreshed by each face's own resolve (8,850 face refreshes in 240 s, because the title
  re-renders it continuously — a one-shot fill would freeze the world's reflection).
  **The face layout was printed face by face and could have refuted the stride model; six
  of six filled.** `358,767 of 999,508 cube fetches (35.9%)` on the outdoor route now read
  it where all of them read the white dummy before. `CZ_VK_NO_CUBE_SNAPSHOT=1` is the arm.
  **What is still owed is the operator's verdict**, and it is now a three-way question
  (rendered cube / white / no cube at all). Know the headless answer first: part 25's
  four-config block put the magenta positive control at **12.7x its null**, so the
  instrument is NOT blind and binding real cube maps changes nothing measurable in the
  safehouse and prologue — both surviving explanations put the effect OUTDOORS.
* **A CLASS OF WHITE-SURFACE DEFECTS IS OPEN, IT IS OURS, AND SEVEN EXPLANATIONS ARE
  ALREADY REFUTED** (part 26, `docs/open-items.md` 00f/00g). White ground patches, white
  props, blown-out glass and windows — **Xenia renders all of them correctly.** Refuted by
  measurement: the tone map, a missing texture, constant UVs, the white dummy (all four
  heaps poisoned magenta), the clear colour, the EDRAM surface format, a flat-decoding
  texture. **Do not re-buy any of them.**
  Round-2 captures (`Xenia logs/R2_world/`, seven self-contained single-frame traces, read
  with `tools/xtr_draw_bindings.py`) then established: our shader coverage is complete
  (357/357); the ground draw matches hardware on shader, textures, texture CONTENTS and
  render state; and our cube declines fire on a shader-versus-constant disagreement
  hardware never shows.
  **PART 27 CLOSED THE LAST INPUT AND THE ANSWER IS THAT THE GROUND DEFECT IS IN THE
  SHADING.** `tools/xtr_draw_vertices.py` reads hardware's own vertex streams and shader
  constants for a named draw; the 25,234-vertex ground draw agrees on **all five vertex
  attributes to the printed digit** and on every constant the capture can reconstruct. The
  recorded "two texcoords decoding identically" anomaly **is what hardware does too**, and
  item 00f's "drawn twice with mask=F" lead was **TILING** — the two draws' scissors are
  `0,0 640x720` and `640,0 640x720` (gotcha 265). Read our translated
  `ps_ad65b98593f95926` against the capture's disassembly of it next.
  **On the cube declines, part 27 kept the conclusion and replaced the measurement**:
  "414 of 414" counted only slots already reading cube, so it could not have found a
  disagreement (gotcha 264) — `tools/xtr_cube_agreement.py` asks it per declared fetch slot
  and gets 0 of 13,203. The disagreement is ours, it is **9 enumerated (shader, slot,
  texture) cases**, and at those draws our slot 4 holds an **exact duplicate of slot 3**
  where the captures show hardware holding a real 128x128 DXT1 cube map for the same shader
  pair. **The magnitude was off by 10x and had a second, larger cause**: on the outdoor
  route the dummy is served to 3,210 of 1,903,592 cube fetches, **2,182 because the guest
  never set that slot at all** and 1,028 for the disagreement.
  **AND THE `.xtr` TOOLS WERE MISSING `LOAD_ALU_CONSTANT`** — 620 packets against 36
  `SET_CONSTANT`s in one capture, so every constant they reported for hardware was a zero
  that meant nothing (gotcha 262), and 81 of those 620 read memory the trace does not
  carry, which the tools now report as `UNRECOVERABLE` rather than as a stale value
  (gotcha 263).
* **OUTDOOR PICTURE CLAIMS ARE POSSIBLE AS ERA AGGREGATES AND IMPOSSIBLE AS MATCHED
  FRAMES — measured in part 26, and the route was never the problem.** Two arms are
  comparable only where `drawFingerprint` AND `cameraFingerprint` agree; run that filter on
  two runs of ONE configuration on the DebugJump route and it yields **422 of 13,056 frames,
  none above 141 draws, and 0 of the 12,174 outdoor frames** (`tools/frame_determinism.py`).
  The route is fine — 93% of its frames are outdoors and two runs' draw counts agree to
  1.4% — but a crowd of animated actors never renders the same draw list twice, so exact
  equality selects for stasis (gotcha 254). **The replacement is
  `tools/frame_era_medians.py`**: era medians over every frame above 1,800 draws, with the
  null measured from the same pair — **0.94% on mean luma, 0.76% on distinct colours**,
  while coverage saturates at 99.67% and can report nothing. That is what unblocks items
  00, 3, 4 and 6 (`docs/measurement.md`).
* **Ordinary gameplay is ~30 fps and the CPU/GPU now OVERLAP** (part 23): the fence wait
  fell from 31.5% of a crowd frame to 0.2% with `CZ_VK_FRAMES_IN_FLIGHT=2` (default; `=1`
  is the old renderer, same binary). The binned frame-time A/B is still owed.
* **THE HUD / AMMO DEFECT IS FIXED** (part 24, open item 00c). It was our own
  cross-frame stream store: its guard was exact only to 512 bytes and sampled 8x64
  above that, so a small edit inside a batched multi-KB UI vertex buffer hashed
  identical and the store served the previous frame's numbers. The exact bound is now
  16 KB (`CZ_VK_STREAM_GUARD_BYTES=N`), the HUD is correct in a gas-station crowd, and
  it costs **zero frame rate** — 32.2 ms at 6,778 draws, still the two-vblank floor.
  Gotchas 242 and 243 are the transferable halves.
* **The renderer's remaining picture defects** are the shadow cascade, NPC part meshes,
  mipmaps and the colour-grading LUT — all in `docs/open-items.md` with the measurement
  for each, and all worth re-testing now that a frame losing its post chain is no longer
  contaminating the evidence.

**The five-tool recompilation pipeline must run in this order**, re-running the
recompiler between each, because each one's evidence is only valid against a current
`ppc/`:

```
find_jumptables.py  ->  coverage_to_function_overrides.py  ->
    fix_switch_function_bounds.py --apply  ->  find_dropped_branches.py --prune / --widen
    ->  find_unlowered_switches.py
```

The last is a **gate, not a repair** — exit 1 means a real defect (gotcha 53).

## Reusability: what gets extracted, and when

**`docs/reusability.md`** — the tier list for what transfers to Case West and what does
not, and the two rules governing it: **extract only what is proven in BOTH ports, never
from Case Zero alone**, and **extract after the second implementation forces the seam, not
in anticipation of a third**. Do not build the Case West abstraction before Case West
starts.

## Conventions (same as the two template ports)

- No copyright/license headers in new files (user's own repo — ask before adding any).
- **Commit proactively** — whenever a change is useful on its own or important
  information was learned. End commit messages with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Document everything** in `docs/` for an outside reader — findings, dead ends,
  formats, retractions. Write it so someone porting a *different* Xbox 360 game can lift
  the technique: say what the idiom or format was, not just what we changed. That has
  concrete value here, because Case West is next and will read these documents.
- **Comment code for humans.** Every tool opens with a docstring answering *why it
  exists* — what went wrong without it — not just what it does. Inline comments explain
  the non-obvious bit-twiddling, the reason a scan runs forward instead of backward, and
  every deliberate exclusion. Generated config files carry a header saying which tool
  produced them and how to regenerate.
- **Retract in place.** When a stated finding turns out to be an artifact, say so where it
  was claimed and explain the artifact.
- Measurement discipline from day one: A/B with same-binary arms, gate comparisons,
  pre-register capture questions.

### Evidence rules (non-negotiable)

- **Measure before inferring.** A hypothesis about guest behaviour is tested against a
  census over the image, the shader bank or the capture — never argued from
  documentation or from model knowledge. **Report counts, not impressions.**
- **One change per experiment.** Fixes with distinct predictions land in separate
  commits and are verified separately.
- **State the prediction before running it.** Every fix commit records the falsifiable
  claim it makes about on-screen behaviour or dumped state, so a run can refute it.
- **A/B ADMISSIBILITY.** Two configurations are comparable at matched indices only if
  they are two states of ONE renderer producing the SAME draw set. If one arm renders
  less — or more — the comparison is inadmissible; say so and fall back to within-run
  evidence. This is the rule that would have caught `CZ_VK_FORCE_COLORMASK` in phase C
  part 9: the arm adds draws, so its picture cannot be compared with the baseline's at
  all, and it took a counter to settle what a picture never could.
- **Refutation by compensation beats refutation by absence.** When a mechanism is real
  but compensated somewhere else, record BOTH — that closes the branch properly, where
  "we looked and saw nothing" leaves it open. Phase 5 §6n is the worked example.
- **An untrusted path is not an oracle.** Only diff against a subsystem that has itself
  been validated, and re-ask that question whenever an upstream defect is fixed
  (gotcha 172). Case Zero has no in-project second implementation to diff against, so
  the substitutes are (a) Xenia traces, read for the FIRST divergent operation rather
  than the visibly broken object twenty frames later, and (b) the Fable 2 port for
  anything in the shared decode layer.

### Things not to do

- **Do not extract a library from this port alone**, and do not build the Case West
  abstraction before Case West starts. No interfaces or library splits "for later".
- **Do not bundle independent fixes into one commit.**
- **Do not treat documentation or prior model output as ground truth over a census** —
  including this file. Every number here was measured once and has a shelf life
  (gotcha 13).
- **Do not add speculative Xenos coverage.** An unsupported packet, format or import
  fails LOUDLY with its identifier; it never guesses (gotcha 5).
- **Do not copy external code before its licence is recorded.**
- **Do not delete the PM4 command processor.** See the note below — it is the control
  arm, not legacy.

### A conflict with the external "Project Constitution", recorded so it is not re-litigated

An outside constitution document describes this port as *"API-level HLE — a D3D
translation layer. There is no command processor in this project and none will be
added."* **That is not this repository.** `runtime/gpu/pm4.cpp` is a live PM4 command
processor: it is the boot engine (phase C's D3D arm still needs it for the ring and the
GPU/CPU hand-off), and more importantly it is the **same-binary control arm** for every
claim the D3D arm makes — the discipline the last nine sessions are built on. Phase C
part 9's four renderer fixes were all found on the PM4 arm because it is faster and
better instrumented. The constitution's *intent* — the D3D translation layer is where
the port is going — is right and is exactly `docs/d3d-translation-plan.md`; its
statement about the CP is wrong about the code and would, if acted on, delete the
ability to A/B.

Two more of its claims are superseded by measurement in this repo and should not be
re-derived: this title's tiles are **left/right 640-wide halves**, not horizontal bands
(window scissors `0..640` and `640..1280`, window offset `-640`), and its
"force a single tile, ignore predication" diagnostic HAS been run — it is
`CZ_PM4_NO_PREDICATION=1`, and it is destructive rather than diagnostic (phase5-notes
§6v).
