# Port history: what each session established

**Split out of `CLAUDE.md` on 2026-08-08.** The per-session narrative from bootstrap
through phase C part 18, in order. `CLAUDE.md` keeps only the current state; this is the
trail, including the retractions, which is the part that stops a later session
re-deriving something that was already tried and refuted.

The authoritative per-subject records are elsewhere and are not superseded by this:
`docs/xenia-capture-analysis.md` (the numbered findings ledger, which wins on any
measured number), `docs/phase1-notes.md`, `docs/phase3-notes.md`,
`docs/phase5-notes.md` and `docs/d3d-translation-plan.md`.

**Bootstrap + round-1 analysis complete (2026-08-04, session 1).** Package unpacked, XEX
identity established, ladders cross-checked, 232 jump tables recovered, round-1 captures
delivered and analysed, forwards coverage oracle applied.

**Phase 0.1 complete (2026-08-04, session 2).** All 42 unrecognized-instruction sites
closed, plus a seventh mnemonic (`vadduws`) that was "implemented" against a nonexistent
simde intrinsic and could never have compiled. A previously unmeasured defect class —
**dropped direct branches** — was found and driven to zero (finding 13).

**Current image: 57,808 functions, 228 TUs, 155 MB — zero unrecognized instructions, zero
undecodable instructions, zero switch-boundary errors, zero dropped branches, zero
unlowered switch dispatches.** The recompiler log is completely silent. (Was 57,822
before session 5: recovering the two missed jump tables let 14 coverage-injected case
labels and mid-body fragments be removed, which is a correction, not a loss.) Reasoning behind the bootstrap numbers:
`docs/bootstrap-2026-08-04.md`. Behind the capture-derived ones:
`docs/xenia-capture-analysis.md`.

The pipeline is now five tools that **must run in this order**, each re-running the
recompiler in between, because each one's evidence is only valid against a current `ppc/`:

```
find_jumptables.py  ->  coverage_to_function_overrides.py  ->
    fix_switch_function_bounds.py --apply  ->  find_dropped_branches.py --prune / --widen
    ->  find_unlowered_switches.py
```

The last one is a **gate, not a repair**: it asks the image which `bctr` sites look like
a table dispatch and are missing from the switch TOML, which is the only question
`find_jumptables.py`'s own output cannot answer (gotcha 53). Exit 1 means a real defect.

**Phase 0.2 complete (2026-08-04, session 2).** `runtime/` exists. All 228 TUs compile
and link — **0 errors, 0 warnings, 89 s on 16 cores** → 155 MB `libppc_image.a`, 109 MB
`cz_smoke`. The gate binary walks all 58,303 `PPCFuncMappings` entries and validates
them; the binary contains all 57,822 guest functions and all 244 imports with zero
undefined symbols.

**Phase 0.3 complete (2026-08-04, session 3): the `.xtr` decoder exists and finding 10 is
closed.** `tools/xtr.py` (the format, in one module) plus `xtr_walk.py`,
`xtr_pm4_census.py` and `xtr_determinism.py`. Format and method: `docs/xtr-decoder.md`.

Measured: B1 and B1b are **content-deterministic to 0.42%** over the boot+movie prefix
(0.19% on draws), with four eras agreeing to the individual draw — but **frame-exact
agreement is only 80.0%**, so phase 4 must gate on per-era aggregates, never on frame
index (gotcha 38). Both captures are intact: clean heads, zero desyncs. The census
self-check found `INDIRECT_BUFFER` is recorded one dword short (gotcha 39), which is a
trap phase 4 would otherwise have hit at replay time.

**PHASE 0 IS COMPLETE.**

**Phase 1 in progress (2026-08-04, session 4): the guest boots, and the GPU is real.**
`docs/phase1-notes.md` is the record; read it before continuing.

The recompiled image runs under our runtime, brings up TLS, threads and the loader
seam, reads files off the package, and **drives a live PM4 command processor**. Over a
25 s run: 1.27 M packets parsed, 563 XE_SWAP frames, 68,588 draws, 1,235
command-processor interrupts delivered to the guest ISR — with **zero unknown opcodes,
zero parser stalls and zero out-of-arena stores**, and the read pointer chasing the
write pointer rather than frozen.

- **`--xenia A1` (masked): an exact 84-deep prefix of Xenia's 93**, stopping before
  `XamShowDeviceSelectorUI`. Position 84 is `MmMapIoSpace`, the XMA context mapping.
- **`--xenia A5 --include-high-frequency`: tracks A5 to position 119, its last, with
  ZERO real mismatch windows** — `SET MATCH: every mismatch is a permutation. Exit 0.`
  The two surviving windows are permutations of one name set each, i.e. thread
  scheduling (findings 35-36). First fully clean A5 gate this port has produced.
  **Gate at 90 s, not 30 s**: at 30 s the run stops around 114 and the XMA path looks
  blocked when it is merely slow (gotcha 75). Even at 90-150 s reaching position 84 is
  usual, not guaranteed — 5 of 7 runs this session — because how far a multi-threaded
  boot gets in fixed wall time is a distribution, not a fact.
- 155 of 244 imports real, 89 generated honest-failure stubs.
- `cz_runtime --smoke` still passes: the phase 0.2 link gate is intact.
- **Stability: 0 crashes in 8 runs at 25 s, and 0 in 8 with `CZ_NO_AUDIO_PUMP=1`** —
  the audio pump's own control arm, same binary, same session. Read it as "no
  measurable difference", **not** "the pump is safe": 8 runs cannot see a 1-in-20
  fault, and the known surviving crash is around that rate (gotchas 50-51). The
  dominant fault — the "null-pointer walk on the main thread", 6-7 in 10 — was the
  unlowered `bctr` of finding 27 and is gone; the poison indirect call on the
  *graphics* pump thread is declined. `runtime/cpu/crash_report.cpp` prints the guest
  state on any fault.

Three things from this session worth carrying to Case West, all in
`docs/phase1-notes.md`:

- **The async completion is an APC, not an event** (finding 20). Our `NtReadFile`
  filled the buffer, filled the `IO_STATUS_BLOCK`, returned success — and hung the
  boot, because the engine passes `event = 0` and signals through `apcRoutine`.
  Every observable looked correct. Only A5 shows it; `NtReadFile` is `kHighFrequency`.
- **The ring geometry is derivable from the guest's own arithmetic** (findings 22-23).
  `28 - clz(size)` in front of the call proves `size = 1 << (arg + 3)`, and the single
  `CP_RB_WPTR` store gives the device-struct offsets. No constant needed inheriting.
- **Xenia's physical addresses carry a +0x1000 skew** (finding 24), so a physical
  address in our log is 0x1000 below the same object's in a capture. Any geometry
  argument mixing the two conventions is wrong — this one briefly manufactured a
  ring-buffer overrun that did not exist.

**The dominant crash is fixed (2026-08-04, session 5): it was not a null pointer.**
`docs/phase1-notes.md` finding 27 and `docs/xenia-capture-analysis.md` §15. A `bctr`
inside `sub_82955780` had never been lowered to a `switch`, so the function returned
without its epilogue and its caller resumed with the callee's `r31`. Finding 26's
diagnosis ("something we return is null where an object is expected") is retracted —
no kernel call was involved. Two such sites existed image-wide; both are fixed, the
image now carries **234 switch tables (was 232)**, and `find_unlowered_switches.py`
reports **0 defects, 2 benign tail-call thunks**.

Measured: **6-7 crashes in 10 runs -> 1 in 20.** The pipeline is clean end to end —
silent recompiler, zero dropped branches, zero `// ERROR`, `--smoke` passing.

**The frontend was waiting for input, and that is now measured (session 6).**
`docs/phase1-notes.md` finding 37. A healthy run reaches the **title screen** — 64
files through to `prologue_menu\prologue_z01.big` — the print cap, corrected in part 6
to 84 files ending at `skeleton\cinezombie.big` (gotcha 109) — rendering at ~34 fps and ~1,982
draws/frame against A1's title-screen ~2,540 — and sits there. Supplying one synthetic
START press (`CZ_FAKE_START_MS`, a measurement arm, never on for a gate) advances the
A1 gate from an 84-deep prefix to **85**, `XamShowDeviceSelectorUI`, five log lines
after the press. 2 of 2 conclusive pairs; the runs that reached the title screen
without a press did not advance. It then stops at position 86 on the phase 2
save-data enumerate stubs — a gap we chose.

Two things fell out. The standing note ("reaches `mainmenu.tex` and stops") had gone
stale and would have sent someone hunting a file-loading bug that no longer exists
(gotcha 79). And about a third to a half of long runs never reach the title screen at
all, stalling with the main thread parked in the renderer's frame fence — a real
defect, traced end to end in finding 38 below.

**The load stall is traced end to end (session 7) and FIXED (session 8) — and it was
our own kernel, not the parser.** `docs/phase1-notes.md` findings 38 and 39.

The chain finding 38 established: the main thread waits on the render fence; the
**Draw Thread** is in no kernel wait at all, spinning in guest code (`sub_8283C6C8`
under `sub_82845160`) on a ring-progress counter; that counter is advanced only by
`EVENT_WRITE_SHD` fence packets in the title's own command stream; and our walk of the
indirect buffers those fences close **stops early**, silently, on data it reads as a
packet header. Drop the last packet and a thread waits for the life of the process.

Finding 39 is why the walk desynced, and it is nowhere near `gpu/pm4.cpp`. **`VdSwap`
wrote 12 dwords of a 64-dword reservation and left the other 52 alone.** The caller
advances its write pointer by the whole reservation regardless (`addi r11,r29,256`,
`r3` never read), so those 52 dwords were submitted to the command processor as
packets — and since command buffers are recycled, what was in them was the *previous
frame's command stream*. Hardware fills them with 52 × `0x80000000`, a type-2 one-dword
no-op; the count is confirmed by two independent witnesses, the guest's own `addi` and
B1's 43 swap-carrying indirect buffers (52 every time).

Measured, same binary, arms alternated, `CZ_NO_SWAP_PAD=1` as the control:
**truncated indirect buffers per 120 s run went from up to 2,945 to ZERO**, and the
stall with them.

Three method notes worth more than the fix:
- Two capture-derived gates said the parser was correct — packet lengths against
  hardware's boundaries on 24,527,474 packets, and every indirect buffer's start
  address and internal boundaries chained on all 28,726
  (`tools/pm4_indirect_walks.py`, written this session). Both were right. An oracle
  for your arithmetic does not clear your inputs (gotcha 88).
- The timing theory — "our vblank-driven command processor reads buffers a frame late,
  so the guest is writing under us" — was plausible, explained the symptom, and would
  have meant rewriting the command processor. `CZ_PM4_IB_VERIFY=1` killed it for the
  cost of a memcpy: **84,808 buffers walked, 0 modified.**
- The desync was located by building a vocabulary from the capture — B1 uses only
  **225 distinct packet headers** in 24.5 M packets — and flagging the first header in
  a dumped buffer that is not in it. All six pointed at the same place, immediately
  after `XE_SWAP`, after six truncation reports had each named a different innocent
  dword (gotchas 85, 89).

Finding 38's zero-header story is retracted a second time: B1's single zero-header
packet is a mis-recorded `INDIRECT_BUFFER`, so the capture contains **no** genuine one
and never had an opinion. `CZ_PM4_ZERO_IS_NOP` stays as an arm and is no longer
interesting.

**The 1-in-40 crash does not reproduce, and hunting it found a memory-barrier hole
(session 8).** `docs/phase1-notes.md` finding 40. Re-measured first, as an inherited
rate always must be: **0 crashes in 20 runs at 120 s**, all 20 reaching the title
screen. The old figure predates finding 39, when a third to a half of runs stalled in
the first minute. Not "improved" — unmeasurable at this sample size, and saying which
would take hundreds of runs nobody needs yet.

What the site means is now understood: thread `0xF2C` (and `0xF30` — there are two)
runs the graphics driver's command-stream consumer, and the null is a callback slot
that only a `0x8C000000` token in the guest's own token stream ever sets. A null there
means the interpreter ran a "run" token before any token set a callback, i.e. it walked
a stream that had not been published — a producer/consumer ordering question.

Which is how **XenonRecomp bug 6** turned up: `sync`, `lwsync` and `eieio` all lowered
to `// no op`. Right for the hardware half on x86-64 and wrong overall, because every
guest access is a plain C++ load/store through `base` and a construct emitting no code
constrains clang not at all (gotcha 92). Now `lwsync`/`eieio` -> `atomic_signal_fence`,
`sync` -> `atomic_thread_fence`. **This is NOT credited with fixing the crash** — the
baseline was already 0 of 20, so there was nothing to improve on; it is a correctness
fix, measured only for absence of regression.

Two instruments came out of it: the crash reporter now names `ctr == 0` (its old test
required `si_addr == nullptr`, which is never true for that case — it was silent on the
one shape it existed for), with `CZ_CRASH_TEST=nullcall` to prove it; and
`tools/gdis.py`, the guest disassembler, kept on the third time of writing it.

**The audio driver is real and the A5 gate is clean (2026-08-04, session 6).**
`docs/phase1-notes.md` finding 36. All seven audio imports implemented: an XAudio
render-driver client with a guest-thread pump at 5.333 ms/frame (256 samples x 6
channels, planar f32 — read out of the title's own de-interleave loop), and the XMA
context array published into the decoder's little-endian MMIO register file at
`0x7FEA1800`. The deciding detail was that the registered callback is
`lwz r3,0(r3); b <body>` — **the driver passes a POINTER to the context, not the
context** (gotcha 72). Our `MmMapIoSpace(bus=2, phys=..., 64 bytes, protect=404)`
now matches A1's field for field.

Two general defects fell out of it, neither audio-specific: `WaitDispatcher`
dereferenced a guest pointer unguarded, so any null dispatcher object crashed the
host inside our own kernel (gotcha 73); and a runtime-owned physical allocation was
displacing the title's own 447 MB reservation (gotcha 74).

**Two of sixteen cores were being burned by a polite-looking spin (session 9).**
`docs/phase1-notes.md` finding 41. Case Zero blocks two threads forever by design —
`DnsLookupThread` and the session shutdown thread each enter a critical section the
main thread never releases — and on console they simply sleep. Our
`RtlEnterCriticalSection` spun on `std::this_thread::yield()`, which on an idle
multicore host returns immediately, so each of them burned a core for the whole run.

Contended sections now `pause`-spin (64), then yield (256), then **park on a host
condition variable** signalled by `RtlLeaveCriticalSection` — a park rather than the
originally-proposed 1 ms sleep, because a fixed sleep would charge the quantum to every
section held longer than the yield phase. The release path needs a `seq_cst` fence: it
stores the lock word and then loads the waiter count, and store-then-load-elsewhere is
the one ordering x86 does not give (gotcha 92 again, one session later).

Measured, same binary, arms alternated, `CZ_CS_NO_BACKOFF=1` as the control:
**317% → 121% CPU, system time 85 s → 4.5 s, frames unchanged at 1943 ± 1.** The
latency question is settled by `CZ_CS_STATS=1` rather than argued: of 1.6 M
acquisitions in 60 s, 0.26% are contended at all, 417 outlive the pause phase, and
**2 ever reach the park phase**. Gates unchanged — A5 exit 0, A1's full 84-deep prefix,
`truncated=0`, and position 71 permutes 1-of-3 on *both* arms.

**Phase 3 is built (2026-08-05, session 10): there is a window, a present seam and a
real pad.** `docs/phase3-notes.md` is the record.

`runtime/host/window.{h,cpp}` — one module, because in SDL a window, a present and an
input device are one thread. The guest entry moved to a spawned thread so the main
thread can own SDL (gotcha 99); the present is driven from `pm4.cpp` case `0x64`, i.e.
from where the command processor *reaches* the swap packet, not from `VdSwap` (gotcha
100); and `XamInputGetState` now answers out of a real keyboard and, when one is
attached, a real SDL game controller.

**A blank window is the correct result of this phase** — there is no renderer until
phase 5. The title bar carries the live frame count, which is what says the present
seam is running.

Measured with the arms alternated over six 100 s runs, old binary rebuilt and run
*now* (gotcha 86): both arms reach A1's full 84-deep prefix, A5 exit 0, `truncated=0`,
title screen 3 of 3. The window costs ~1% of the frame rate — 3151 vs 3183 frames —
and `CZ_NO_WINDOW=1` on the *phase 3* binary returns exactly 3183, so the cost is the
window rather than the wiring. **Position 71 permutes on BOTH binaries** (3 of 3 on
the old one), which is the same scheduling-sensitive window findings 41 and gotcha 86
already recorded — not a regression, and 1-of-3 vs 3-of-3 is not an improvement
either.

**THE PHASE 3 GATE PASSED, on one real press.** The operator focused the window and
pressed Enter; `[host] pad packet 2: buttons=0010` (START), and **five log lines later
`XamShowDeviceSelectorUI` — position 85** — before the key was even released.
`CZ_FAKE_START_MS` appears zero times in that log. Every run before it stopped at 84.
The gate could not be self-served — any press this machine could synthesise is the arm
the gate exists to retire — so it was scheduled with the operator like a capture
(gotcha 103), with everything not depending on it committed first. Three of the four
arms are now observed: real press → 85; no press over 420 s → 84, zero pad packets;
`CZ_NO_WINDOW=1` → 84. Two packets for one press over ~600 polls, which is gotcha 101's
contract holding.

**The save-data layer is built, and the A1 gate now reaches 92 of 93 with no
divergence at all** (2026-08-05, same session) — `docs/phase3-notes.md` §9.
`runtime/kernel/content.cpp` implements the content enumerators, the XAM enumerate
message and the `save:` mount, and the whole protocol was recovered from the title's
own statically-linked `XamEnumerate` rather than from a capture, which cannot show a
return value (gotcha 104). The chain lands in A1's exact order and our
`XMsgCompleteIORequest(result=1627, extended=80070012, length=0)` matches A1's line
field for field. Four of finding 34's never-executed imports executed for the first
time, `XamTaskSchedule` among them.

The defect worth remembering is gotcha 105: `XamGetExecutionId` was a stub, and it is
the enumeration's **title-id filter** — so every save this runtime enumerated was
silently skipped, producing a log identical to an empty save list. Measured, one save
present: title-id field 0 → filtered (`result=1627`), title-id field `XexTitleId()` →
**accepted (`result=0`)**.

The press also showed where the boot goes next, and it is exactly where the plan said:
after `XamShowDeviceSelectorUI` the title resolves a XAM export dynamically
(`XexGetProcedureAddress(xam, ord=0x279)` — **A1 makes the same call four lines after
its own**, so this is the sequence, not a divergence) and then runs
`XamContentAggregateCreateEnumerator` → `XamGetPrivateEnumStructureFromHandle` →
`XamAlloc` → `XamTaskSchedule` → `XamGetOverlappedResult` → `XMsgInProcessCall` →
`XMsgCompleteIORequest`. That is positions 86-92 and **precisely the save-data layer
deferred out of finding 34** — and it includes `XamTaskSchedule`, one of the eight
implemented-but-never-executed imports, so that debt starts being paid by the next
phase rather than needing its own.

**PHASE 5 IS BUILT: there is a renderer, and it draws real game content
(2026-08-05, session 11).** `docs/phase5-notes.md` is the record — read it before
touching `runtime/gpu/vk_renderer.cpp`.

`runtime/gpu/vk_renderer.cpp` translates the PM4 draw stream onto a host Vulkan device
with the XenosRecomp shaders. **Off unless `CZ_VKDRAW=1`**, which makes the phase 3
binary available in the same build as the control arm for every claim below.

- **The shader pipeline is complete: 336 of 336 distinct shaders translate, zero
  failures**, and not one recompiler change was needed — XenosRecomp's Fable 2 patches
  carry over whole. `tools/build_shader_spv.sh` is the pipeline.
- **Our `IM_LOAD` arithmetic is now validated against hardware.** A boot dumps 121
  distinct microcode blobs and **120 are byte-identical to A1's**, modulo dword order.
  Nothing had ever checked that packet's size field.
- Measured over a 120 s headless boot: **1,087,826 indexed draws**, 125 pipelines, 958
  textures untiled and uploaded, 67 resolve snapshots, **450,488 texture fetches served
  from a snapshot**, and **1,187 of 1,195 frames presented from the front-buffer
  resolve**. The picture is the blood streak from the DEAD RISING 2 wordmark plus UI
  text — recognisably E2's logo, and not yet all of it.
- **All pre-existing gates hold with the renderer on**: `--smoke` OK, A1's full 84-deep
  prefix, A5 exit 0 (2 windows, both permutations), both PM4 capture oracles clean,
  `truncated=0`, zero parser stalls. Position 71 permuted on the renderer-**off** arm
  this time, which is gotcha 86's lesson arriving from the other direction.
- **Cost: 1,488 frames per 100 s with the renderer on against 3,090 with it off** —
  roughly half the frame rate, from a synchronous submit and a full readback per frame.
  A number to re-measure once the picture is right, not a defect to fix before it.

**ARCHITECTURE PIVOT DECIDED (2026-08-06, operator's call): the renderer moves to a
D3D TRANSLATION LAYER in UnleashedRecomp's architecture** — hook the title's
statically-linked XDK D3D functions, never touch the ring. **THE NEXT SESSION STARTS
FROM `docs/d3d-kickoff.md`** — it carries the hand-off, the measured recon tables
(Present is identified: `sub_82841F00` is `D3DDevice_Swap`, the 7 ring-emit
primitives are named, device init is two walks from CreateDevice, all 20 Vd-import
anchors listed), the OBSERVE-then-REPLACE bring-up order, and the missed-hook
detector (`Pm4_PacketCount()==0` on the hooked arm). `docs/d3d-translation-plan.md`
is the decision and licensing record beneath it. **plume is license-VERIFIED MIT**;
only video.cpp-derived code carries GPLv3. The short form: every hard renderer defect this phase hit lived below the D3D
line, and UnleashedRecomp's runtime stubs VdSwap/VdInitializeRingBuffer EMPTY — the
whole findings-38-41 layer is dead code in that architecture. Case Zero's D3D cluster
is already bounded (0x8283xxxx-0x8284xxxx, TUs 159/160/175/176) via import call sites;
the PM4 executor stays as boot engine and control arm; the shader cache, texture
decode, and every instrument transfer. UnleashedRecomp is GPLv3 and the operator has
authorized taking code, not just structure — provenance headers on every adapted file.

**PHASE A IS DELIVERED (2026-08-06, session 12): the hook table exists and OBSERVE
mode validates it.** The table with evidence per row is in
`docs/d3d-translation-plan.md`; the instrument is `runtime/gpu/d3d_hooks.cpp`
(`CZ_D3D_OBSERVE=1`, 43 hooks, log + call through). The structural result that
shrinks Phase B: of the cluster's 117 externally-called functions only **27 can
reach the ring** — the rest are state setters writing the device struct's register
shadow and can stay guest code even in replace mode. CreateDevice is
`sub_8283CCE8`; the engine submits draws directly (finding 40's worker threads are
idle at boot/title); a 360 Clear is a resolve-with-clear-bits, which reconciles the
API stream's 20 Clears/frame with phase 5's ~20 resolves/frame.
`tools/guest_callers.py` is the call-graph scanner Phase A was answered with —
reach for it before disassembling anything's callers by hand.

**PHASE C IS BUILT AND RENDERING (2026-08-06, session 13): draws serviced by
REDIRECTED EMISSION — the title's own flush is the encoder.** `CZ_D3D_DRAW=1`
redirects each content API call's command-buffer cursor (`dev+0x30/0x34/0x38`)
into a private guest scratch, lets the guest body run, and a private PM4-subset
walker (`runtime/gpu/d3d_draw.cpp`) folds the emission into a private register
file + shader hashes for the phase-5 renderer's decode guts
(`VkRenderer_D3DDraw/D3DSwap`, parameterized — `pm4.cpp` untouched as the control
arm). Measured: the legal screen and CAPCOM logo render pixel-correct from the
D3D arm; **A1 = exact 82-prefix (phase B's KeResetEvent window CLOSED), A5 exit 0
with zero real windows — the port's best kernel gates**; zero crashes on the
final interrupt design. The hard-won piece was interrupt delivery: content-stream
INTERRUPTs carry the token worker's kick, their arms are dual-transport, and the
walker now performs the ISR's source-1 path itself from one guarded read (four
designs; the trail is in the git log and `docs/d3d-translation-plan.md`).
~~OPEN BLOCKER: the boot deadlocks mid-cinematics.~~ **CLOSED in session 14, below** —
and the walker's ISR replication described above is retracted with it.

**PHASE C PART 2 (2026-08-06, session 14): the movie deadlock is fixed, and the rule
the redirect was missing is "emit where the READER lives".** Details in
`docs/d3d-translation-plan.md` §"Phase C part 2".

`sub_82846288` is the **callback armer** (the Phase A "fence/throttle" label is
retracted): it lays down an arm of scratch registers `0x057C/0x057D`, three
`WAIT_REG_MEM`s, an `INTERRUPT` and a re-poison, and the graphics ISR reads that
callback back out of GUEST MEMORY. Redirected emission put the whole block in our
private scratch, where the walker had to emulate a hand-off whose correctness IS its
ordering — four designs, all racing the poison. It now runs with the REAL cursor
restored (`D3dDraw_ServiceRealRing`), so the title's own ISR delivers it. The second
half: **the reserve `sub_82845F68` is not "give me space", it is CLOSE-AND-KICK**, and
Resolve's multi-tile path calls it purely for the kick and discards the return value;
suppressing it left the block unkicked.

Measured, same binary, one boot each — and both halves are load-bearing, because
`CZ_D3D_NO_RESERVE_KICK=1` reproduces the old stall exactly:

| | before | after | NO_RESERVE_KICK arm |
|---|---|---|---|
| ISR delivers `sub_8284AAD0` (the worker kick) | **1 in a whole boot** | continuous* | 5 |
| walker-delivered interrupts | 200, all `82841878` | 0 | — |
| deepest file | #56 `cinematics.big` | **#60 `models\zombies.big`** | #56 |

Gates on this binary, both arms: `--smoke` OK; A1 = exact 84-prefix (control) / exact
82-prefix (draw); A5 = exit 0, 2 permutation windows, **0 real**, on both. Unchanged
from the phase C best.

~~**THE NEW BLOCKER, localised:** the boot parks at `models\zombies.big` with the
engine thread at 99% CPU in `sub_82846210`'s `while ([dev+0x2B04] != 0)` spin ... the
worker is woken constantly with nothing to drain; reconciling that is probably the
fix.~~ **Half retracted in session 15 — see phase C part 3 below. The spin is real; the
reading of it was not.** The counter is **NEGATIVE**, so the `!= 0` test can never
succeed, and the worker drains far MORE than the title submits (6 increments against
18,900 decrements in one boot), not less.

**PHASE C PART 3 (2026-08-06, session 15): the counter is negative because the command
processor is REPLAYING the hand-off block.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 3"; hand-off in
`docs/d3d-phase-c4-kickoff.md` (superseded by
`docs/d3d-phase-c5-kickoff.md`).

`CZ_PM4_MEM_WATCH` pointed at the ISR mirror's callback slot counts **8,152,069 writes
in 200 s** — `8284AAD0` armed 2,717,263 times and poisoned 4,076,035 — while the guest
calls the armer **405 times**. Corroborated three ways: the ring goes from ~390 packets
and ~48 draws a frame to **1.25 M packets and 135,000 draws per second** with `XE_SWAP`
frozen; `sub_828455C0` is called **106 M times**, always `count=1`, always cycling the
same three segments (93/11/23 dwords, the 93 being the one that contains the arm); and
the fence-completion word freezes at a constant while `emitted` climbs, which is the
signature of replay rather than of a slow GPU. `truncated=0` and the IB verify stay
clean throughout — the parser is right and the bytes are wrong (gotcha 88, third time).

The loop needs no seed and has gain one: a segment containing an arm block reaches the
worker's token stream -> the worker submits it -> the CP executes the arm and its
INTERRUPT -> the ISR's `sub_8284AAD0` pushes the SAME token-buffer pointer back on the
worker's ring -> the worker restarts that buffer at `buffer+4` -> resubmits the segment.

Two more of the four arm-block emitters moved to the real ring this session, both on
measured evidence and **neither of them the cure**: `sub_82841AD0` (the Phase A name
"PreSwapResolve" is retracted — it RESOLVES NOTHING, it is a pure GPU/CPU hand-off
emitter, and redirected it put all 405 of a boot's armings in the private scratch), and
`sub_8284B9C0` (all six of its calls ran with the scratch cursor installed; it is also
the only site that arms `sub_8284AAD0` and the only `+1` the counter gets).
`CZ_D3D_REDIRECT_PRESWAP=1` is the same-binary pre-fix arm.

~~`CZ_PM4_STOP_ON_WAIT=1` was re-tested rather than inherited as retired: part 2 retired
it while the arm blocks were in the SCRATCH, where the flag could not apply to them at
all. With them in the ring it genuinely gates them — and it is still runaway.~~
**Retracted in part 4 below: the flag was gated on `depth == 0`, so it could not apply
to these packets in EITHER session** (gotcha 151).

Gates, this binary, both arms: `--smoke` OK; A1 = exact 84-prefix (control) / exact
81-prefix (draw, when the run does not hit the long-known position-71 permutation);
A5 = exit 0, **0 real windows**, on both. Unchanged from the phase C best.

**PHASE C PART 4 (2026-08-06, session 16): the replay is the FLYWHEEL, not the fault —
and the one brake our command processor has never had is now built.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 4".

Part 3's two ranked candidates are both retired **by measurement, on the control arm**.
The arm block is inside its own segment by construction (`sub_8284B9C0` writes it at
`r28-4` and submits `[r28, armEnd+4)`), and the control arm queues that segment to the
worker on 5,696 of its 5,698 frames without ever looping — so "the queued segment
should not contain the arm block" was never a difference between the arms. The same
probe on both arms, same era:

| | PM4 control | phase C draw |
|---|---|---|
| `[fence] arm cb=8284AAD0` : `[fence] isr cb=8284AAD0` | 768 : 766 | 12 : 856 |
| whole boot, `incr=1` submits : drains | 3,958 : 7,913 | 6 : 132,545 |

The hand-off regenerates its own wake-up on hardware too, and it converges because the
guest arms with a NEW token buffer every frame: gain one, pointer advancing, a
pipeline. The draw arm's whole boot has **four** armings cycling between **two** buffer
pointers, so once the guest stalls each walk resubmits the same segments forever.
**The guest stalling makes the counter negative and the negative counter keeps the guest
stalled** — part 3 read the flywheel as the cause.

The missing brake: on hardware the CP STALLS at the hand-off block's `WAIT_REG_MEM`s.
`gpu/pm4.cpp` gated `CZ_PM4_STOP_ON_WAIT` on `depth == 0`, and every one of these waits
is inside an INDIRECT BUFFER — so both of the flag's retirements measured a no-op
(gotcha 151). It now stalls at any depth and **resumes at the recorded dword** rather
than re-walking the buffer (gotcha 152), and a deliberate stop is explicitly not a
truncation. With it working, **both arms park at frame 1 on the same packet**: a wait
for SCRATCH_REG1 (`mirror+4`, register `0x0579`, the one `sub_82841AD0` sets to 1) to
read back zero, which nothing in our runtime ever writes. That is a statement about the
runtime, not about phase C, and it is where part 5 starts.

Gates, this binary, both arms, default flags: `--smoke` OK; A1 = 84-prefix on the
control arm (this run hit the long-known position-71 permutation) / **exact 82-prefix**
on the draw arm; A5 = exit 0, **0 real windows**, on both; `truncated=0` with 3.59 M
packets walked. Deepest file: #63 `prologue_z01.big` (control) / #60 `models\zombies.big`
(draw). Unchanged from the phase C best.

**PHASE C PART 5 (2026-08-06, session 17): the missing CPU side of the hand-off was a
DISPLAY CONTROLLER, and the brake now works.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 5"; hand-off in
`docs/d3d-phase-c6-kickoff.md`.

Part 4's question — who writes the zero the hand-off block's `WAIT_REG_MEM` waits for —
is answered, and the answer is nowhere near the ring. `CZ_PM4_MEM_WATCH=BBF39464` on a
healthy control boot: **3,089 writes, every one the value 1, every one from the PM4
stream.** The zero comes from the CPU, in the guest's own vblank ISR path, behind a GPU
MMIO **read** at `0x7FC86544` bit 0 — the display controller's gate, which our runtime
left at zero for the life of every process this port has ever started. Behind it sits
the title's swap queue: 16 records of `{surface, due tick}` at `dev+0x418C`, a vblank
tick at `dev+0x4174`, and a walker (`sub_82841760`) whose ONLY caller is that branch.
A record whose surface is zero means "nothing to scan out, just release the GPU" and IS
the `[mirror+4] = 0` store. Fable 2 found the same gate at the same address (its
findings 48 and 57); two sessions of phase C reasoned about the hand-off's packets
without either port's notes being consulted for the register (gotchas 153-154).

A second defect surfaced the instant the first was fixed: **the graphics interrupt is
addressed to a SET of hardware threads.** The arm block writes a six-bit CPU mask into
`mirror+0`, the ISR clears `1 << PCR[0x10C]`, and the block's trailing `WAIT_REG_MEM`
holds the CP until the word is zero. The mask DEFAULTS to CPU 2 — where the pump has run
since phase 1, so the common case was right by accident — but `sub_827D2FC0` arms with
CPU 4 and our one ISR thread could never acknowledge it. The pump now takes the interrupt
once per named CPU (gotcha 155). The ISR body is per-CPU too: `sub_8284AAD0` pushes onto
the job ring at `dev + cpu*0x6C + 0x2C40`.

Measured, same binary, arms alternated, one 100 s boot each:

| | control (`CZ_NO_VBLANK_GATE` / `CZ_ISR_SINGLE_CPU`) | fixed |
|---|---|---|
| vblank tick after 30 s | 0 | 1,860 (62/s) |
| swap queue, brake OFF | head 0 / tail 1,540 | head 27 / tail 1,165 |
| brake ON, PM4 arm | parks at frame **7**, `ack=00000010` | **217 frames and climbing**, head = tail, `truncated=0`, `prologue_z01.big` |
| brake ON, draw arm | parks the same way | **725 frames**, head 724 / tail 725, `truncated=0`, `models\zombies.big` |

**With all three pieces — the gate, the per-CPU acknowledge and part 4's
stall-with-resume — `CZ_PM4_STOP_ON_WAIT=1` runs this title's real GPU/CPU hand-off end
to end, paced by the guest, on BOTH arms.** The draw arm's part-3 runaway (1.25 M packets
and ~135,000 draws a second with `XE_SWAP` frozen) does not happen: 306,288 packets,
46,560 draws, 725 frames. The gate and the per-CPU acknowledge are ON by default; the
BRAKE is not, because this is one run per arm and not a rate (gotchas 50-51) — measuring
it properly and promoting it is part 6's first job.

The cost, stated because it is real: **with the brake OFF the per-CPU acknowledge makes
the draw arm's runaway spin harder** (1,745 -> 2,856,448 `XE_SWAP` in 100 s), because
each interrupt now produces several worker kicks instead of one. Same flywheel, more
gain; the control arm is untouched (3,091 vs 3,088 frames). It stays default-on because
it is correct, and the pairing with the brake is now explicit.

Gates, this binary, default flags: `--smoke` OK; A1 = **exact 84-prefix** (control) /
position-71 divergence (draw, the long-known scheduling window); A5 = **exit 0, 0 real
windows, on both**. Deepest file: `prologue_z01.big` (control) / `models\zombies.big`
(draw). Unchanged from the phase C best.

**PHASE B IS DELIVERED (2026-08-06, session 12): `CZ_D3D=1` services the content APIs
(draws/clears/resolves → no-op) while the frame lifecycle calls through — and
the ring goes SILENT (+0 packets/frame steady state), the boot reaching the
title screen at ~340 fps with zero faults over 33,984 frames.** The title's own
Swap takes its empty-frame branch when nothing was drawn, so the completion
protocol did not need replacing for the skeleton. Two failures worth their
weight: servicing Swap directly deadlocks three threads (the completion protocol
lives in the D3D worker `sub_8284B828` + an event inside the device struct), and
servicing the busy-track entry `sub_82837D70` crashes — it RETURNS A CPU POINTER
(a Lock-style API); OBSERVE validates firing patterns but only REPLACE validates
return-value semantics. A1 on the replace arm has exactly one real window
(`KeResetEvent` + the ISR spinlocks — all verified downstream of ring
consumption, which the arm removes by design). Next: phase C — service the
draws/state with a host renderer reusing `vk_renderer.cpp`'s decode guts, keyed
off the device struct's register shadow (offsets in the Phase A table).

**PHASE C PART 6 (2026-08-06, session 18): the brake is the DEFAULT, on 40 runs — and
three of the four numbers it was to be judged on could not have judged it.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 6".

`CZ_PM4_STOP_ON_WAIT` is promoted; **`CZ_PM4_NO_STOP_ON_WAIT=1` is now the control
arm.** 10 runs per configuration, 120 s each, arms alternated within each round:

| | frames (median) | spread | max hold streak | queue head==tail | deepest | crashes |
|---|---|---|---|---|---|---|
| PM4, brake off | 3,680 | 1x | 0 | **0 of 10** | #83 | 0 |
| **PM4, brake on** | 2,446 | 1x | 1 | **10 of 10** | #83 | 0 |
| draw, brake off | 290,874 | **10,397x** | 0 | 3 of 10 | #60 | 0 |
| **draw, brake on** | 3,616 | 1x | 2 | **10 of 10** | #60 | 0 |

`truncated=0` in all 40. The cost is 2,446 frames against 3,680 and it is not a loss —
it is the title paced at its own frame timing rather than the CP outrunning it. Two
facts in that table are about the title, not the brake: **free-running overflows the
flip queue in 10 of 10** (the unpaced state was never healthy, it just had no
instrument on it), and **the draw arm's default configuration is BIMODAL** — 332 to
3,451,841 frames — so part 5's two draw-arm numbers are two modes of one distribution
(gotcha 159).

**Part 4's prediction is RETIRED, not confirmed.** Re-running part 3's instruments on
the current binary: the brake cuts the callback hand-off's per-frame amplification
~39x (430 -> 11.1 kicks/frame against the control arm's 1.9) but leaves the raw
deliveries-to-armings ratio **unchanged at ~300x**. It contains the symptom; the cause
is untouched, and that is where part 7 starts. (**The ~300x is RETRACTED by part 7** —
it is a stopwatch, not a gain; see below. The ~39x per-frame figure stands.)

Three of the harness's own numbers were broken before any of the above could be
measured, and the story is gotchas 157-160: the deepest-file column was the PRINT CAP
(the boot opens 84 files, not 64, and ends at `cinezombie.big`); the stall counts were
the running index of a capped print; the number that decides the promotion — is a stall
ever released — did not exist; and the counter written for it was wrong twice, caught
both times by the deliberately-parked control arm rather than by reading the code.
`MirrorIsPoisoned()` is NOT retired by the promotion as the kickoff expected: it
records zero skips across all 40 runs **including brake-off**, so it was already inert
independently.

**Gates:** `--smoke` OK; A5 **exit 0, 0 real windows, both arms**; A1 exact 82-prefix
on the draw arm; both capture oracles clean. A1's position-71 window permutes 4 of 10
brake-on against 1 of 10 brake-off (Fisher p ~= 0.30, no cost in depth or in A5) —
reported because it is the one number that is not flat.

**PHASE C PART 7 (2026-08-06, session 19): there is no 300x amplifier — and the draw
arm's stall is ONE event, at the first tiled frame.** Details in
`docs/d3d-translation-plan.md` §"Phase C part 7".

Part 6's open question was "what does one delivery do that makes the next one happen,
to the tune of three hundred?" Nothing does. `cpu/chain_stats.h` counts the hand-off
link by link on every run (`ring: chain ...` in `CZ_RING_TRACE`), and on the PM4
control arm every ratio is one or a constant: **`ints/arms` = 0.9997** — the command
processor executes each arm block exactly once — **`isr/ints` = 1.000**, `kicks/isr` =
0.523, and `walks == kicks == drains` to the unit over 173 s. The draw arm's `arms`
column **freezes at 227 seven seconds into the boot** while the numerator keeps
counting, so the "~300x" reads 1.8x at 8 s, 10x at 35 s and 30x at 78 s on one binary.
A frozen denominator was the finding (gotcha 161); part 6's own table already showed it
(437 and 230 armings against the control arm's 14,794) without it being read.

What the freeze IS, traced end to end and reproduced:

- **A single event, not a decay.** For the whole healthy era the D3D worker is never
  used at all — `kicks=0`, `queued=0` at all 986 segment submits. Within one tick the
  worker engages and the guest stops arming for good. The control arm has the identical
  transition at the identical era and survives it.
- **The era is the first TILED frame**, not a file: Resolve's multi-tile path, taken
  once the title starts rendering its scene as two 640-wide tiles (gotcha 118). It is
  the only site in the image that arms `sub_8284AAD0` (82838A94), closes-and-kicks
  (82838AA8) and queues that segment to the worker (82838AD0) in one breath. `#60
  models\zombies.big` is a coincidence of timing (gotcha 164).
- **The engine blocks in the per-frame GPU sync** (`sub_82845230` -> `sub_82845160`),
  `target=1039 emitted=1043 completed=1019`, and never returns.
- **The fence completion word is on a nine-value CAROUSEL, not lagging.** 26,017 GPU
  stores in 100 s; the last 4,000 are 440 laps of `2DF 2E1 2E3 2E5 2E7 2E9 2EB 2ED 2EF`.
  It also visibly REGRESSES between two consecutive sync-wait prints (1023 -> 1017), so
  a wait past the top of the carousel is unsatisfiable rather than slow (gotcha 163).
- **What is being replayed is the arm block itself.** The three most-resubmitted ring
  entries on the draw arm are 93-dword segments — the size of the segment the multi-tile
  Resolve closes around its own arm — at 132/126/86 resubmissions against the control
  arm's worst case of **11**.

**And the obvious cure was RUN and is a negative result.** `CZ_PM4_FENCE_MONOTONIC=1`
refuses any GPU store that moves the completion word backwards; it engages **5,711
times in 90 s** and the boot freezes identically (`arms` pinned at 190, `distinct=2`,
`#60`). So the regression is not what blocks the wait — the wait is for a fence beyond
the top of the carousel, and the segments carrying those `EVENT_WRITE`s are **never
executed at all**, because the ring is saturated with the replayed arm segment. The
missing execution is the fault; the regressing word was its most visible symptom.

Two more hypotheses were drafted and killed by running the same probe on the control arm,
recorded because each looked decisive: the `[obj+0x48]` resume pointer is non-null at
half the drains on **both** arms (1,732:1,731 vs 3,576:3,575), and "6 increments against
1,873" is 1.0 per frame against 3.0 per tiled frame — the same frozen-denominator trap
as the 300x, one screen further down. `CZ_PM4_NO_CP_INTERRUPT=1` is also recorded as a
NEGATIVE result: it cannot isolate the replay, because the boot deadlocks at `boot.bct`
(file #5) without source 1.

Gates unchanged: `--smoke` OK; the control arm reaches `#83 cinezombie.big`;
`truncated=0`, `max` hold streak 1 (control) / 2 (draw).

**PHASE C PART 9 (2026-08-06/07, session 21): the title screen is TWO screens, and four
defects sat between the 3D one and the display.** Details in `docs/phase5-notes.md`
§§6s-6u and `docs/d3d-translation-plan.md` §"Phase C part 9"; hand-off in
`docs/d3d-phase-c10-kickoff.md`.

Part 8 handed over "the 3D background and the DEAD RISING 2 wordmark are black on BOTH
arms" as a phase-5 renderer gap. Measuring all 32 dumped frames of a boot rather than
looking at one says why the claim was half wrong: **49 frames in ~1,000 carry the
DEAD RISING 2 CASE ZERO logo and are a near-exact match for capture E2.** The renderer
had been producing a correct title screen for one frame in twenty, unseen, and it was
the OTHER era — capture **E3**'s animated Still Creek background — that rendered black.
Four defects, each hiding the next, all found and fixed on the **PM4 control arm**:

1. **A stale texture-cache entry composed the whole scene away.** `UploadTexture`
   consulted the fetch-constant cache before the resolve-snapshot check, so the rule the
   code already stated (a snapshot must not be cached) only held for a surface whose
   FIRST fetch already had one. The colour-grading LUT is resolved late in a frame and
   sampled early in the next, so its first fetch during the boot uploaded whatever the
   allocator had left there and froze it for the process. The tone map ends in two LUT
   lookups: a black LUT is a black frame. Tone map output 0.00% -> **95.3%** non-black,
   presented frame 2.31% -> **99.4%**.
2. **The exploded geometry was DRAW_INDX read one dword off** — the index swizzle is the
   TOP two bits of the SIZE dword, and reading it off the ADDRESS also masked away
   address bit 1, real for a 2-byte-aligned 16-bit index buffer (~40% of draws, read one
   index early). Every draw in this title is `8-in-16`. Fixed: a recognisable Still Creek.
3. **A rectangle list's fourth corner was never drawn** (`v0 + v2 - v1`), so half of
   every per-pass CLEAR was missing — and 233,155 draws a boot clear DEPTH ONLY, so the
   previous pass's depth survived in the other half and rejected the scene behind it.
4. **Window coordinates are in PIXELS and our EDRAM is at SAMPLE resolution**, so on the
   4x-MSAA surface the scene tile's clear covers 320 of its 640 columns.

**The title screen's LEFT HALF now renders as a complete, bright Still Creek** — sky,
power lines, the gas station, zombies, the road, the grass.

Every one of the four has a same-binary control arm (`CZ_VK_TEX_CACHE_FIRST`,
`CZ_PM4_INDEX_ADDR_SWIZZLE`, `CZ_VK_RECT_HALF`, `CZ_VK_NO_MSAA_WINDOW_SCALE`), and the
instrument that made 3 and 4 diagnosable is `CZ_VK_NO_DEPTH_TEST=1` (gotcha 173).

**Gates, PM4 arm:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 0 real windows**;
`truncated=0`; deepest file **#83 `cinezombie.big`**; frames presented unchanged.

**PHASE C PART 11 (2026-08-07, session 23): THE RIGHT TILE IS FIXED, AND IT WAS A
PACKET WE NEVER ANSWERED.** `docs/phase5-notes.md` §6x; hand-off in
`docs/d3d-phase-c12-kickoff.md`. The open item below and part 10's whole explanation of
it are superseded — read §6x before either.

`EVENT_WRITE_EXT` with event `0x1A` is the Xenos **screen extent query**: the GPU writes
the rectangle it just rasterized into guest memory, and this title feeds it straight
into the next frame's bin masks. Our command processor decoded that packet and did
nothing with it — 818,507 no-ops a boot — because the fence family's handler stores only
when a packet carries a value dword and this form carries an address and none. So the
guest's own fix-up pass (`sub_8284A7F8`) intersected **uninitialised memory** against
its tile rects and wrote "touches no tile" onto 76% of records, and the right tile's
pass discarded them.

Answering it conservatively — an extent larger than any tile, "this draw may have
touched anything" — with `CZ_PM4_NO_SCREEN_EXTENT=1` as the same-binary control arm:

| | control | fixed | B1 (hardware) |
|---|---|---|---|
| draw packets discarded by the bin rule | 32.7% | **0.28%** | **0.3%** |
| fix-up pass output | 76% `80000000` | 100% `8000000F` | — |
| scene surface median coverage | 56.1 / 53.8% | **99.5 / 99.5%** | — |
| draws per frame (median) | 1,630 / 1,634 | **2,484 / 2,474** | — |

First time this port's predication has agreed with the capture. Two runs per arm,
alternated; 45.7 pp cross-arm against a 1.5 pp band and 0.00 pp within-arm.

**And part 10's three claims about this are RETRACTED in place** (`phase5-notes.md` §6w):
the fix-up pass is not gated shut (3,496 of 3,497 dispatcher entries have it open) and
does not patch zero records (1,751 calls, 388,451 records) — those numbers were a probe
printing only its FIRST call (gotcha 186); `[obj+0x164]` is the current bin SELECT, not
a flags word, and its bit 31 means "tile 0", which is also why the LEFT tile keeps every
draw regardless of mask; and `0x80000000` is not the placeholder but a trailing reset,
the placeholder being the LEADING `SET_BIN_MASK_LO FFFFFFFF`.

**THE OPEN ITEM AS PART 10 LEFT IT — superseded by the above, kept for the trail:** the RIGHT tile
(screen 640..1280) is nearly empty, and it is **ME bin predication**. `gpu/pm4.cpp` has
implemented `(header & 1) && (binMask & binSelect) == 0` since phase 1 and had never
counted it: **a third of this title's draw packets are discarded by it** (1,039,423 of
3,113,236 over 1,579 frames). One frame's two scene passes execute **931** draws and
**23**. `CZ_PM4_BIN_TRACE` prints the pair hardware compares, and the shape is stark —
the tiles select bins `{0,1,31}` and `{2,3}`, and in the `{2,3}` tile 74,773 draws
carrying mask `80000003` and 25,770 carrying `80000000` can never overlap it. A title
does not emit 100,000 unreachable draws a boot, so either the bins are not the
left/right split we assume or the comparison is wrong in one of three places (the 64-bit
LO/HI assembly, the meaning of bit 31, or the ORDER — a mask read one draw late gives
exactly this shape). **The check to run first needs no emulator:** B1 carries the same
`SET_BIN_MASK`/`SET_BIN_SELECT`/`DRAW_INDX` stream, so replaying the rule over it says
whether 8% survival is what hardware does.

**And a number withdrawn before it did damage:** "hardware issues ~2,540 draws a frame
and we issue ~1,620" compares draw PACKETS in a capture against draws the RENDERER
ACCEPTED. At one instant of one run the command processor parses 1,971 packets a frame
and hands the renderer 1,313; the predication eats the difference. `ring: ... draws=N
(predicated out=M)` is now always on, because a mechanism with no counter cannot be
subtracted from a comparison (gotcha 162).

**PHASE C PART 12 (2026-08-07, session 24): the menu panels, localised — and the dead
ISR code deleted.** `docs/phase5-notes.md` §6aa; hand-off in
`docs/d3d-phase-c13-kickoff.md`.

Part 11 handed over "three black panels and malformed label text on the new-game
screen", newly reachable headless via `CZ_FAKE_PRESS_SEQ=START,A,A`. Both are now
localised to a NAMED OBJECT by arms rather than by reading, and the shape part 11
predicted (§6s's — a pass reading a surface the renderer never wrote) is **refuted**:
the panel is the frame's last pass, a 115-draw compose into the front buffer, and its
inputs are all present.

* **The three black rectangles are ONE texture** — `0364B000`, a 16x16 DXT1 whose every
  texel reads zero. `CZ_VK_SKIP_TEX=0364B000` removes exactly those three rectangles and
  **reveals three correct thumbnails underneath**, so everything behind them is right.
  The draws blend `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` (honoured), and an all-zero DXT1 is
  opaque black under BC1 — so hardware's bytes differ from ours, and the open question is
  who writes them. That is a CPU write, so `CZ_PM4_MEM_WATCH` cannot see it; the tool is
  gotcha 143's hardware watchpoint.
* **The malformed text is one of two glyph atlases** — `007C6000` (376x376) garbles,
  `007BB000` (184x184) is perfect, through the SAME `(vs, ps)` pair with every other
  fetch-constant field identical. Two arms cleared the atlas itself: `CZ_VK_TEX_REFRESH`
  (2,250 in-place re-uploads, picture identical) and `CZ_VK_TEX_DUMP` (a clean, correctly
  untiled page of glyphs). That leaves the draw's texture COORDINATES; 376/184 = 2.04 and
  the glyphs read as magnified fragments.

**Part 11's item 4 is closed**: the private walker's `case 0x54:` ISR replication and
`MirrorIsPoisoned()` are deleted, after re-confirming both zeros on a correctly
configured draw arm at `#83` (`arms=12627 ints=12626 isr=12626`, `kicks == walks ==
drains = 6752`, `distinct=885`, `truncated=0`). They guarded a race the brake closed in
parts 4-6, and the counter read zero even on part 6's brake-OFF arm.

Six new instruments, all off by default, and one of them existed only in this file:
`CZ_VK_SKIP_TEX`/`CZ_VK_ONLY_TEX`, `CZ_VK_TEX_CENSUS`, `CZ_VK_TEX_REFRESH`,
`CZ_VK_TEX_DUMP`, `CZ_VK_SNAP_FRAME`, `CZ_VK_FRAME_DUMP_EVERY` — plus
**`CZ_VK_PASS_DRAWS`, which has been documented here since part 9 and was never
implemented** (the count was a hardcoded 4).

**Gates, both arms:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 2 windows,
0 real**; `truncated=0`; deepest file **#83 `cinezombie.big`**.

**PHASE C PART 13 (2026-08-07, session 25): the UI's whole text layer was ONE run
repeated, and the crash 53 files deep was the title's own ASSERTION.**
`docs/phase5-notes.md` §§6ab-6ad and `docs/phase3-notes.md` **finding 50**; hand-off in
`docs/d3d-phase-c14-kickoff.md`.

Part 13's list was the two menu defects then the picture; the item listed LAST as a
frontier turned out to be the biggest thing in the session.

- **The malformed menu text was not the texture coordinates — it was
  `VGT_INDX_OFFSET`.** A draw packet has no base-vertex field, so that register is the
  only way a title sub-allocates ONE dynamic vertex buffer between draws, and this
  title's entire UI works that way: 115 draws whose fetch constant never changes
  address, with the offset advancing by exactly the previous draw's index count. The
  renderer printed the register in `CZ_VK_STATE_PROBE` and applied it in none of its
  three submission paths, so **every draw rendered the FIRST run's vertices** — one text
  run correct, every other one that same run's glyphs through whichever atlas it bound.
  Part 12's attribution to the two glyph ATLASES is retracted: that is the visible
  difference between the draws and the cause of neither. The save-slot screen now
  renders `SLOT 1/2/3`, `- NEW GAME -`, `GAMER PROFILE`, `Player`, `LV. N/A`, the
  PP/Money rows and the `A SELECT / B BACK / Y DEVICE SELECTOR` legend.
  `CZ_VK_NO_INDX_OFFSET=1` is the control arm; it engages 211 times in a plain
  title-screen boot and on essentially every menu draw, which is why five phases of
  scene work never saw it.
- **The black panels: nothing writes them.** Three hardware watchpoints, one per
  physical alias, through the whole menu era: **zero hits**, and no resolve targets that
  address. Part 12's inference that "hardware's bytes differ from ours" is retracted for
  a measurement — the title binds a 16x16 DXT1 it never fills, on three draws for three
  EMPTY save slots. ~~The one remaining test is a run with a real save present.~~
  **CLOSED in part 14 by that test: the panel is the save's THUMBNAIL, and black is the
  correct picture for a slot with no valid content** (`docs/phase3-notes.md` finding 51).
- **The SIGSEGV at file #137 was `dbAssert(0 && "Bad file digest.  Please re-link the
  executable and try again.")` from `digestmanager.cpp`** — not a memory bug. It looks
  like one because XenonRecomp lowers `twi` to nothing, so the deliberate `stw r26,0(0)`
  that follows the trap is what faults. Three links, each measured, none of them
  changing an observable alone: `XexGetModuleSection` answered nothing ever (its comment
  was written about a runtime with no loader and never re-asked); the XEX RESOURCES it
  should answer from live in `.idata`, which `main.cpp` skipped by NAME under a comment
  describing a bounds condition only `.reloc` meets; and the SHA-1 the digest manager
  calls is three kernel imports left as generated stubs — and a stub is the wrong shape
  for a hash, because no digest value means "not implemented", so the guest compared
  twenty zero bytes and refused to run.
  **Result: #137 + SIGSEGV -> #154 `skeleton\childfullbody.big`, zero faults over
  300 s** — 71 files past anything this project had reached.
- **The picture against capture E, asked cleanly for the first time.** No transform
  (every frame's best orientation is `identity`, runner-up 0.14-0.35 behind), and four
  named differences: the whole frame is uniformly out of focus AT EVERY DEPTH where E3
  is sharp except in the far distance; colour is flat and green-shifted; the
  `(C) CAPCOM CO., LTD. 2010` line and one sign's lettering are missing; the `GAS`
  balloon and the street bunting are blank.

**Gates, BOTH arms:** `--smoke` OK; A5 **exit 0, 0 real windows**; `truncated=0`;
deepest file on a no-input boot **#83 `cinezombie.big`**. A1 is an exact 84-prefix on
the PM4 arm and hit the long-known position-71 scheduling window on the draw arm
(gotcha 86). The draw arm's chain is the healthy shape — `arms=12741 ints=12740
isr=12740`, `kicks == walks == drains = 6776`, `distinct=813` — and it applies the base
vertex 2,258 times a boot, because `d3d_draw.cpp`'s `SetReg` is generic and the register
lands in its private file by construction.

**PHASE C PART 14 (2026-08-07, session 26): a resolve has a SOURCE, and it was the
blur — three of the four picture defects at once.** `docs/phase5-notes.md` §6ae and
`docs/d3d-translation-plan.md` §"Phase C part 14".

Part 14's list was the frontier at `#154`, then the blur, then the other three picture
differences. The first item dissolved on measurement and the second turned out to
contain the third and fourth.

- **`#154` was never a frontier, and the boot is not stalled.** `NtCreateFile`
  successes stop climbing because the title stops OPENING files, not because it stops
  loading: with `CZ_FILE_TRACE=1` the reads run on for another ~40 s out of `.big`
  containers it already has open — `npcs.big`, `cine_props.big`, `streamedassets.big` —
  through the prologue cinematic's props, ending with three `XMACreateContext` calls.
  Throughout, the ring chain is the healthy shape, `truncated=0`, frames keep presenting
  at ~1,200 draws each, and every `[wait]` is an idle worker or one of the two threads
  the title blocks by design (finding 41). Gotcha 206.
- **The blur was `RB_COPY_CONTROL`'s `copy_src_select`** — three bits this renderer read
  nowhere. **18.4% of this title's resolves copy the DEPTH buffer** (10,448 of 56,925 in
  B1, `tools/xtr_resolve_census.py`): three shadow cascades and the scene depth. The
  depth-of-field pass was therefore computing its circle of confusion out of the scene's
  own COLOUR, saturating it, and compositing full blur over every pixel at every depth.
  Two runs per arm, alternated, `CZ_VK_NO_DEPTH_RESOLVE=1` as the control: median
  mean-|gradient| **1.185/1.204 -> 7.640/7.666** (6.47x, no overlap), median distinct
  colours on the scene colour surface 72,740/72,711 -> **85,555/85,752**, frames per
  85 s 859/848 -> 803/811. **It closes §6ad's items 1, 3 and 4 together** — the missing
  `POP 753` and community-watch sign lettering and the absent bunting and gas-station
  signage were fine detail the blur erased — and moves item 2 (colour) a long way.
- **No aggregate over pixel VALUES could see it.** Coverage moved **0.01 pp**, inside
  `frame_compare.py`'s own 1.5 pp band, so this project's purpose-built renderer A/B
  metric reported "no detectable difference" about its largest visible defect. Gotcha
  135 in a second disguise, and `tools/frame_sharpness.py` is the instrument for it.
- **RETRACTION: `06BE4000` is the scene DEPTH.** It has been documented here as "the
  scene" since phase 5 and used as `CZ_VK_FRAME_STATS_SURFACE` for every renderer A/B
  in this port — and it held colour pixels only BECAUSE of the defect above. The scene
  colour is **`0684B000`** (`0685F000` for the second tile); the depth's tiles are
  `06BE4000`/`06BF8000`. Earlier measurements stand; the label did not (gotcha 205).
- **`CZ_VK_RESOLVE_TRACE_PASSES` did not exist**, for the second time in three sessions
  (gotcha 193) — and the budget it names still counted 60 HEADER lines while the two
  follow-up lines printed uncapped, which is the exact defect part 9's note says it
  fixed. Now real.

**Gates, PM4 arm, renderer on:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0,
2 windows, 0 real**; `truncated=0`; deepest file on a no-input boot **#83
`cinezombie.big`**.

**PHASE C PART 15 (2026-08-07, session 27): the prologue's black screen was THREE
defects and only two were ours — the renderer draws the prologue.**
`docs/phase5-notes.md` §§6af-6ag; hand-off in `docs/d3d-phase-c16-kickoff.md`.

Part 14 handed over "a live pass with live inputs produces black, the same SHAPE as
§6s". It is a stack, and the bottom of it belongs to the guest:

1. **A shader the cache did not have.** `vs_24e60d91249e6d04`, 351 dwords, loaded by
   the prologue and in NEITHER capture (A1 stops at the title screen, A2 is gameplay)
   nor in our own dump, whose recipe built from a plain boot that also stops at the
   title screen. **28,718 draws a run declined**, reported as one log line and a
   counter. Fixed (337 shaders now, and the recipe reaches the prologue) — **and it did
   not change the picture.** A real defect hiding behind a bigger one.
2. **The colour-grading LUT's resolve snapshot EXPIRED.** The rule was "taken this
   frame or last", which is right for a post pass reading an earlier pass of the same
   frame and wrong for a surface the title resolves ONCE. The title screen re-renders
   all three LUTs every frame so the window never bound; the prologue's grade is
   static, so the fetch fell through to guest memory, which is zero. Measured with (3)
   patched out: **0.00% -> 99.99% non-black, 1 -> ~89,450 colours**.
   `CZ_VK_SNAPSHOT_MAX_AGE=1` is the control arm.
3. **The rest is the GUEST asking for black, and the compose is faithful.** Printing
   the tone map's constants rather than reasoning about them: `pc(111).x`, the
   vignette POWER, is **0** at the prologue against 1.0 at the title screen, and
   `pc(110).w`, its strength, is **1.0** against 0.4. `pow(x,0) == 1` at every pixel,
   so the compose lerps 100% to `pc(110).xyz` = black. The LUT arithmetic constants are
   bit-identical between the eras. Proved with an arm, not argued: one line patched in
   `ps_114c4965eaabd54c` under `CZ_SHADER_SPV` takes the frame to **99.99% non-black,
   ~89,450 colours**, and the picture is the prologue's opening highway into Still
   Creek, tone-mapped and graded.

**What is actually wrong is upstream of the renderer.** `CZ_VK_FRAME_STATS` over the
black era: the **camera fingerprint is ONE constant value for 1,700+ frames** and the
scene surface's mean luminance is pinned at 104.484, while the draw stream still moves
(1,225-1,247 draws, 848k-883k vertices). The ring chain is the healthy shape
(`arms=11489 ints=11483 isr=11483`, `kicks == walks == drains`, `distinct=764`,
`truncated=0`) and every `[wait]` is an idle worker. The prologue is **stuck in a
faded-out state**. Leading hypothesis, stated as one: the cinematic is cued off audio,
the pump submits **55,808 driver frames of peak exactly 0.0000** because there is no XMA
decoder, and it waits forever.

**And one shadow-cascade defect of ours, with the title's own numbers beside it.** A
window-coordinate draw was mapped through the PRESENTED FRAME's 1280x720 rather than
the EDRAM's 1280x1024. The arithmetic is an identity either way so nothing looked
wrong; the CLIP is not, so every such draw taller than the screen was cut at row 719.
Cascade non-black **12.82% -> 13.28%**, which is the clipped 64x304 strip to the pixel
(`CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1` is the control arm). The rest of the empty half
is the title's: `CZ_VK_DRAW_PROBE` says its clear rects for a 1024x1024 cascade are
**`(0,0)-(480,512)` and `(960,0)-(1024,1024)`**, at z=1.0 with compare func ALWAYS.
**Shadows still do not appear** — committed on mechanism plus a matching structural
delta, which part 14's own rule says to declare.

**Gates, PM4 arm, renderer on:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0,
2 windows, 0 real**; `truncated=0`, 0 parser stalls; deepest file on a no-input boot
**#83 `cinezombie.big`**; presented frame 98.99% non-black at the title screen.

**PHASE C PART 16 (2026-08-07, session 28): four wrong answers removed from the
prologue, and part 15's own conclusion confirmed.** `docs/phase5-notes.md` §6ah;
hand-off in `docs/d3d-phase-c17-kickoff.md`. This session is mostly **negative
results** — each cost a build and a run, each has a same-binary arm behind it, and
that is what stops the next session paying for them again.

First, the timeline nobody had written down. Collapsing `CZ_VK_FRAME_STATS` on the
camera fingerprint turns "the run freezes" into four eras: the title screen (frames
1..591, 2,514 draws, a new camera every frame), the **loading screen** (596..962, ~150
draws, ~36% coverage, 4-5 cameras cycling), the world's first frame (974), and then
frozen from 1002 (1,225-1,247 draws, ~849,000 vertices, presented frame 0.00%). The
loading COMPLETED; the scene surface's mean luminance is pinned at **104.484 to three
decimals**, so the world is not merely hidden, it is not being simulated. The last
files opened name what the title was about to do: `#146 cinematics\cinematics.big`,
`#147 anim\cinematic\701_chuck_arrives_in_town.big`, `#148 skeleton\cineplayer.big`.
**It is sitting at the start of the first cinematic.**

- **NOT AUDIO — refuted, not merely unconfirmed.** Part 15's evidence was a peak
  amplitude of 0.0000, which is a fact about our OUTPUT that no guest code can see.
  The image states the real mechanism: `sub_8285EFE0` reads the XMA context's two
  input-buffer-VALID bits, `sub_82862A90` ORs them into IsPlaying, `sub_82864808`
  caches the answer at `voice+0x120` and branches on the transition. The guest sets
  those bits; the DECODER clears them — so with no decoder every voice ever started is
  still playing (measured: 284,373 polls, 284,354 "playing", **0 stop edges**).
  `CZ_XMA_NULL_DECODER` supplies the missing half, and **all three configurations of
  one binary give the identical frozen frame**: always-playing (stock), never-playing
  (instant consume, `playing=0/318,631`), and plays-then-ends (40 ms/packet, 19 start
  / 18 stop edges). Both polarities and the transition between them.
- **NOT A DEADLOCK.** `gdb -p` over all 31 threads, joined to guest tids by the
  always-on thread trace: exactly ONE thread is in guest code and it is the Draw Thread
  doing its ordinary per-frame GPU sync. The MAIN guest thread is in an infinite
  `NtWaitForSingleObjectEx` that `CZ_WAIT_TRACE` never reports — i.e. it is being
  signalled and re-entered, so the main loop is turning. `[kcall]`'s first-occurrence
  list ends at `XeCryptShaFinal`: **the prologue era reaches no new kernel import**, so
  the blocker is not a stub we have yet to write.
- **NOT OUR SYNTHETIC INPUT.** `CZ_FAKE_PRESS_SEQ` holds its last button forever, so
  every prologue observation this port ever made was taken while the title was being
  poked with A every 8 s. `NONE` now exists. Ten A presses then NONE — no input for the
  last ~170 s — reaches `#154` and the identical state.
- **PART 15 WAS RIGHT, and it was worth re-asking** (gotcha 172): a constant that is
  WRONG and one the guest never wrote look identical from inside a shader. Over the
  black era the guest writes `pc(110) = (0,0,0,1.0)` and `pc(111) = (0,0,0,0)`, **5,662
  times each with exactly one distinct value per register**. The full-black fade is the
  guest's and the renderer draws it faithfully.
- **The engine has its OWN log, and it is switched off.** `sub_827877C8` is a vsnprintf
  with **640 distinct callers** feeding one sink; `CZ_GUEST_LOG=1` hooks it. It prints
  nothing today and the zero is checked rather than believed — the call sites are each
  gated on a debug byte a shipped build leaves at zero. Raising them is the highest
  leverage item on the board (gotcha 215). `game:\cl.txt` is **not** the switch: it is
  read as a CHANGELIST NUMBER.
- **One real defect, recorded rather than fixed.** `VfsTranslate` returns empty for any
  path with no `:`, so a guest path with no device prefix can never resolve. A boot
  makes 29 such opens (`data\anim\weapon\<Weapon>.big`); none of those files exist under
  any prefix, so nothing is currently lost.

**Gates, PM4 arm, renderer on:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 2
windows, 0 real**; both capture oracles clean; `truncated=0`, 0 parser stalls,
`max=2`; `no translated shader` = 0; deepest file on a no-input boot **#83**.

**OPERATOR SESSION ON THE PART-16 BINARY: GAMEPLAY IS REACHABLE, AND THE PROLOGUE
FREEZE IS "CINEMATICS NEVER END".** `docs/phase5-notes.md` §6ai. The most informative
hour this port has had, and no instrument produced it. The operator **skipped both
prologue cinematics and played** — Zombrex tutorial card, watch/MESSAGES screen, Still
Creek, combo weapons. A combo-weapon cutscene then parked the camera on the workbench
with no HUD while Chuck still took input, and **skipping that cutscene restored the
camera**. So:

> **Every cinematic in this title starts and never ends. Skipping is the only exit,
> and the skip path works perfectly.**

The prologue's black screen is that defect wearing §6af's fade. Part 16's four negative
results all stand and are now EXPLAINED rather than merely true — a cinematic that never
advances asks nothing of the kernel, blocks no thread, and does not care whether audio
finishes. **The skip path being clean is the strongest clue on the board**: it runs the
same teardown a natural end would run and it demonstrably restores camera, HUD and
control, so the teardown is fine and only the TRIGGER is missing. A alone does not skip.

And the first like-for-like GAMEPLAY comparison this port has been able to make. **The
HUD is NOT a defect** — an indoor frame missing most of it was written up as one and
retracted within the hour when it appeared in full outside; the safehouse just has not
raised it yet, and capture E4 is a LATER first-gameplay frame than the one it was being
compared against (gotcha 127, applied to a whole screen rather than a metric).

**The colour is**, and the exterior names it far better than the interior:

| | hardware | ours |
|---|---|---|
| safehouse interior (vs E4) | warm red/brown wood, bright orange shirt | green-shifted, blacks crushed |
| Still Creek exterior, daylight | pale hazy blue sky | **the sky is PINK/MAGENTA** |

Chuck's orange shirt, the red car and the yellow LIFE pips are all CORRECT in that same
frame, so this is not a tint or an exposure error — those would move the shirt too. A hue
error that spares saturated reds and yellows while turning a pale blue sky magenta and
the mid greys green is the signature of a **wrong colour-grading LUT**, which is exactly
what §6s proved this frame depends on completely and §6af caught silently expiring. It is
item 6 below, at last visible somewhere it cannot hide.

**PHASE C PART 17 (2026-08-08, session 29): the world is reachable headless, the frame
is PROFILED, and the renderer is not where it goes.** Hand-off in
`docs/d3d-phase-c18-kickoff.md`.

* **Analog sticks in the synthetic-input arm.** `LSUP/LSDOWN/LSLEFT/LSRIGHT` walk Chuck,
  `RSUP/RSDOWN/RSLEFT/RSRIGHT` aim the camera; a stick entry HOLDS for its interval where
  a button entry taps for 150 ms. Verified: `LSUP` walks him from mid-safehouse to the
  door and raises its "Open (B)" prompt, `RSRIGHT` swings the camera ~90°. This closes
  the other half of gotcha 190 — menus were reachable headless and the WORLD was not, so
  every gameplay defect on the board was an operator report with no reproduction.
* **THE FRAME RATE IS NOT THE RENDERER'S, and this is measured rather than argued.**
  `CZ_VK_PROFILE` on gameplay: **the entire renderer is 8.6% of an 87 ms frame**, against
  33% GPU wait and 58% guest code + command processor, at 134% process CPU. Making the
  renderer instant buys ~1.09x. **My own prime suspect before measuring — the per-draw
  constant upload, 8 KB x ~1,900 draws = ~19 MB a frame — is 0.5%** (gotcha 80: write the
  oracle before the theory, and this session did it backwards).
* **One real win: the readback buffer was WRITE-COMBINED.** Presenting a frame read
  3.7 MB back uncached at ~230 MB/s. `readback` 15.7% -> 0.4%, frame 103 -> 87 ms, 9.7 ->
  11.5 fps headless and ~10 -> ~15 windowed. `CZ_VK_READBACK_UNCACHED=1` is the control.
* **`CZ_VK_SNAP_ON_BLACK`** captures the view-dependent black by itself. The operator
  produced its first BEFORE/AFTER pair, and the DIRECTION names the mechanism: a
  degenerate auto-exposure that saturated would go WHITE, and this goes BLACK — which is
  what `exposure = key / averageLuminance` does when the average is enormous or
  non-finite, with an enormous blown-out glow sitting in the "before" frame.
* **Two retractions, both from ordinary play** (the part-17 pattern, again): the Still
  Creek **pause menu is pixel-correct**, and **Dick renders**. Re-check Fausto's legs and
  Gemini's hair before working item 3d.
* **The save's failure MOVED rather than persisting.** `XUserWriteAchievements` no longer
  returns E_FAIL and `XamContentCreateEx` mounts. Its `XamContentClose` is ~10 lines
  later with no file activity between — **which is NOT evidence that nothing was
  written**, because `NtCreateFile` successes print only for the first 512 and then every
  64th and that run was thousands of opens deep (gotcha 109). One save with
  `CZ_FILE_TRACE=1 CZ_SAVE_PROBE=1` settles it.
* **Operational, and it cost the session's best evidence: `/tmp` is a TMPFS on this
  machine**, 32 GB, and it was sitting at 24 GB of scratch from earlier sessions. A full
  one makes `cz_runtime` runs fail with `write error: Disk quota exceeded`, which
  presents as the SHELL DYING seconds after a run, and it stopped the operator saving
  screenshots — losing both frames of the black-transition pair. `df -h /tmp` before a
  run, not after a mystery. Note the misdiagnosis too: this session's own ~1 GB of frame
  dumps were blamed for the whole thing, and deleting them changed the total by 4%.

**PHASE C PART 18 (2026-08-08, session 30): the frame rate was 11.8 fps because the
graphics pump was ASLEEP for 57% of every frame — it is ~29 fps now, and no work was
deleted.** `docs/phase5-notes.md` §§6aj-6am; the plan executed is
`docs/perf-plan-overnight.md`.

Its §1 said to attribute the 58% that `CZ_VK_PROFILE` calls `outside` before optimising
anything, on the grounds that it contains four different investigations. It contains
one, and it is not work.

* **`perf` says the CPU is a guest busy-wait, and that is a red herring.** 38.1% of all
  cycles in `sub_8283C6C8` and 21.3% in `sub_82845160` — finding 38's ring-progress
  spin, 73% of the process with the ladders, 77.6% in a single thread — while the thread
  running the command processor AND the whole renderer uses 0.14 of a core. A cycles
  profile cannot see the thread that decides when a frame ends, because it is asleep
  (gotcha 218).
* **`gpu/pump_stats.h` can: 3.00 pump ticks per frame, every window, and 57% of the wall
  clock in `sleep_for(16 ms)`** — 48 ms of an 85 ms frame. The walk stops at every
  unsatisfied WAIT_REG_MEM and resumes on the next tick, so each hand-off wait cost a
  whole sleep period.
* **`CZ_PM4_TICK_MS` splits the ring tick from the vblank: 84.4 -> 69.9 ms (1.21x)**,
  with `submit` identical at 24.0 ms on both arms, i.e. the entire delta is sleep.
* **`CZ_VBLANK_TICKCOUNT` names a defect older than phase C: the vblank has never been
  60 Hz.** It was delivered every N loop ITERATIONS, and an iteration is a sleep plus a
  ring walk — so the GPU's wait pushed the guest's vblank out. 40.2/s on the old loop,
  31.2/s once the ring ticked faster, **62.2/s on a deadline**, and 2.0x, because the
  CP's per-frame waits are released by the swap-queue walker inside that very ISR.
* **The GPU has been at 210 MHz of 2100 for every measurement this port has ever made**
  (gotcha 219). `submit` split into `[call 0.1% gpu 35.4%]`, so there is no host-side
  driver cost — and `nvidia-smi` says P8, 15.7 W of 240 W, reason "Idle: Active".
  **This retires the overnight plan's §2a and §2c until one root command answers it.**

**AND THEN THE OPERATOR PLAYED IT, which moved the whole problem.** A Still Creek
zombie crowd renders **4,800-6,800 draws a frame** against the headless recipe's 1,930 —
3.5x the workload every conclusion in this port had been based on (gotcha 222). Three
results:

* **The GPU clock question is ANSWERED.** `sudo nvidia-smi -pm 1` + `-lgc 2100,2100`
  takes the card from P8/210 MHz to 1950, and the GPU term in a crowd frame from
  **18.8 ms to 6.64 ms — 2.9x, now the SMALLEST term**. Crowd fps 15-25 -> 22-25.
  Invisible everywhere else, because everything else is on the title's own cap.
* **Per-draw state binding is cached** (`CZ_VK_NO_STATE_CACHE=1` is the arm): 11.4% off
  `record`, engagement 76-99.9% against a 0.0% negative control. Small — 1.6 pp of an
  ordinary frame, ~3% of a crowd — and recorded as small.
* **A crowd frame is now 75% our own CPU**, in two roughly equal halves: renderer draw
  path 21.4 ms and PM4 walk 11.0 ms, both linear in the draw count, neither ever
  optimised. **`docs/perf-cpu-plan.md` is the plan**, and its item 0 blocks the rest —
  the headless recipe cannot reach a crowd, so every A/B would run against the vblank
  cap where a CPU saving measures as exactly zero.

**Gates:** `--smoke` OK; A5 **exit 0, 0 real windows**; `truncated=0`; `no translated
shader` = 0; deepest file **#83 `cinezombie.big`**; swap queue one record in flight
(head 8547 / tail 8548), not a queue nobody drains; the picture unchanged — a dumped
boot's `frame_003456` matches capture E2 at **+0.959, identity orientation**.
**The cost, stated: A1's position-71 window now permutes on every run** (1 of 10 -> 5 of
10 -> every run, over the two changes). Same six-name two-thread interleave, mechanism
in §6ak, and `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` restores the exact 84-prefix in
one command — but `kernel_call_diff.py` refuses to relax the masked gate for
permutations on purpose, so it is a real loss and not a technicality (gotcha 221).

**PHASE C PART 19 (2026-08-08, session 31): the port's top rendering defect was a
128 MB allocator, the save could not write, and the headless recipe now reaches the
outdoor world.** `docs/phase5-notes.md` §§6ao-6ap, `docs/phase3-notes.md` finding 52.

The part-18 kickoff's list, worked in its own order. Item 1 was already done (part 17
committed and controlled `CZ_VK_SNAP_ON_BLACK`); item 4's performance section was
superseded by the overnight session before this one started.

* **THE VIEW-DEPENDENT WHOLE-FRAME BLACK IS SOLVED, and the auto-exposure hypothesis
  that led the board for six parts is REFUTED.** It is the renderer's per-frame bump
  ARENA overflowing: `ArenaAlloc` skips every draw it cannot satisfy, this title's post
  chain is at the END of the frame, and the arena was a fixed 128 MB against a true peak
  of 161. A frame with enough geometry in it therefore lost its downsamples, its
  luminance ladder, its three colour LUTs and its tone map, and presented black with a
  correctly rendered scene sitting in EDRAM behind it. "View-dependent" was literal:
  which way the camera points decides how much geometry is in the frame.
  **One binary, two arms: 128 MB gives 160 black frames of 8,216 gameplay frames,
  512 MB gives zero — and every one of the 160 is the frame immediately after an
  `arena EXHAUSTED` line, 160 of 160.** The arena grows now rather than being a bigger
  number (`CZ_VK_NO_ARENA_GROWTH=1` is the control); what is NOT closed is the
  consumption, ~27 KB a draw, 8 KB of which is the constant block `perf-cpu-plan.md`
  §1a-D already wants deduplicated for an unrelated reason.
* **The resolve-chain dump is what named it, and the pair is why.** `CZ_VK_SNAP_ON_BLACK`
  fires on COVERAGE and in a gameplay run catches only loading screens, which are
  legitimately black. `CZ_VK_SNAP_ON_DARK` fires on mean luminance and **dumps a BRIGHT
  REFERENCE chain from the same location seconds later** — one dark chain is equally
  consistent with "this pass is broken" and "the scene really is dark here" (gotcha 133
  turned into an instrument). Read side by side, the black frame's scene colour was
  98.4% lit at mean 35.7 and every downstream surface was identically zero. That is what
  ruled the tone map, the exposure and the grade out: they are victims in the same list.
* **A second, independent renderer defect found on the way: a snapshot is PITCH-sized
  and a fetch is WIDTH-sized.** Where those differ every texture coordinate is scaled by
  width/pitch, which is invisible on full-screen surfaces and compounding on a reduction
  ladder — and the 2x1 scene-average luminance the tone map reads was identically ZERO
  in every frame of every era. Predicted lit-column counts from that one ratio match five
  consecutive links exactly, before and after. Fixed with right-sized views (9 created in
  a boot, refreshed free inside the resolve that writes their source), after a counter
  said the mismatch is 3.3% of fetches rather than a general problem needing a general
  mechanism.
* **THE SAVE WORKS — end to end, and cross-checked against the hardware save.** The
  operator played to a save point on this binary and the title reported **"Game saved
  successfully"** with the slot panel filled in and a rendered thumbnail. The log is A3's
  exact sequence (`XamContentCreateEx -> mounted`, `NtCreateFile ... WRITABLE,
  disposition 5 (created)`, `NtWriteFile ... 303104 written`, `XamContentClose`), and the
  file on disk is **303,104 bytes with bytes 4..31 IDENTICAL to A3's real 360 save** and
  the same non-zero region boundaries; only the leading four bytes differ, which is a
  checksum or a timestamp. That RETRACTS open-items 1b: the unhandled XGI message it
  blamed does not appear in the successful run at all — it was fixed by part 16 and the
  item outlived the defect. What is still untested is the LOAD half, and it is now a
  better test than it has ever been, because the save on disk is OURS rather than A3's
  (whose foreign profile GUID was the confound under open-items 2's `Damaged Content`).
* **AND IT LOADS. The save round trip is closed** — the first title state in this port
  that survives a process exit. The first Load attempt failed with **"Load failed. File
  appears to be corrupt."** with the file untouched on disk, and the log named the real
  cause: `XexGetProcedureAddress ord=0x271 -> NOT_FOUND`, and **the file was never
  opened**. The title's own error text named the wrong subsystem, which is what three
  sessions of reading `Damaged Content` as evidence about the save's CONTENTS had been
  worth. **Ordinal `0x271` is `XamContentCreateInternal`**, named from A3's own load-back
  and from the guest's call site rather than from an ordinal table — `sub_825D8F30`
  builds an `XCONTENT_DATA` on its stack with a **42-byte** file-name field, which is
  exactly `XCONTENT_DATA::szFileName`, and passes flags 3 = `OPEN_EXISTING`. It is the
  same mount `XamContentCreateEx` already did, so the two are now one shared body.
  **This CLOSES open-items 2 and retires its per-profile-signature explanation as
  wrong**: A3's save was never ours to load. And `kResolvable` is no longer "the seven
  A1 resolves" — A1 was captured with an EMPTY save root, the one configuration in which
  the load path never runs (gotchas 45 and 106, for the second time).
* **THE SAVE COULD NOT WRITE, IN TWO INDEPENDENT WAYS, and neither was visible.**
  `NtCreateFile` ignored `createDisposition` entirely and opened every handle `"rb"`;
  `NtWriteFile` was a generated honest-failure stub. Either alone produces exactly the
  symptom part 17 recorded, which is why the symptom could not discriminate. Both are
  implemented from A3 (six dispositions, mode from the guest's own access mask,
  `IO_STATUS_BLOCK.Information` per outcome, and the write path's EVENT completion where
  the read path uses an APC). A THIRD defect only a test could find:
  `CZ_FILE_WRITE_SELFTEST` wrote 303,104 bytes and then could not re-open them, because
  `VfsResolveExisting` caches NEGATIVE results and the create's own existence check is
  what caches the "no". **What is still not known is whether the title's save completes**
  — no headless recipe reaches a save point — but every file operation off `game:`/`d:`
  is now logged uncapped, so the printer can no longer be the reason for a silence.
* **`docs/perf-cpu-plan.md` item 0 is CLOSED: the headless recipe reaches the outdoor
  world at 6,400-8,100 draws a frame**, past the operator's 6,592. One extra `B` at the
  safehouse door and alternating `LSUP` with `RSRIGHT`/`RSLEFT`. It is in `CLAUDE.md`
  beside the safehouse recipe, and it is what reproduced the black in the first place —
  the same argument the plan made for why it had to come first, paid off twice.
* `tools/snap_dump_stats.py` summarises a 61-surface snapshot dump as one line per
  surface, because "open them and look" is gotcha 133 applied to a directory.
* One shader recovered from a run that reached new ground; the cache is **371**.

**Gates:** `--smoke` OK; A5 **exit 0, 0 real windows**; `truncated=0`; deepest file
**#83 `cinezombie.big`**; `no translated shader` = 0; the picture matches capture E2 at
**+0.9597, identity**, against the pre-fix arm's +0.959; median scene coverage inside
the 1.5 pp band across the renderer change.

**And a correction to part 18's gate note, paid for by running it twice.** Part 18
recorded that `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` "restores the exact 84-prefix in
one command". It does not, reliably: two runs of that exact configuration this session
gave an exact prefix once and the familiar position-71 rotation
(`XamUserCheckPrivilege` / `XexGetModuleHandle` / `XexGetModuleSection`, three names,
same set) the other time. The flags REDUCE the interleave's frequency; they do not
remove it, and `kernel_call_diff.py` exits 1 on the run where it appears. That makes
every single-run "exact prefix" claim a coin flip (gotcha 159), which is exactly what
gotcha 221 says about this window and what part 18's own wording softened. The
set-based A5 gate is the one that holds, and it holds.


**PHASE C PART 20 (2026-08-08, session 32): the profiler was counting nested phases
twice, and the CPU plan was ranked on the result.**

The session began where `d3d-phase-c19-kickoff.md` points — `docs/perf-cpu-plan.md`,
"now fully runnable and the biggest single item" — and the kickoff's own warning is what
it found: **re-measure before optimising.** Not because the numbers were stale, but
because they were wrong.

* **`ProfScope` accumulated INCLUSIVE time and the scopes nest.** `record` opens partway
  down `DoDraw` and lives to the end of it, so the `UploadStream` calls below it ran
  inside it: their cost landed in `streams` AND in `record`. The frame print then derived
  DoDraw's residual by subtracting the named phases from the whole, removing `streams`
  twice. `record` read 11.07 ms of a 21.40 ms draw path and the residual read 0.91 —
  where the true split is 6.30 and 5.68. **The plan's §1 and §2 are ranked by expected
  size, so the ranking was wrong**, and it filed its second-biggest item as "the cheapest
  item in this document".
  What makes this class hard is that nothing looks wrong: the error MOVES time out of the
  outer scope's residual into an inner scope's name, so every column still sums to the
  total and the `outside` column — which exists to show what the instrument cannot
  account for — is unaffected. Gotcha 228, whose corollary is that a profiler is
  instrumentation and gotcha 30 applies to it: break it on purpose and check the columns
  move as predicted. Neither this port nor the two before it had ever done that.
* **The corrected crowd frame, re-measured at 6,737-6,806 draws** (P8/210 MHz of 2100 —
  no passwordless sudo this session, so the fence column is inflated ~2.9x and nothing
  else is): `record` 6.7 ms, `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3,
  draw path **19.9 ms (36.5%)**; PM4 walk **11.8 ms (21.6%)**, unaffected by the defect
  because it was always derived from the renderer's INCLUSIVE total. `[vkprof]` now
  prints the walk's own cost as a `pm4` column instead of leaving it to be subtracted by
  hand.
* **The per-draw path was carrying its own instrumentation**, which gotcha 223 had warned
  about in this project the day before: five `Count()` calls (each a `std::string`
  construction and a red-black tree walk), four `getenv` calls for instruments nobody had
  enabled, and one `snprintf` formatting the draw's shader hash for `[psbind]` — **above**
  the gate that tests whether `CZ_VK_PSBIND` is set. That last is its own rule now
  (gotcha 230): `docs/instruments.md` promises every arm is "free when off", and a gate
  around the `fprintf` does not deliver that when the work feeding it sits outside the
  gate. `COUNT(literal)` resolves each counter's ADDRESS once per call site into a
  function-local static; names, order and the stats block are untouched. At matched draw
  counts: **`record` −47%, `other` −49%**, four points of frame time.
* **§1a hypothesis A measured and mostly refuted.** The plan's leading idea for `record`
  was that a crowd rebinds the same vertex buffer over and over, and it prescribes
  counters before code. In the crowd era **34% of vertex binds and 22% of index binds
  repeat the previous offset** — ~1.3 of the ~6.4 `vkCmd*` calls a draw, worth ~1.4 ms of
  a 54 ms frame. Real, about a third of what the hypothesis expected, and below the
  noise floor, so it can only ever be claimed from the counter. Not acted on, as asked.
* **THE NOISE FLOOR, which is the most transferable thing here.** `tools/frame_perf_bins.py`
  compares two `CZ_VK_FRAME_STATS` files BINNED BY DRAW COUNT rather than averaged over a
  run, because the crowd recipe is 57 fixed 8-second steps against a boot whose depth in
  wall time is a distribution — so a whole-run mean is dominated by the capped safehouse
  era. That binning is necessary and **it is not sufficient**: two runs of ONE binary
  disagree by 10-13% in exactly the crowd bins, with the tool's own standard-error column
  reading up to 22 sigma, because consecutive frames in a bin share a camera and a
  location and are nowhere near independent samples. Gotcha 229: run the A/B with nothing
  changed first, and treat what it prints as the floor.


---

**PHASE C PART 21 (2026-08-08, session 33): the stream cache is 94% hits and still
copies 74 MB a frame, and the arena growth was being charged to the draw path**

Two items off the part-20 hand-off's ordered list, both of which it had ranked from the
operator session. Neither is a speculative change: item 1 was explicitly "count before
writing anything", and item 2 was a call-graph observation with a measured spike.

* **`perf-cpu-plan.md` §1b is ANSWERED, and it answers the opposite way to the guess in
  its own text.** The plan set up the ambiguity correctly — nearly all hits means the
  cost is the lookup and the fix is a cheaper key; mostly misses means real copying and
  the fix is a different cache lifetime — and said no reading of the code could decide
  it. `CZ_VK_STREAM_CENSUS=1|2` decided it. At ~6,400 draws in a ~50 ms crowd frame:
  **93.6-94.0% hit rate within a frame, and still ~2,000 misses copying 74-77 MB every
  frame**, `streams` 11.3-11.7% = 5.6-5.9 ms, with **95-97% of the copied bytes
  repeating the previous frame's key**. Vertex bindings 61-63 MB, dependent fetches
  11 MB, index buffers 1.8 MB. So it is real copying, of the same buffers, thrown away
  at every frame boundary — the fix is the cache's LIFETIME, and it is worth ~11% of a
  crowd frame.
  Half the ambiguity had in fact been answerable by reading nine lines, which is worth
  saying because the plan said it was not: `ProfScope(&g_prof.streams)` wraps only the
  `CopySwapped`, so a cache hit never touched that column at all and its lookup was
  charged to `other` the whole time. Gotcha 233 is the general form — a hit RATE and a
  byte COST are different questions, and 94% sounds like the copying is gone.
* **The content check needed a control, and the control changed the answer.** A
  persistent cache is only correct if the bytes at a repeated key are the same bytes, and
  level 2's hash comparison read **100.0%, in every window of every run**. That is
  indistinguishable, from the output, from a comparison whose two sides are equal by
  construction. `CZ_VK_STREAM_CENSUS_POISON=1` salts the hash with the frame number so
  identical bytes must hash differently: it reads **0 of 96,048 (0.0%)** on the same
  binary, against 75,492 of 75,492 unpoisoned. With the control in place the unpoisoned
  arm reads honestly too — **164 of 10,154,820 repeated keys DO change content**, a
  recurring set of ~26 — so the design conclusion moves from "safe to cache blindly" to
  "must invalidate". Gotcha 234, which is gotcha 30 in the shape it takes for equality
  checks, where it hides best because the passing state is silent.
* **The arena growth ran inside `DoDraw`.** `BeginFrame()` is called from there, and the
  growth's `vkDeviceWaitIdle`, allocation and map were charged to the draw path's
  `other` — the 29.8% single-frame spike the operator session measured. Moved to the end
  of `DoSwapImpl`, after the fence wait, which satisfies the old comment's own stated
  safety condition more directly than the old site did. **Stated as a measurement fix,
  not a performance one**: verified against the pre-registered prediction that the
  black-frame count per growth is UNCHANGED at one, and it is (pre-move: exhausted 1817,
  black 1818, 6 of 13,410; post-move: exhausted 1258, black 1259, 7 of 5,387 — the same
  four early-boot frames otherwise, at identical draw counts). The frame that overruns is
  lost either way. Closes the last live piece of open-items 1c.

**Gates:** `--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean (24,527,474 packet lengths
agreeing, every indirect buffer tiling exactly). The picture was not re-checked against
capture E — nothing this part touches what is drawn — and that is an argument rather than
a measurement, as it was in part 20.

**What this part deliberately did NOT do.** The cross-frame cache itself. It needs
storage that outlives the frame, an invalidation mechanism that is not hashing (the
candidate is guest-page write tracking), and counters for both; the hand-off says so and
sizes it at a session. Measuring first and stopping is what the plan asked for, and the
0.0016% mismatch is exactly the fact that would have been discovered late and expensively
by writing the cache first.

## Part 35 (2026-08-12) — the operator re-asks the parked picture items; five theories
## die; the striped-material class gets its writer hunt and its oracle in one night

An operator-driven session (the most productive picture hunt of the port) plus the
same-night delivery of round-3 hardware traces. Full records: `docs/phase5-notes.md`
§6bi, `~/DR2CZ-troubleshooting/part35-item1-operator/`, `Xenia logs/R3_world/`.

* **Item 3d (NPC part meshes) CLOSED**: Dick renders whole on two binaries — the
  missing parts were the shader-cache gap, as the item's own re-test note predicted.
* **Item 00i captured**: the same shop as flat colour panels at street-across distance
  and full siding close (reload_test 30631/30807); one deliberate Xenia look owed.
* **Item 0s created — the striped-material class**, with the evidence bounded from
  both sides. Guest memory genuinely holds the banded garbage at blotch time (live
  dumps seconds after each F9 — `tools/live_texdump.py`, gotcha 285), and five
  reader-side theories died by measurement: the shadow term (atlas 0.0006% zero at
  the blotch frame, patches stick under a strafe); a VFS positional-IO race (fixed on
  principle in d65874d — seek-then-read was never atomic as NT requires — but the
  overlap counter read 0 across two sessions and the commit's registered prediction
  is retracted in §6bi); "the tanker wears a pickup's atlas" (misattribution — a real
  pickup was in frame); the snapshot age fallback (cannot fire, no age limit); the
  texture cache freezing changed content (content guard: 4 stale of 92,730,622 hits,
  4 re-uploaded). The affected textures include CPU-composed impostor sheets — odd
  extents, DXT5, 4-vertex quads — that exist nowhere on disc and are never resolve
  destinations. **Every reader is exonerated; part 36 traces the writer**
  (gotcha 286), with the resolve-to-guest-memory writeback gap the standing suspect
  for the sources.
* **R3_world landed the same night**: four single-frame `.xtr` traces with
  FRAME-LOCKED screenshots (the operator's fork now captures the guest framebuffer at
  the F4 press) at exactly the four defect sites. Hardware clean at all four — and
  hardware's tanker is CREAM, so the skin recorded as "changed after a reload" was
  the correct one. Each trace carries the bytes the sheets should hold: the writer
  hunt's ground truth, in hand before the hunt.
* Gotchas 284 (an end-of-run report does not survive the recipe's timeout kill —
  prove engagement differentially), 285 (a live-process dump has a time), 286 (stop
  instrumenting readers, trace the writer).

## Part 34 (2026-08-11) — the 4x MSAA Y factor ships as the default, and the exposure
## question dissolves

Part 32's item 0, executed. Full record `docs/phase5-notes.md` §6bh.

* **The Y factor for 4x MSAA window coordinates is the DEFAULT**, with
  `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` the same-binary control arm and the part-32 arm
  variable retired. The reconciliation that unblocked it: a clear rect is in the CLEAR
  declaration's own pixel space, so both axes scale by the draw's OWN declared sample
  factors — no rule needs the render declaration at clear time, and the Y over-clear
  past a shorter surface is the same approximation the X factor has always applied to
  the 640x360 post surface.
* **Gates**: title-boot atlas 46.8750% -> 0.0038% zero, 512 -> 1024 covered rows
  (§6bf's arm numbers to four decimals, arms differing by the env var alone); no other
  surface regressed — the scene and post chain gain luma and colours, the direction a
  live shadow term predicts; outdoor era medians distinct colours **+8.30% at 5.2x the
  null** (registered prediction, commit e10df05); validation tally unchanged (zero
  08733, the 6 pre-existing topology-08773); capture-E +0.958 identity (statistically
  the recorded +0.9597 — the fixes do not live on a 2D title card); both PM4 oracles
  exit 0; A5 kernel diff and `truncated=0` re-run clean.
* **§6ba's owed exposure re-measure came back "no discrepancy left"**: all three arms
  identical, frame 3000 reads 0.211 (part 31: 0.2146), era range 0.200-0.354 with mean
  0.2755 — and hardware's 0.298/0.331 sit INSIDE that adaptive range. Owed: only a
  matched-location comparison, free on the next operator session.
* **The operator's three-way verdict landed the same day and closed open item 3**: one
  Case 0-2 crowd spot, F9 per arm — default *"perfect"* (atlas 0.0006% zero at the
  capture), both control arms showing hard-edged black false-occlusion blotches on the
  same truck (46.875% / 75% atlas zero). At close range the control defect presents as
  blotches on nearby geometry rather than the boundary line — same defect,
  distance-dependent presentation. `no translated shader` 0 in all three arms.

## Part 33 (2026-08-11) — the white plateau was NaN, the NaN was a vertex-input type
## mismatch (entry written in part 34; the part-33 session recorded itself only in
## §6bg and its kickoff)

Item 00f, open since part 26, closed both ways in a day. Full record
`docs/phase5-notes.md` §6bg; gotchas 281-283.

* The exact-`rgb(180,180,180)` surfaces were the shared tone epilogue evaluated at
  `x = NaN` — `max(NaN,K1)=K1`, `saturate(NaN)=0` land every NaN on `sqrt(K1*K2)` =
  180/255, invariant under every constant, which is why four whole-frame arms read
  "unmoved". The NaN entered at the VERTEX FETCH: fmt16 `k_10_11_11` packed normals,
  TEXCOORD-wrapped as float4, bound `R32_UINT` — a VUID-08733 type mismatch delivering
  packed bits AS floats since phase 5. One `CZ_VK_VALIDATION=1` run named it.
* Fixed in XenosRecomp (`XeUnpack_10_11_11`, static format branch) + runtime (fmt16 ->
  `R32_SFLOAT`): plateau 1,092 px -> 0, scene mean luma 35.5 -> 44.7, distinct colours
  80k -> 112k, 08733 10 -> 0. The NaN footprint was 17x the visible plateau, so every
  fmt16 mesh had garbage normals since phase 5 — the parked picture items were told to
  re-ask on this renderer.
* **The operator confirmed it the same day: seven for seven part-27 locations, scene
  plateau ZERO in every one** (`~/DR2CZ-troubleshooting/part33-operator/`).

## Part 32 (2026-08-11) — the cascade's other half, and a hardware oracle that was a
## photograph

**Part 31's hand-off recommended the shadow tail past the last cascade split. It is not
there.** Our translation of that fade is instruction-for-instruction the guest's, and the
arithmetic works out. The remaining shadow defect is one layer up: **half of every cascade
band holds zero, and a zero depth sample reads as OCCLUDED.**

The atlas is **46.8750% zero in every band** — rows 0..511 populated across every column,
rows 512..1023 only in the last 64 — on two routes and two frames, in four bands rendered
from four different light frusta by 108/87/221/35 draws. 15/32 exactly is not scene
content. Three arms, in order:

* `CZ_VK_DEPTH_ALWAYS=1` -> **1.86% zero**. The geometry is submitted for the whole
  1024x1024 and the bottom half is REJECTED, against the zero the image was created with.
  (`CZ_VK_NO_DEPTH_TEST`, the arm that exists for this, returns 100% zero on a depth-only
  pass because Vulkan ties depth writes to the depth test — gotcha 279.)
* `CZ_VK_DEPTH_CLEAR_FAR=1` -> **0.0113% zero**. The input is the clear VALUE.
* `CZ_VK_SCOPED_CLEAR=1` -> **46.8750%**, a registered prediction refuted to four
  decimals: the region is not wiped by another pass, it is never cleared by anybody.

**Then the derivation.** `CZ_VK_RECT_TRACE=0` prints every rect-list clear with its
surface pitch and MSAA mode, and locates the rect part 15 recorded as the cascade's:
`(0,0)-(480,512)` on a **520-pitch 4x MSAA** surface, beside `(960,0)-(1024,1024)` on the
1040-pitch one. 520 x 2 = 1040 is the cascade's own sample pitch, and Xenos 4x is a 2x2
sample grid. Scale both axes and the two rects tile 1024x1024 EXACTLY; scale X only —
which is all this renderer has ever done — and the union is 557,056 of 1,048,576 =
**53.125%**, the observed coverage. `CZ_VK_MSAA_WINDOW_SCALE_Y=1` -> **0.0038% zero** with
the title-screen picture unmoved. Left off by default: the scene tile's 4x clear wants X
scaled and Y not, and two 4x surfaces asking for different things is two data points.

**THE RETRACTION, and it is the part worth copying to another port.** §6bc measured our
atlas against "hardware's copy of the same surface, 3.5% zero", dumped from `w1_spawn`
with `xtr_draw_bindings.py --dump-texture 1812F000`. Those 16 MB are the **previous
frame's composited scene** — detiled, the game's own HUD is legible in the "shadow map".
A `.xtr`'s memory records are snapshots with a time: Xenia dumps the bytes behind a
resource the first time the GPU reads it and never again, so for any address the title
resolves into during the traced frame the only snapshot predates the surface. There is one
chunk covering `1812F000`, at walk position 39; the first resolve into it is at 3522.
Gotcha 280 corrects gotcha 275's second half. The fix it was quoted alongside stands — it
rests on register values and an operator verdict — but the yardstick never existed, and
the right target was the surface's own definition all along: a shadow map's unwritten
region must read FAR, so 100%.

The tool gates itself now: it reports how many snapshots cover the range and when they
arrived, whether the address is a resolve destination, and **exits 2** when every snapshot
predates the first resolve. Checked both ways — part 27's ground-texture comparison prints
*"a sound oracle"* and is unaffected.

## Part 31 (2026-08-11) — the shadow atlas is fixed, and the white plateau's model is
## retired

**The comparison part 30 left owed, finished.** All seven R2 captures answer for the
ground shader `ps_ad65b98593f95926` — part 27's "`w1_spawn` cannot" is true only of
`c253..c255`, whose `LOAD_ALU_CONSTANT` reads memory the trace does not carry. Every one
of the 32 constants that is not a function of the camera is **hardware's to the printed
digit**, and the run proved it was in hardware's own lighting state without being asked
to: `pc(21)`, a point light's WORLD POSITION, reads
`(-149.4081, 6.2343, -106.2974, 0.7400)` against hardware's
`(-149.408096, 6.234319, -106.297379, 0.740000)`. The registered prediction — that
`c20`, `c24` or `c67` would disagree, with a non-zero `c20.x` sending the shader down an
unlit path — is refuted. The constants are exonerated as a class.

**THE SHADOW ATLAS DEFECT, open since part 15, is found and fixed.** The one input to
that draw nobody had compared was the shadow map, because we render it rather than load
it — and it turned out to be comparable anyway: the capture carries the consumer's copy
as a `MemoryRead`, and `xtr_resolve_census.py` prints the title's own resolve
destinations. Ours was **86.7% zero**, hardware's **3.5%**. The title packs four
1024x1024 cascades into one 4096x1024 atlas by pre-offsetting `RB_COPY_DEST_BASE` by
0x20000 each while leaving the window scissor at the origin, and 0x20000 is exactly
+1024 texels in X in Xenos tiled address space (a 32bpp macro tile is 4096 bytes; a
4096-wide surface is 128 tiles per row; +32 tiles). `DoResolve` un-offset the SCISSOR
form of that idiom and derived the offset from the scissor, so for the cascades the
subtraction was a no-op and the four became four disjoint snapshots. Fixed by deriving
the destination offset from the address as well, with source and destination offsets
separated. **53.125% non-zero across all 4,096 columns and ONE atlas, against 13.281%
across columns 0..1023 and FOUR atlases under `CZ_VK_NO_ADDR_TILE_FOLD=1`** — and
`13.281% x 4 = 53.125%` exactly. 17,355 folds against zero. No frame cost.

**THE WHITE PLATEAU IS NOT THE TONE CURVE AT `x = 1`, which is what parts 27, 28, 30 and
the first half of 31 all assumed.** Four whole-frame arms on the outdoor route, read off
the scene buffer with the new `tools/snap_plateau.py`:

| arm | px at exactly rgb(180,180,180) | px at grey 181/182/183 |
|---|---|---|
| null | 1,348 | 0 / 0 / 0 |
| `67.w=0` (the additive `tf1^2 * 4`) | 961 | 0 / 0 / 0 |
| `24.xyz=0` (the sun) | 721 | 0 / 0 / 0 |
| `1.xyz=0` (all multiplicative; **61.5% of the frame black**) | 893 | 0 / 0 / 0 |
| `14.w=0.25` (**mean luma 35.07 -> 18.30**) | 1,093, and **zero at 119** | 0 / 0 / 0 |

The counts are inadmissible across runs (gotcha 254) and the prediction they were meant
to test is unsupported rather than refuted. **The invariant is the finding**, because it
is a within-frame property: the peak never moves off 180 and nothing is ever above it —
through an arm that removes two thirds of the picture, and through an arm that quarters
the exposure that curve multiplies by. 119 is where the curve sends `x = 1` at quartered
exposure and **not one pixel of 921,600 landed there**. A value produced by that curve
cannot be invariant under scaling its exposure, so these pixels are not its output.
`180 = 255 * sqrt(0.5)` is now the coincidence to explain.

**What survives, re-measured on DAYLIGHT rather than the night captures part 30
withdrew:** the plateau is a hard pin — 1,348 px at exactly 180, zero at grey 181-183,
and only 45 px of 921,600 above 180 in all three channels, in a frame with mean luma 36.3
and 63,398 distinct colours.

**New instruments:** `CZ_VK_EXPOSURE_TRACE` (frame 3000 is 6,116 draws all within
0.214622..0.214647, so one exposure is in force per frame — assumed for four parts, now
measured), `CZ_VK_NO_ADDR_TILE_FOLD`, `tools/snap_plateau.py`, and `psbindLine` 2048 ->
8192 with an explicit truncation marker.

**Gates:** `--smoke` OK; `truncated=0`; A5 **exit 0, 3 permutation windows, 0 real**;
`shader_dim_census.py` exit 0; both PM4 capture oracles exit 0; no new Vulkan validation
messages. Gotchas 275-277.

## Part 30 (2026-08-11) — the white plateau's tone curve gets numbers, and two steps of
## the chain are retired

Part 27 built an eight-step chain to the white surfaces without ever reading a value in
the tone curve it was reasoning about. Part 30 read them, and the reading costs the chain
two of its steps while confirming a third from an independent direction. **The surfaces
are not fixed.** What changed is that the item is now pointed at a specific, enumerated
population instead of at an inference.

* **`tools/xtr_draw_constants.py`** — hardware's pixel-shader ALU constants for a named
  draw, with per-register provenance (`set` / `unset` / `UNRECOVERABLE`, gotcha 263).
  Part 27 asked `w1_spawn`, got `UNRECOVERABLE` for the three registers that decide the
  whole curve, and wrote down that a new capture was needed. **Five of the other six
  captures answer**, from data on disk for weeks — 68 draws, identical values (gotcha
  274, and the same shape as gotchas 3 and 264 from a third direction).
* **The curve.** With `x = colour * pc(14).w`, every one of the 48 emitting shaders ends
  in `out^2 = (max(0.25x + 0.75, 1.0) - saturate(1-x)^2) * 0.5`, which is 0 at `x=0`,
  exactly **180** at `x=1`, and 255 only at `x=5`. Part 27's knee arithmetic
  (`sqrt(K1*K2) = sqrt(0.5)`) is confirmed against the constants themselves.
* **A prediction, pre-registered and refuted.** "Our `A` term reads 0" was the only
  assignment reproducing all five of part 27's observations at once. It reads 0.25 —
  hardware's — on all 55 distinct bindings of the biggest emitter, as do `B`, `K1`, `K2`,
  the fog distances and the fog colour. The run confirmed something better than the
  prediction was: the literal pool is **per shader** and ours carries each shader's own,
  `ps_ad65b98593f95926` reading the same four numbers one register lower than
  `ps_7d2f8f33deec1b65`.
* **The emitter is exonerated on the ground shader specifically.** Part 29's named next
  step — read our translation against the capture's own disassembly — done, and they are
  instruction for instruction identical through the whole program, all six `_sat`
  modifiers included. The clamp is on an INPUT.
* **Two retractions, in place.** 180 is `sqrt(0.5)` from the shader's trailing `sqrt`,
  not "a literal 0.5 in a `k_8_8_8_8_GAMMA` surface" — there is no gamma encode to look
  for. And "these surfaces are not shaded at all" is not supported by the plateau: the
  curve's derivative vanishes at `x=1`, so a 10% spread in the colour quantises to ONE
  8-bit value (gotcha 273). A third over-claim was withdrawn before anyone could quote
  it — "nothing in five of seven frames exceeds 180" is arithmetic, not evidence, because
  those five are the operator's night captures.
* **What is owed, enumerated.** The ground shader reads **32 pixel constants and nine
  have been compared.** `c28..c39` is a twelve-register block shaped like a light array,
  `c40/c41/c42` are the `dp4` rows of a shadow projection, `c23`/`c27`/`c67` multiply the
  term the fog LERP consumes. Since the vertex data and all three DXT1 textures already
  match hardware bit for bit, and DXT1 cannot carry a value above 1 on either side,
  hardware's extra range arrives through a constant.
* **`CZ_VK_PS_CONST_SCALE`** — the arm that separates "the colour varies and the curve is
  flat here" from "the colour is pinned", by moving the surfaces to a part of the curve
  whose derivative does not vanish.

**And two things that were not the white surfaces.**

* **A harness defect: since phase A/V, every headless run WITH SOUND ignored `timeout`.**
  `SDL_HINT_NO_SIGNAL_HANDLERS` sat below the `CZ_NO_WINDOW` early return, and the audio
  device is a second, independent SDL entry point. Exit 124 at 20 s with
  `CZ_NO_AUDIO_OUT=1`, still alive at 180 s without it; fixed and verified with the same
  two-arm test. The symptom is a longer SUCCESSFUL run, which nothing reports (gotcha
  272), and it was found only because an A/B block visibly failed to advance.
* **The XMA decoder costs no frame time**, closing part 29's item 0b. Three runs an arm,
  alternated, decoder shown to engage on the route first, null measured within the
  control arm: every bin medians 32.0 ms in both arms, largest mean difference 0.2%
  against a 0.6% null, `>33 ms` share 1.05-1.12% with the decoder and 0.99-1.17% without.
  Quoted with its bound — the workload is pinned at the two-vblank floor in both arms, so
  it says the decoder does not push frames off the cap, not that it is free.

## Part 29, second half (2026-08-11) — FIXED, by one field in a packet header

The diagnosis in the first half was right and its conclusion about the ROOT was wrong.
The operator refuted it in one sentence — "the clip is around 5 min 10 s" — and reading
the asset that sentence forced open answered everything at once.

**The asset.** `audio/cinematics.big` entry `39694.xma`, 24,377,344 bytes. Its 11,903
packet headers sum to 89,007 frames of 512 samples = **316.5 s as three interleaved
streams**, matching the operator's stopwatch. Three streams because the 360 decodes 5.1
as several 2-channel streams sharing one packet stream, one XMA context per pair —
**which is why the prologue's contexts 5, 6 and 7 all pointed at input buffer
`02584000`.** This project had noticed that twice and written it down twice as "worth a
look, not worth a conclusion". It was the bug's fingerprint.

**The defect.** The XMA2 packet header carries `packet_skip` — how far to step to reach
the next packet OF THE SAME STREAM. Our walk advanced by one. Byte-correct for mono and
stereo, which is every other asset this title plays and where `skip` is always 0, so it
survived 29 parts. Wrong for 5.1, where each context then decodes the other streams'
packets as its own. One 128 KB buffer is 64 packets carrying 519 frames = **1.845 s of
programme**; the skip chain from packet 0 reaches **20 of those 64**; we produced
**4.916 s** per context. 2.66x too much audio, so each output ring filled about three
times faster than the title's mixer drained it, the whole 5.1 voice group wedged after
one buffer, `SamplesPlayed` stopped, and the PID clock tracked a frozen input.

**The result.** Cinematic-era `runs/distinct` **120 -> 1.00 in every quarter**; the audio
clock **4.906667 s frozen -> 310.7 s of a 316.5 s track**; `audio/cinematics.big` read
**2 -> 201** times. An operator session then played **four** cinematics to completion with
sound — the prologue, one more, the safehouse exit and the combo-weapon award, which
awards the weapon — with `CZ_CINE_TIME` recording exactly three playback segments running
0.00 -> 310.68, 0.00 -> 60.26 and 0.00 -> 17.53 s across 21,172 frames that never looped.
`open-items.md` 1 closes with it: no cinematic is known to fail.

**A second defect, fixed separately and recorded as having changed nothing visible.** The
walk retired a spent input buffer by unconditionally switching to buffer 1, which this
title never uses (136 context dumps, `in1Ptr` 0 in every one; it re-arms buffer 0 in place
and swaps only the pointer). That parked a context on a buffer that does not exist,
unrecoverably. `ctx7` was caught in it. Necessary and correct; moved the gate by zero.

**Knock-on:** the shader cache grew **411 -> 417**. A cinematic that plays binds six pixel
shaders no run had ever reached, and the session logged `no translated shader` six times.
Gotcha 13 on this project's own claim — "the shader cache is complete" expired because an
unrelated audio defect got fixed.

**Two ledger entries, both of which cost real time.** 270: two components you built
agreeing is a consistency check and never an oracle — our decoder's output matched the
guest's reported position to 448 sample-frames out of 235,968, and that agreement was read
as "the clip ended". 271: a format field that is zero in every asset you have played is
untested, not absent; and an unexplained structural oddity in your subject is the bug,
waiting.

## Part 29 (2026-08-11) — the cinematic loop is a CONTROL loop, and the defect moved
## one level up into the audio pipeline

The part-28 hand-off left one instruction for the ping-pong and it was the right one: a
palindrome means some clock decrements, so find what writes the cinematic's time. Doing
exactly that took a day of candidate-chasing off the board.

**The lead was invisible to a tool that answered "0".** Asked who touched the cinematic
manager's singleton, `gdis --find-uses` printed **0 site(s)** — for a global read on the
frame path. It reconstructed `lis`+`addi` pairs only, i.e. it could see code that takes
an address and not code that dereferences one, which is what the compiler emits for a
global it is about to load. It is 301 sites. Gotcha 25 in its purest form, and the
control cost one command.

**What writes the time.** `sub_82475718`, whose return `sub_82478FC8` stores straight
into `[cine+0x1698]`. It switches on a mode word the image ships as **2**: `0` raw scene
time, `1` the audio stream position, `2` `PID(audio position)`. `sub_824741D8` is that
PID and did not have to be guessed at — its own tail plots four values through the
engine's debug-graph API under `Cine.Audio P-gain / I-gain / D-gain / MV (ms)`, the
module naming its own control law. It returns **setpoint minus an accumulator**, a value
with no monotonicity anywhere in it, which is the whole defect class.

**The measurement found something better than "the controller oscillates."**
`CZ_CINE_TIME` on the prologue: mode 2 on all 2,212 lines and the PID ran on 2,208;
`setpoint` climbs linearly to 122.7 s; **`audioPos` freezes at 4.906667 s after four
seconds and never moves again**; `ret` hunts 4.91 <-> 5.27 s. The controller is tracking
an input that has stopped and integrating against an error it cannot close. **The PID is
the mechanism of the symptom and not the defect** — which is the difference between
tuning a gain and fixing an audio pipeline. The probe's columns were chosen so it could
have come out otherwise: `mode` never reading 2 would have killed the explanation, and an
oscillating `setpoint` would have moved the defect to the caller.

**The camera palindrome IS that clock, joined rather than asserted.** Interpolating `ret`
onto every frame of the same run, the median spread of `ret` within one
`cameraFingerprint` is **0.0052 s**; the same statistic at deliberately wrong alignments
reads 0.042-0.377 s. The null comes from the same data, so nothing is assumed.

**The arm, and every setting is a path the title implements.** `CZ_CINE_AUDIO_MODE`:
mode 2 **LOOPING** (15 poses, runs/distinct 120), mode 1 **FROZEN** at the stuck position
for 338 s, mode 0 no loop and the run reaches gameplay. Mode 1 was predicted in the commit
before it was run. **Mode 0 is confounded and is not a fix** — with no sync the first call
site hands over an uninitialised ~138,181 s scene time and the cinematic ends immediately.

**Where the defect is now.** Five named functions down: `SamplesPlayed / sampleRate`,
from a struct shaped exactly like `XAUDIO2_VOICE_STATE`. `4.906667 x 48000 = 235,520`
samples exactly `= 1,840 XMA subframes of 128`. A diagnostic run says the clip **ENDED**
rather than starved — the three dialogue contexts decode 5.03/5.04/**4.92** s and stop
while our decoder stays healthy — so the stream ran out and nothing told the title, and
the voice sits in the "playing" branch forever with the wall-clock fallback never firing.
It could only appear now: before phase A/V nothing decoded, so no voice ever reached the
end of a stream. The one thing NOT settled is whether 4.91 s is the clip's true length or
where our decode stops, and those have opposite fixes.

**Two corrections.** The recorded `runs/distinct = 6.13` for this defect is **diluted 6x**
— menus 1.01, cinematic era 38.27, steady state 15 poses at 120 — and
`tools/frame_loopiness.py` prints era quarters now; it caught this part's own author
reading a mid-run file and calling mode 0 healthy. And "the audio stopping" was one phrase
covering two facts with opposite orderings: the reported position stops first and is
upstream, audible output stops ~5.5 s later and is downstream.

**Gates:** `--smoke` OK. The repro reproduces exactly (6.14 / 0.58 / 1169 against the
recorded 6.13 / 0.58 / 1170) and reproduces again on the probe binary, so the instrument
does not perturb its subject. Nothing outside `runtime/cpu` and `tools/` changed.

## Phase A/V (2026-08-11) — the game makes sound, and the prologue cinematic was
## waiting for it

`docs/phase-av-notes.md` is the full record; `docs/phase-av-plan.md` is the plan it
executed, now annotated with what the plan got wrong.

**One fix closed two operator requests.** The plan's §2 said the two subsystems might turn
out to be one job, and led with the single run that separates a STALLED cinematic from an
INVISIBLE one. That run answered STALLED in seven minutes — `cameraFingerprint` constant
for **10,527 consecutive frames**, ~1,230 draws, 0.00% coverage, parked at
`701_chuck_arrives_in_town` exactly as §6ah left it on the part-16 binary. The cinematics
track the plan had prepared for afterwards was never needed.

**The audio defect was one address, and the decoder alone would not have found it.**
`runtime/audio/xma_decoder.cpp` is ffmpeg's `AV_CODEC_ID_XMA2`, lifted from the Fable 2
port; wiring it in produced nothing, and the two-sided instrument said why in one line —
`ctx0 first packet @02538000: 00 00 00 00 ...`, with an independent `Xma_Validate` at
rms 0.0000. The input was a page of zeros, and a page of zeros decodes to SILENCE rather
than to an error, which is why every layer above still looked healthy.

Adding the DESTINATION address to the `NtReadFile` trace closed it:

    NtReadFile('game:\data\audio\music.big', 131072 bytes @ 16666624)
         -> 131072 into A2538000
    [xma] ctx0 in0=02538000 64 pkts (131072 bytes): 0 non-zero (0.00%)

**Same page, 0xA0000000 apart.** The XMA decoder is a DMA device, so the title writes
PHYSICAL addresses into the hardware context; our flat 4 GB map puts the physical arena in
a window at 0xA0000000, so the two are different offsets and nothing aliases them. The
guest was right throughout — 16,666,624 is exactly `PressStartPrologue.xma`'s offset in
`music.big` and 131,072 is exactly the 64 packets the context declares — and
`MmGetPhysicalAddress_x` had implemented the inverse mapping since phase 1. Gotcha 267,
and it is a Case West item on day one.

**The result, with its off-state measured on the same binary.** `maxpeak` 0.000000 ->
**0.108854**, non-silent frames 0 -> **15,991 of 18,433**, an SDL device on pipewire
holding a steady 27 ms queue. `CZ_NO_XMA_DECODE=1` and `CZ_NO_AUDIO_OUT=1` are the two
control arms and they are independent.

**Then the cinematic.** Same recipe, same binary, one variable:

| prologue, 400 s | decoder OFF | decoder ON |
|---|---|---|
| longest run on one camera | **10,513 of 12,427** | **159 of 12,429** |
| distinct camera runs | 1,014 | 7,175 |
| coverage > 0 | 1,864 (**15.00%**) | 12,422 (**99.94%**) |
| deepest file | #155 | #155 |

Three runs, not two: `CZ_FAKE_PRESS_SEQ` is a fixed-interval arm against a boot whose
depth in wall time is a distribution (gotcha 75), so the control was run on the pre-commit
binary and again on the current one with the decoder off. They agree to 0.1%, which is two
orders of magnitude tighter than the control-to-arm split.

**THE METHOD FINDING, AND IT IS THE ONE TO CARRY.** Part 16 had refuted this hypothesis
with an arm — `CZ_XMA_NULL_DECODER`, three configurations of one binary, voices always
playing / never playing / starting-then-finishing with 19 start and 18 stop edges, all
freezing frame-for-frame identically. §6ah recorded it as "refuted, not merely
unconfirmed". It was a better-built negative result than most and it retired a true
hypothesis, because the arm moves **the predicate the title polls** and can reach nothing
downstream of PCM actually existing. An arm refutes a hypothesis only over the states it
can reach. The tell was in the arm's own header comment: it "fabricates playback progress
the real hardware would only make after actually decoding the audio". Gotcha 268 —
and it widens gotcha 172, because your own stub is an oracle.

**Two smaller retractions.** The pump was never Fable 2's broken `sleep_for` kind; it has
always been `sleep_until` on an accumulating deadline and measures **187.4-187.6
callbacks/s** against the 187.5 that 48 kHz needs. What was missing was the counter, not
the fix, and both `open-items.md` 00e and the plan's step A0 said otherwise. And the
64-byte context layout was checked against the guest's own arithmetic — `dw[8] - dw[7] =
6400 = 25 blocks x 256` — rather than taken from a recollection of Xenia's struct.

**Three instrument notes.** A plausibility check that was VACUOUS and the compiler caught
it (`p < PPC_MEMORY_SIZE` for a `uint32_t` against a 4 GB map is always true — gotcha 30
as a build warning). A packet budget per tick that is an EVIDENCE guard rather than a
performance one: without it a decoder returning nothing walks a whole 64-packet buffer in
one 1 ms tick and retires it, which reads downstream as "the voice finished" and destroys
the state being measured. And a 5-second report whose filter had to test `decodeCalls` as
well as frames, or it would have hidden exactly the context it exists to find (gotcha 264).

## Phase A/V, second half (2026-08-11) — the operator plays it, and the cinematic
## turns out to ping-pong

The audio half above landed and the operator was handed the build. Everything below came
out of that session, and two of the four findings are corrections to claims made earlier
the same night.

**"No sound" was not ours.** Pipewire held the `cz_runtime` sink input at `Mute: yes`, at
100% volume on the correct sink, because wireplumber remembers per-application state and
this application had never produced audio before. `pactl list sink-inputs` is the check,
and it is now in the hand-off — an hour of debugging our output path was available here
and was not spent.

**THE PROLOGUE CINEMATIC PING-PONGS.** Operator report: it plays, and the instant Rebecca
Chang speaks the scene runs forward ~1 s, backward ~1 s, and repeats, with the subtitle
re-appearing on every forward pass. **It was already in data captured hours earlier and
had been read past.** The arm run has 7,175 camera runs over only **1,170 distinct**
fingerprints, many recurring exactly 463 times, and the sequence is a clean palindrome of
~26 runs. `docs/open-items.md` 00j.

**The correction that matters more than the finding**: the A/V result had quoted "longest
frozen camera run 10,513 -> 159" as the cinematic being fixed. That number is real, and
`1,170 distinct over 7,175 runs` was in the same file and says the scene is LOOPING. The
statistic that answered the question being asked got read; the one sitting next to it did
not. **"Not frozen" and "playing" are different claims and only one had been measured.**
That is now a gate anyone can run — `tools/frame_loopiness.py`, which needs BOTH
`runs/distinct` and `runs/frames` because a frozen camera also scores ~1 on the first:

    gameplay, healthy    1.09 / 0.65      prologue ping-pong  6.13 / 0.58
    prologue frozen      1.01 / 0.08

**Three explanations built and eliminated, each with its own measurement.**

1. *Our output ring.* `XmaFillOutput` treated `write == read` as EMPTY at the top and FULL
   at the bottom — one state, two opposite meanings, twenty lines apart. Real, and
   **refuted**: repaired properly, `distinct` did not move by one (1170 -> 1170). Reverted,
   because keeping a behaviour change motivated by a refuted hypothesis is not a trade
   worth making — and because the repair's own "is the ring full" test turned out to be
   VACUOUS, being exactly the fill loop's exit condition. That is the second tautological
   predicate of the day; the first was a `uint32_t < 4 GB` bounds check the compiler
   caught.
2. *The audio stopping.* The operator noticed sound dies when the loop starts, which it
   does — permanently, 0 non-silent frames across the remaining 62,000. **Ordered by one
   grep on an existing log**: the stall precedes the silence by ~5.5 s, i.e. the scene
   stops advancing, never fires its next audio event, and the queued dialogue plays out.
   Downstream. Do not chase it.
3. *The "end sync point".* `CZ_GUEST_DIAG` made the engine name its own condition —
   `WAITING: end sync point not received yet!` at `0x824A10D4`, on the failing side of a
   branch whose success side is the cinematic's `Update(delta)`. Compelling, and the line
   fires **once** in 7,778 looping frames, which was flagged as a caveat rather than
   buried. `CZ_CINE_PROBE=1` then counted it: **ten entries in a 400 s run**, an even
   received/not-received split, and the containing function stops being called entirely
   while the loop continues. So the sync point is at most an event near the START of the
   loop, not the condition sustaining it.

**What survives is unevidenced and is labelled as such.** The title runs a PID controller
on cinematic audio latency (`Cine.Audio P-gain`/`I-gain`/`D-gain`/`MV`/`Cor Latency`, in
`cinematicmanager.cpp`'s region). It was named as "the mechanism" mid-session on the
strength of a string table and the shape of an oscillation; that was a stronger claim than
the evidence carried and it is demoted in place. Nothing shows the controller is running.
**The next move is to stop guessing at consumers: a palindrome means some clock
DECREMENTS, so find what writes the cinematic time each frame.**

**Two instruments, and one lesson about instruments.** `tools/xma_live_sample.py` reads a
running process's XMA contexts via `process_vm_readv` — no ptrace stop, so an operator
sitting in a live defect is never interrupted; it is what showed the mono dialogue voices
toggling `output_buffer_valid` ~94 times in 8 s while the stereo music voice never did.
And `CZ_CINE_PROBE` **reported from inside the function it counted**, so it went silent
exactly when that function stopped — which was the answer. It stayed readable only because
frames were visibly advancing elsewhere. Gotcha 269: an instrument must not share a
liveness dependency with its subject.

## Phase C part 28 (2026-08-10) — the engine's whole diagnostic layer, from one byte;
## and an LOD report with no defect yet attached

Two operator requests, in the order they arrived.

**1. A plan for audio and cinematics — `docs/phase-av-plan.md`.** Planned as ONE part
because the assets say the two subsystems are coupled: a cinematic script carries
`cCineAudioEvent audio_stream` with `AudioEventName = "sync:39791"`, and our audio path
completes nothing, so a scene gated on an audio event that never finishes is a black
screen. The plan leads with the test that separates the two readings, because a STALLED
cinematic and an INVISIBLE one are the same picture and completely different counters
(draws/frame, `cameraFingerprint`, whether the era ends after the script's `Duration`) —
one run, no new code. Audio order is fixed by the measurement already in hand: the guest
hands us buffers full of zeros, so XMA decode before any output device, and before that
the pump's fixed `sleep_for(5333us)` needs a deadline (Fable 2 measured that shape at
~184 frames/s against the 187.5 that 48 kHz needs, and a 2% deficit stutters exactly like
a broken decoder).

**Finding 7 corrected while there.** The no-Bink half stands; the "in-house Movie Player
Object" half was misleading. `cinematics.big` is 29 `.txt` SCRIPTS and
`data/anim/cinematic/*.big` is animation data, so **a cinematic is an in-engine scripted
scene played through the ordinary renderer and there is no codec to write**. Corrected in
the ledger and in CLAUDE.md, because Case West is the same engine and would have read that
sentence and gone hunting for a decoder that does not exist.

**2. "LOD seems really broken — I have to be really close for the near LOD."**

**First, what LOD IS in this engine, because the word misleads.** All 54 `lod` strings in
the image — the complete set — are about STREAMING, not a distance curve: a
`cLODController` array parsed per zone beside the occluders and model instances,
`ForceLODTexForStreamingWorld`, `WAITING: cLevel - wait_for_tex_lod`, a `UseLOD` bool in
the PROP schema (it sits between `Durability` and `Unmoveable`), and a per-zone choice
between `COMMON_TEXTURE.tex` and `COMMON_TEXTURE_LOD.tex`. **There is no LOD-distance
scalar named anywhere in the executable**, and 989 `*_LOD.tex` entries ship across the
archives. So this is a question about the streaming system's promotion rate and must not
be chased as a distance comparison.

**The instrument, which is the part worth keeping.** The useful evidence is the engine's
own, and `CZ_GUEST_LOG` had always printed nothing. Gotcha 215 explained that as "a debug
byte per category that a shipped build leaves at zero" — a hunt across hundreds of flags.
That framing survived thirteen parts unexamined and was wrong twice. A scan of `.text` for
`lis`-resolved byte references finds **one** address, `0x829EC974`, read by **2,013 sites
and written by none**, every site the identical `lbz / cmplwi 0 / bne skip / bl
sub_827877C8` — and the image ships it as **1**. The polarity is inverted from the guess:
the layer was switched OFF, and clearing one byte re-enables all 2,013.

`CZ_GUEST_DIAG=1` clears it and sets `0x82AC3EAD` (592 readers, 2 writers) so the assert
sites it un-silences PRINT their file and line instead of reaching their `twui` trap —
suppressing the trap is what makes the message reachable, and the assert still prints.
Pumped rather than poked once; it reads exactly 2 writes, which is the scan's prediction.
**Null and positive control are the same route twice:** `CZ_GUEST_LOG=1` alone gives 0
`[guest]` lines of 11,168; adding this gives 1,239.

**What the engine then said, and none of it is a smoking gun.** The per-zone LOD decision
runs and is MIXED — of 7 zones loaded, 3 took `COMMON_TEXTURE.tex` and 4 took
`COMMON_TEXTURE_LOD.tex`, which is what working looks like. **No streaming failure of any
kind fired**: no `Queue is full in MoveLoadRequest()`, no `Out of memory in the load &
decomp heap!`, neither `cZone::UpdatePriorities()` assert. Heap headroom shrinks smoothly
(zombie VB heap 5,742,568 -> 2,221,880 largest-free in ~330 KB steps, no floor hit). The
title asks for 447 MB up front and gets it. The per-level streaming VB heap is a fixed
24-entry table at `0x82042EB8`, 25-65 MB by level id — guest data, identical on hardware.

**So the item is OPEN with no mechanism, and that is recorded as such** (`open-items.md`
00i). The one candidate that would be OURS and is untested: `KeSetBasePriorityThread` is a
no-op and `KeQueryBasePriorityThread` returns NORMAL for every thread
(`runtime/kernel/imports.cpp:1463`), while the title creates ~47 threads including a
`DecompressThreadLoop` that on the 360 carries an explicit priority and hardware-thread
affinity. A decompression thread scheduled level with the render thread promotes late,
which is exactly "it loads, but only when I am close." **Before building that, ask the
oracle** — DR2-family titles are aggressively streamed on real hardware, so "faithful" is
a live answer and nothing measured here distinguishes it from slow streaming.

**Method notes.** Gotcha 266 is the transferable half and it is cheap: for any global
suspected of gating diagnostics, count its READERS and its WRITERS separately — thousands
of readers and zero writers is a build-time constant whose value states the polarity, and
reasoning about which flags a release build "would have" left unset is guessing at a fact
the binary says outright. Gotcha 215 is corrected in place rather than deleted. And the
first version of the arm's own counter printed on `reasserts == 2`, which is true on every
subsequent pump too and put one line in the log tens of thousands of times — a counter's
log line has to fire on the EDGE.

**Gates:** `--smoke` OK. Runtime change is one diagnostic arm in `debug_tunables.cpp`,
inert unless `CZ_GUEST_DIAG` is set; nothing on the render or PM4 path.

## Phase C part 27, second half (2026-08-10) — the white patches get a value, a
## population and an instruction

Everything above was written before the operator drove the game. What followed took the
white-patch item from seven refuted hypotheses to a chain of eight measurements.

**The two instruments that made it possible were both asked for by the operator**:
`CZ_CAPTURE_KEY` (one F9 -> picture + census + all 67 resolve snapshots of one frame) and
`CZ_DEBUG_FLAGS` (the title's own debug bools by menu label, held on by the pump). The
second mattered more than it sounds: `DISABLE TIME OF DAY` put the world at night, and a
surface that does not vary with the light stops hiding in a lit scene. The daytime
captures had contained the same plateau all along and it read as "a bright bit of ground".

**The chain, each step with its own control:** the patches are exactly rgb(180,180,180) in
the scene buffer at all seven locations; they are not modulated by lighting (90% of the
slot-machine frame below luma 40, the cabinets at 255); the tone map is the amplifier and
not the cause (96.1% of the presented white was already >=150, max exactly 180); the
MATERIAL SHADERS write it (`XE_VALUE_PAINT`, 3,242 -> 0, positive control 99.73%); 48
shaders emit it (`XE_SHADER_TAG` over the operator's 59 captures, `ps_7d2f8f33deec1b65`
alone 47%); they share ONE epilogue; 180 is that epilogue's KNEE and is the same for any
exposure; and no max takes its floor there, so the colour arriving is pinned at
`1/pc(14).w`.

**Four hypotheses died, each by measurement:** the cube dummy (the fix engaged and the
surfaces stayed white), a NaN (positive control 99.85%, zero painted), XenosRecomp's `rcp`
clamp (`FLT_MIN` is -FLT_MAX, and +3.4e38 gives 255 not 180), and an emitter defect in the
epilogue (the HLSL is one-to-one with the microcode — **nothing for Fable 2 to inherit**).

**Three of the day's false results were the same error**: comparing two populations built
by different membership rules (gotcha 264). The 414-of-414 cube census counted only slots
already reading cube; the binding diff compared our declared slots against hardware's
whole fetch file; the floor paint was first read as if a 48-shader measurement said
something about one shader.

**And the day ended on a contradiction between two of its own instruments, resolved
structurally rather than by preference.** The translated shaders are a `switch` over exec
blocks inside a loop, so a block can run more than once per pixel: `f = f || ...`
accumulates and is safe, `x = <value>` records whichever iteration was last and is not.
The hand probe was the unreliable one.

**Also this part:** `CZ_DXC_DEFINES` + `CZ_SHADER_SPV` make a shader change a same-binary
A/B; the `.xtr` tools learned `LOAD_ALU_CONSTANT` (620 packets to `SET_CONSTANT`'s 36, so
they had been reporting hardware's constants as zeros) and learned to say UNRECOVERABLE
where the capture cannot answer; a cube fetch over a single 2D surface now replicates that
face instead of serving white; and `ps_7d6044e7dcaea1f2` was found missing from the shader
cache — microcode we had held for sessions, invisible because 410 dumps and 410 modules
were different sets.

## Phase C part 27 (2026-08-10) — the ground draw's last input, and a replay that could
## not see the guest's constants

Two open items were worked and both produced an answer; one of them also produced a
defect in this project's own oracle tooling that had been quietly limiting every
capture comparison.

**ITEM 0 — THE VERTEX DATA FOR THE GROUND DRAW. CLOSED, AND IT MATCHES.** `tools/
xtr_draw_vertices.py` decodes hardware's vertex streams out of a single-frame trace in
the same shape `CZ_VK_DRAW_PROBE` prints ours, sharing the vertex fetch decode, the
endian unswap and the per-format component count with `xenos.h` and `vk_renderer.cpp` on
purpose. For the 25,234-vertex ground draw (`vs_36eef2c94b4a065c` /
`ps_ad65b98593f95926`) all five attributes agree to the printed digit over the first six
vertices — position `-23.79100, 3.10100, 25.04500`, texcoord `0.11789, 0.49056`, and the
three packed ones bit for bit.

**And the anomaly that motivated the whole comparison is not an anomaly.** The two float2
attributes at different fetch slots and different dword offsets that decode identically —
`loc4` from slot 94 and `loc6` from slot 93+1 — do exactly the same thing on hardware. The
guest genuinely duplicates that texture coordinate into two streams. The lead closes by
showing the mechanism is real and shared, which is a better close than failing to find it.

**The shader CONSTANTS were compared too, which part 26's input list had also missed**,
and they match wherever the capture can answer: `pc(1)`, `pc(22)`, `pc(45)`, `pc(46)`
identical, `pc(14)` a world position that differs with the camera as it must. `pc(0).w`
is 1.0 on hardware and 0.0697 for us, but the ground pixel shader never reads `c0`.

**So every comparable input to the white ground now matches hardware** — shader pair,
vertex count, bindings, texture contents, render state, vertex data, recoverable
constants — and **the defect is in the SHADING.** The next step is reading our translated
`ps_ad65b98593f95926` against the capture's own disassembly of it, which the round-2
shader dump contains.

**AND ITEM 00f's LAST LEAD IS REFUTED: the ground mesh drawn twice is TILING.** The probe
prints the scissor and the two draws read `0,0 640x720` and `640,0 640x720` — this
title's left and right halves, which `CLAUDE.md` already records from the other end.
There is no second pass to combine (gotcha 265).

**ITEM 0b — THE CUBE DISAGREEMENT. THE CONCLUSION SURVIVES; THE MEASUREMENT BEHIND IT DID
NOT, AND THE MAGNITUDE WAS OFF BY 10x.**

* **Part 26's "414 of 414, no disagreements" could not have found one.** It selected draws
  where a cube-declaring shader was bound and then counted the constants that ALREADY read
  cube; a disagreeing slot reads 2D and was outside the population by construction.
  `tools/xtr_cube_agreement.py` asks it per declared fetch slot, out of the same sidecar
  arrays `bindTextures` binds from: **0 of 13,203 fetches disagree.** Same answer, now
  falsifiable (gotcha 264).
* **The disagreement is ours, and now positively identified.** `CZ_VK_DIM_DISAGREE`
  enumerates the whole population of a 400 s outdoor run: **9 distinct (shader, slot,
  texture) cases over two textures.** The slot-4 ones are `vs_2f13eecec64e508e` with
  `ps_3da9454d30a3a225`, `ps_ad2d8362c47d9e45` and `ps_71d569a46634d72a` — **and those
  exact pairs appear in the captures binding a real 128x128 DXT1 cube map at slot 4**
  (`0E751000`, dimension 3, stack depth 6). Our 32-slot dump at those draws shows slot 4
  holding an **exact duplicate of slot 3**. It is not the decode: the same dump reads
  `s6 06805000` as dim 3 depth 6 correctly.
* **The share is 0.05%, and the decline had a second, larger cause nobody had named.** On
  the outdoor route `cube fetch got the dummy` is **3,210 of 1,903,592 cube fetches**, and
  it now splits exactly: **2,182 because the constant at that slot is not a texture at all**
  (the guest never set it) and **1,028 for the dimension disagreement**. Nothing
  unattributed. Part 26's 14,670 was a pre-cube-snapshot binary and should not be
  re-quoted.

**AND THE TOOLING DEFECT, which is the part that transfers.** All three `.xtr` tools
decoded `SET_CONSTANT` and `SET_CONSTANT2` and silently dropped **`LOAD_ALU_CONSTANT`
(0x2F), which is how this title sets nearly all its shader constants — 620 packets against
36 `SET_CONSTANT`s in `w1_spawn.xtr`.** The runtime's own `pm4.cpp` has handled it since
phase 4; the oracle tools were written later from the same mental model rather than from
the same code (gotcha 262). With it handled, **81 of those 620 loads read memory the trace
does not carry**, so some registers are genuinely unrecoverable and the tool now says
`UNRECOVERABLE` instead of printing the stale value (gotcha 263). **What exposed the whole
class was an impossible value, not a suspicious one**: the ground pixel shader uses
`c255.w` as its literal 1.0 and the replay said `c255` was `(0,0,0,0)`.

`pc(253..255)` are therefore the only inputs to the ground draw still unknown, and
`ps_ad65b98593f95926` does read all three — a capture carrying the constant-buffer memory
would close the input list completely.

**Gates:** `--smoke` OK; `tools/shader_dim_census.py` exit 0 over all 410 shaders with the
ucode parse and the SPIR-V agreeing on every one; `no translated shader` = 0.
Not re-run and owed: the A5 kernel-call diff, `truncated=0`, the PM4 capture oracles and
the capture-E picture correlation — no renderer behaviour changed this part (both runtime
edits are counters and a diagnostic), but they are owed before any claim that rests on
them.

## Phase C part 26 (2026-08-10) — the rendered cube map, and a filter that was never
## going to report anything

Four things, and the two that will matter longest are both about measurement.

**The cube snapshot path — open item 00's remaining half.** `06805000` is an environment
map the title renders itself, so its address is a resolve destination and guest memory
there is zeros; part 25 declined it to the 1x1 white dummy. It is now assembled from the
six resolve snapshots at `base + i * 0x4000` into a six-layer `VK_IMAGE_VIEW_TYPE_CUBE`
image in set 2, refreshed by each face's own resolve in that resolve's own command buffer.
The refresh is load-bearing — **8,850 face refreshes in 240 s**, because the title
re-renders the map continuously — and the face layout was PRINTED face by face so the
stride model could refute itself; six of six filled. **358,767 of 999,508 cube fetches
(35.9%)** now read it. `CZ_VK_NO_CUBE_SNAPSHOT=1` is the arm.

**The outdoor admissibility filter is unsatisfiable, and the route was never why.** Part 25
built the DebugJump route to reach an admissible outdoor frame and handed part 26 the job
of checking it: two runs of ONE configuration, count the frames sharing both fingerprints.
**422 of 13,056, none above 141 draws, and 0 of the 12,174 outdoor frames** — the same
answer as the old stick recipe, on a route that demonstrably goes where it should (93% of
its frames are outdoors, and two runs' draw counts agree to a median 1.4%). A crowd of
animated actors does not render the same draw list twice, so exact equality selects for
stasis (gotcha 254). `tools/frame_determinism.py` is the check; `tools/frame_era_medians.py`
is the replacement protocol.

**The cube A/B, read with three baselines — and the third one changed the answer.** Six
420 s runs, one block, arms alternated. Removing EVERY cube map moves the era median luma
from a 56.59-56.91 band to **59.47, eight times the band with no overlap**: the outdoor
instrument is sensitive, and cube maps as a class measurably darken this scene. Removing
only the rendered map does NOT separate — its two runs straddle the band — so its
contribution is under ~0.5% of the frame's median, which is a bound and not a null. That is
consistent with its 35.9% fetch share, because a fetch count is not a screen area
(gotcha 257).
**On two baselines that arm had read 12.0x the null on median distinct colours and would
have been published.** The third baseline landed 5.4% away on that statistic, whose two-run
null had read 0.12%, and the result vanished — mean luma reproduces across three runs
(0.55%) and distinct-colour count does not (gotcha 258). The earlier numbers are retracted
in place in `docs/measurement.md`.

**Three of the validation layer's five defects, closed, and the layer now names our
objects.** `03320` (20 messages) and `01021` (4) were both found by reading — a barrier on
a depth/stencil format must name both aspects, and an image's TYPE must come from its view
type — and both went to zero. `vkCmdDraw-None-09600` (14) needed a run and, first, NAMES:
`VK_EXT_debug_utils` now comes in with the layer, and the next run said `[resolve snapshot
14A7A000 96x45 slot 32]` with the other thirteen forming a halving chain, i.e. one bloom
pyramid. The defect was the publish order — the descriptor was written before the
fill-and-transition recorded into the frame's command buffer, so for that window a
descriptor claimed `SHADER_READ_ONLY` on an `UNDEFINED` image. The snapshot VIEW path in
the same file already had it right, which is why views never appeared in the messages
(gotchas 255, 256).

**And the audio trace was rewritten before its answer was trusted.** It sampled one frame
in 512 and printed a peak that read 0.0000 both for a silent frame and for a null pointer.
It now scans every frame, counts nulls separately, and self-tests the scanner on a
synthetic frame of big-endian 0.5f. Boot to gameplay: `null=0 non-silent=0
maxpeak=0.000000`, self-test 0.5000. **The guest hands us real buffers full of zeros**, so
open item 00e's direction stands on a measurement: the next step is XMA decode, not an
output device.

**Gates:** `--smoke` OK; `shader_dim_census.py` exit 0 over all 409 shaders;
`no translated shader` = 0 on both 420 s runs; Vulkan validation **26 messages / 2 VUIDs**,
down from 64 / 5, both pipeline-creation rather than per-draw. Not re-run and owed: the A5
kernel-call diff, `truncated=0`, the PM4 capture oracles, the capture-E correlation.

### Part 26, second half — the operator drove it, and the captures answered

The first half of this entry is the cube snapshot path and the measurement work. The rest
of the session was an operator at the keyboard, and it changed what the part is about.

**A class of picture defects was reported and is OURS**: flat-white ground patches, white
props, blown-out glass and windows. The operator has played the title on Xenia and none of
them are wrong there — the check nobody had run, and it should have been first.

**Seven hypotheses refuted, each by a measurement**, three of them mine and each costing a
run: the tone map (the white is in the scene buffer, max 180 not 255), a missing texture
(three real DXT1s at real slots), constant UVs (the probe shows them varying), the white
dummy (all four heaps poisoned magenta — the ground stayed white), the clear colour
(`RB_COLOR_CLEAR` is black), the EDRAM format (`rtFmt=0` on 600 of 600 passes), and a
flat-decoding texture (a new uniform-upload counter finds nine, none of them the ground's).

**Two instruments were built for the operator and both had defects found by USING them**:
`CZ_VK_DRAW_CENSUS` truncated its lines silently at 512 bytes (105-215 lines of every
census, and "zero dummy binds" was about to be published off it) and overwrote itself on
every press (one captured frame was lost). F9 itself came from the operator, mid-run,
standing on a defect waiting for a frame counter to reach a number chosen in advance.

**Then round-2 captures**: seven self-contained single-frame traces, one per surface. They
established that our shader coverage is complete (357/357), that the white ground matches
hardware on shader, textures, texture CONTENTS and render state — leaving the vertex data
as the only uncompared input — and that our cube declines fire on a disagreement hardware
never exhibits (414 of 414 cube-declared draws agree perfectly). Details in open items 00f
and 00g; the reusable half is gotchas 254-261.

## Phase C part 25 (2026-08-10) — cube maps bound, and the effect measured absent indoors

Open item 00, the top picture item since part 23: 92 of the cache's 397 shaders sample
`TextureCube[]` and every one of them read descriptor index 0 — the 1x1 dummy — on every
draw since phase 5, because `bindTextures` published every fetch's slot into the
`Texture2D` array whatever its dimension. All three parts of the specified fix are built.
The full record is `docs/phase5-notes.md` §6ay; the short version and what it cost:

**Two independent derivations of the dimension, so it is a gate and not a claim.** The
shader's fetch instruction carries it (word 2, bits 14..15) and `synth_shader_container.py`
now writes it per SLOT into the sidecar; DXC's `OpDecorate ... DescriptorSet` words in the
translated SPIR-V carry the same fact through a path containing no code of ours.
`tools/shader_dim_census.py` compares them and exits 1 on disagreement — 298 modules /
973 slots 2D, 92 modules / 92 slots cube, zero 1D and zero 3D, agreeing everywhere. Shown
capable of failing by moving the parse one bit.

**The guest's own dimension field was located by CENSUS, and recollection was wrong.**
`CZ_VK_DIM_CENSUS=1` partitions every fetch by the shader's independent answer and
accumulates each class's always-set / always-clear bits: dword5 bits **9..10** (memory said
7..8), cross-checked against dword2's top six bits reading 5 for every cube fetch — a
prediction stated before the run from Xenia's published layout. Gotcha 244 is the general
form and it is the most reusable thing here.

**Three latent instrument defects, each of which would have produced a confident wrong
answer:** `Barrier`'s hardcoded `layerCount = 1`, already live in `R->dummyCube` since
phase 5 so five of the dummy's faces were written and sampled in `UNDEFINED`; the dummy
upload writing four bytes for a six-layer copy; and `CZ_SHADER_DUMP` failing silently into
a directory that did not exist, which cost a ten-minute recovery run and reported "0 blobs"
— a fact that reads as "no new shaders" and is not.

**And then the measurement, which is the part worth reading.** The picture A/B came back
byte-identical on every admissible frame, and it took four more experiments to learn why:

All four configurations in one serial block, same recipe and binary, p90 of the per-frame
mean |RGB| (the median is 0.0000 for every non-control row):

| | frames differing | p90 |
|---|---|---|
| **null** — default vs default | 81/111 | **2.972** |
| real cubes vs white dummy | 81/111 | **3.101** |
| real cubes vs white dummy, 2nd pairing | 81/111 | **2.393** |
| **positive control** — magenta vs white dummy | 77/105 | **37.877 (12.7x the null)** |

plus: admissible pairs 13-44 of ~300, **all under 1,800 draws**; 746,355 draws asked for a
cube, **45% bind a real one and 55% get the dummy**, of which **409,911 are ONE map,
`06805000`**, which the title renders itself.

So the cube path is live — the control repaints up to 72% of a frame — and **the instrument
is emphatically not blind, separating that control from the null by 12.7x with no overlap.**
Against that sensitivity, binding real cube maps changes **nothing measurable** in the
safehouse and prologue. Two explanations survive and both put the effect outdoors: this
era's cube maps are near-white, or the surfaces sampling them are not on screen indoors.
The outdoor era is exactly what no admissible comparison can reach. An earlier framing in
this session — "the harness is blind to a change of this size" — was written before the
control was measured properly and is retracted in `phase5-notes.md` §6ay.

**Three gotchas, and they are one error in three disguises** — 246 (a count with no
denominator, published as "0.03%" and corrected in place), 248 (a positive control read
with a statistic that could not see its own effect: "how much of the frame is magenta"
read 0.24% where a per-pixel diff read 80 of 110), 249 (an effect quoted with no null: "82
of 109 frames differ" where the null also differs on 82 of 109). Plus 245 and 247. The
mechanical fix for all three is one habit: **measure the arm against itself first, in the
same block, and quote ratios.**

**An operator opened and closed the game mid-session**, which is exactly the contamination
this discipline exists for; the drift baseline that overlapped it was discarded and re-run
in one serial block rather than defended.

## Phase C part 24 (2026-08-09/10) — the title's own debug build, switched back on; and
## the HUD defect, which was ours

Two independent threads, one of them a retraction of my own table and one a closure of
open item 00c. Both are recorded here because the *reasons* are more reusable than the
results.

### The retail executable still contains Blue Castle's whole debug build

* `common\debugmenu\debugmenu.cpp` is still in the image's source-path strings,
  `cDebugMenu` is still a class, the menu's item tree is intact, `God Mode:ON` is
  referenced by live code, and **`debugjump.txt` SHIPS** — it is the largest entry in
  `data/frontend/mainmenu.big`, larger than `title.txt`. None of it was compiled out.
* 393 boolean tunables gate it. One loader, `sub_824A2470`, resolves each BY NAME and
  stores the answer as a byte; every consumer gates on a plain `lbz`/`cmplwi`/`beq`. The
  loader runs ONCE, three hops off the XEX entry point, and nothing rewrites the bytes —
  so a post-hook is permanent and has no per-frame component. `CZ_DEBUG_MENU=1`.
* **The OPERATOR built the usable part**, not me: F2 opens the shipped DebugJump screen
  through the frontend's captured transition manager; F4 opens a host-rendered menu over
  the genuine retained `cDebugMenu` (retail destroys the populated startup instance before
  gameplay and it cannot be rebuilt, so `sub_824A8FE0` preserves that one instance).
  **AutoChuck is the most useful entry** — an AI that completes objectives, so a test no
  longer needs a human driving. Plus a PP award and a level-50 cap, which also has to
  suppress the combo-card reward rows above level 5 or receiving one crashes.
* Negative results kept: the in-game "Quickie Menu v0.21" renderer never draws even with
  its dispatcher bypassed; zombie spawning through the retained labels produces no actor
  across `dbgrun20`-`26`; the 64-entry "NPC To Spawn" selector has no surviving consumer;
  there is no retained vehicle spawn command. Incomplete retail scaffolding, not features.

### My tunable table was off by one, on all 387 flags (gotcha 241)

The loader does not store a lookup's result next to that lookup's name — it stores it
AFTER the next name is already in `r4`. Pairing each `addi` with the following `stb` named
every flag after its neighbour. **It survived two checks that could only confirm it**: an
`lbz` consumer scan finds readers at BOTH candidate addresses because every byte in a
dense flag struct is a real tunable, and reading back bytes I had written myself was a
tautology. The fix is a dataflow simulation over the loader. The operator's independently
derived table was right where mine disagreed with it; only the 8 entries they had copied
from mine were wrong. This is also the whole explanation for the failed first day: the
preset never enabled the debug menu, it set `enable_dev_only_debug_tiwwchnt`,
`debug_on_controller_2_only` and `debug_show_loading_time`.

### Open item 00c closed — and the cause was part 22, our own change

* **The operator's A/B settled it**, after they clarified they HAD fired a weapon in the
  store-off arm (the single hole that had forced an earlier retraction): store off = clean
  all run; store on = HUD collapse and ammo flickering 26<->27.
* **The mechanism is the guard's SAMPLING, not the store.** `StreamGuard` was exact only
  to 512 bytes and hashed 8 blocks of 64 above that. A HUD is batched into one multi-KB
  vertex buffer where only the digit quads change; those quads fall outside the sampled
  windows; the guard reports "unchanged" and the store serves last frame's numbers. It is
  independent of `CZ_VK_FRAMES_IN_FLIGHT` (the ping-pong is off at 1) and invisible to the
  census (`GUARD MISSED: 0 of 0` — a zero DENOMINATOR, blind not negative). That
  combination is why it survived three sessions.
* **Fix: raise the exact bound to 16 KB** (`CZ_VK_STREAM_GUARD_BYTES=N`, no rebuild to
  retune; `CZ_VK_STREAM_GUARD_EXACT=1` is the unlimited diagnostic). Exact-everywhere is
  75x the hashing and +11.9 points of frame time, so it is not shippable.
* **Cost at the gas-station crowd: zero frame rate.** 6,778 draws/frame, 32.2 ms, 31.0 fps
  — still the two-vblank floor — with `guard read` 14.15 MB/frame, `record` 19.3%,
  `outside` 54.2%, while the store avoids 50-61 MB/frame of copying. HUD confirmed correct
  throughout. Gotcha 243 is that reading: when the platform pins frame time, a CPU COST is
  as invisible as a CPU saving.
* **Residual exposure, counted rather than assumed**: 604-624 streams/frame still exceed
  16 KB and are only sampled, printed on every profile window. Gotcha 242 — a threshold
  fitted to a census is fitted to the population the INSTRUMENT could reach; 512 came from
  a census whose recipe never fired a weapon or changed a HUD number.

### A dead end of mine, recorded so it is not rebuilt

I tried to make 00c self-servable by counting frames where the LIFE pips / PP bar / LV
circle are absent. It reproduces beautifully — 69.0% and 69.4% across two runs of one
config — and it measures the WRONG THING: `phase5-notes.md` §2152 already records that
partial HUD is location-dependent (the safehouse has not raised it). Its three-arm result
said the store was innocent and is retracted. A valid headless metric must watch a HUD
number CHANGE, not a widget's presence. `CZ_FAKE_PRESS_SEQ` also has no trigger in its
vocabulary while attack here is RT, which is the real reason no headless recipe has ever
fired a weapon — **but adding the button was considered and dropped**, because a recipe
would still have to ACQUIRE a gun and ammo along a long scripted path, so the trigger
alone buys nothing. Weapon tests stay with the operator.

### Also fixed

`XamInputGetState` was returning SUCCESS for every user index after the debug-bridge
wrapper forced `r3 = 0`, telling the title four pads exist where two do. The bridges make
guest calls and a guest call clobbers `r3`, so the value does need restoring — just the
real one.


## Phase C part 22 (2026-08-08) — the cross-frame stream store, and two ways a real win
## nearly measured as noise

**The item the last three parts pointed at, built.** open-items 0a. `docs/phase5-notes.md`
§6av is the full record.

* **The measurement was re-run first**, because the hand-off said its numbers were one
  afternoon's (gotchas 50/51/86). On the part-22 binary the shares reproduce exactly:
  93.5-94.1% hits within a frame, 61-66 MB copied a frame, 93.5-94.7% of it repeating last
  frame's key. The absolute MB does not reproduce (part 21 said 74-77) because level-2
  hashing slows the frame and the fixed-interval recipe drifts with it — the shares are
  the claim.
* **The census was extended to name WHICH streams the guest rewrites in place, and that
  is what decided the design.** Part 21 knew the count (164 of 10,154,820) and not the
  identity. All 30 are **exactly 80 bytes**, endian 2, declared vertex bindings, in two
  narrow guest ranges; no index buffer and no dependent fetch was ever rewritten. So a
  guard that hashes anything up to 512 bytes in full covers the whole observed population
  with a 6x margin, and the `mprotect` + `SIGSEGV` design the hand-off called for was
  never built. §6av records it anyway, including the hazard that would have bitten it:
  `kernel/vfs.cpp` reads file data into guest memory with `fread`, and `read(2)` into a
  `PROT_READ` page returns **EFAULT** rather than faulting, so a level reload into a
  protected page would have failed silently.
* **The store itself is a SECOND BUFFER, not a region of the arena.** The arena's
  exhaustion path is load-bearing — it is what turned a fixed 128 MB into six parts of
  view-dependent whole-frame black — and a moving floor under it buys a smaller diff at
  the price of the riskiest coupling available. The cost is `StreamLoc` and three call
  sites. Maintenance runs where the arena's growth runs, after the fence wait.
* **THE STALE COUNT IS THE FINDING.** ~20 streams a frame are served from an address the
  store already held whose contents had changed — two orders of magnitude more than the
  30-per-run the census reports. Not a disagreement: **the census compares against LAST
  FRAME and the store against the LAST COPY**, and an address the guest recycles after a
  gap is invisible to the first by construction. Gotcha 235. Part 21's "0.0016%, and it
  could have been zero" was an honest answer to a smaller question than a persistent cache
  asks, and had it read a true zero the temptation to skip invalidation would have been
  much stronger.
* **What it does.** `streams` 11.1% of a crowd frame -> **0.0%**; copied bytes 61-66 MB a
  frame -> **0.23**; 97-99% of first-touch streams served across the frame boundary; 0
  overflows and 0 flushes after one growth to 256 MB.

**Then the frame-time A/B said +1.7% against a +1.3% null, and the rest of the part was
finding out why.** Publishing that would have been publishing a number I could not
account for. Two separate things were hiding the result, and both generalise:

* **Gotcha 237 — a MEAN frame time on this title measures the vblank pacing floor.**
  `tools/frame_perf_bins.py` reports means. Read as medians and binned finer, the same six
  runs say **44 ms -> 32 ms at ~3,700 draws**, and the decisive statistic is neither: the
  share of frames within 1 ms of a 16 ms multiple goes **10% -> 97%**. Arm B is
  free-running and CPU-limited there; arm A has been pushed onto the title's own floor. At
  ~6,500 draws both arms are already parked on the 48 ms three-vblank floor and 5 ms
  cannot reach 32, so the same change measures as nothing. `perf-cpu-plan.md` item 0 said
  exactly this for the TWO-vblank cap at ~1,930 draws and it was never generalised.
* **Gotcha 238 — a column that falls to zero is not a saving until the residual is
  checked.** `record`'s scope encloses the `UploadStream` calls and `ProfScope(streams)`
  wraps only the copy, so the guard hash landed in `record`, which nearly doubled. Matched
  at ~5,700 draws: streams 5.35 -> 0.00 ms, record 2.36 -> 4.23. Net 3.3 ms, not 5.5.
* **So the guard was fixed, and it was a dependency chain rather than bandwidth.** FNV-1a
  makes every byte wait on the previous byte's multiply. Folding a uint64 per step cut 512
  iterations to 64. Predicted before running (record 9.0-9.2% -> ~7.5%, streams still 0.0,
  guard bytes unchanged, GUARD MISSED still 0) and measured: **record 9.4-9.7% -> 6.4-6.5%,
  guard read 0.84 -> 0.82 MB/frame, GUARD MISSED 0 of 26.** The draw path at ~6,000 draws
  is now 9.2 ms against the store-off arm's 13.9 — **the store plus the guard fix is
  4.7 ms, a third of it.**

**The correctness counter and its control.** `CZ_VK_STREAM_CENSUS=2` computes the full
hash as well and counts every real content change the bounded-cost guard let through as a
hit — a stale buffer handed to a draw, the only defect this design can cause. It reads 0,
and it is demonstrably capable of reading otherwise (gotcha 234): under
`CZ_VK_STREAM_CENSUS_POISON=1` it reads **240,652 of 240,652**.

**Gates:** `--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean (and `gpu/pm4.cpp` is untouched
by this part). **The picture WAS re-checked, because a store can only fail by drawing the
wrong mesh:** capture E2 at frame 576 reads +0.9590 identity with the store on against
+0.9596 with it off, and the two arms' own frames correlate +0.9998/+0.9934/+0.9929/+0.9921
at matched indices.

**Also fixed:** `CZ_VK_FRAME_DUMP` silently wrote nothing when its directory did not exist,
which is indistinguishable from a renderer that drew nothing (gotcha 236).

## Part 36 (2026-08-12) — the R3 oracle unmakes item 0s's framing

The comparison the part-35 kickoff ordered as step one was run first and reframed the
item: the "junk" impostor sheets are **byte-identical to hardware's sampled bytes**
(400x240 and 1024x64, md5), decode to coherent billboard alpha-cutouts
(`tools/tex_decode.py`, new), and the "3 DXT5 / 0 DXN in hardware's frame" concern
was a filtered pass over a census that really reads 3,514 / 3,040. Five checks in one
session: the fetch-format census, the byte pairing, the decode-and-look, a 4K-prefix
search proving the "weird" 110AD000 slats texture absent from hardware's frame, and a
content-match census (226 of 459 our-frame textures byte-identical to hardware's).
The engine's own narration (1,209 [guest] lines) names no compositor. Item 0s is now
a wrong-binding question; the writer hunt is closed unfired. Gotchas 287-288;
`phase5-notes.md` §6bj; `part36-kickoff.md` is the live hand-off. Docs + one tool
only — no runtime change, part 34's gates stand.

## Part 36, second half (2026-08-12) — the reproducibility layer, and a teleport chased
## to its real cause

An operator session produced both quality levels of the tanker in one boot, and the
capture layer grew the thing every picture finding has lacked: a POSE. F9 now records
the player's world position (via the shipped debug console's own lookup — the position
is `obj + 0x1C`) and the camera matrix. `CZ_VK_TEX_FILTER_FILE` isolates textures live
while an operator plays; streaming addresses were measured stable across boots, which
is what makes a one-boot census reusable. The teleport was built, crashed, and was
diagnosed to the instruction: the engine's per-thread context lives in TLS slot 8, no
input-polling thread has it, and our pumps live in the input imports — so it was the
wrong thread rather than the wrong moment (gotcha 289). Hooking the context accessor
itself fixed the crash; the player still does not move because the actor's position
fields are outputs the engine rewrites each frame (gotcha 290). DebugJump's own spawn
code is the named lead. `phase5-notes.md` §6bk-§6bn; `part37-kickoff.md` is live.

## Part 37 (2026-08-12) — the striped-material class SOLVED: our unswizzle mask was
## correcting a correction the shader already makes

Item 0s's mechanism, found offline and closed with a headless same-binary A/B. The
chase first burned two wrong cross-platform draw pairings (the "body/cab draws" of
§6bk are street clutter; a verts-matched candidate was an NPC's head — gotcha 291),
then identified the tanker draw by CONTENT: decode hardware's textures, find the truck
skin by looking, md5 it into our dump, let the census name the draw. Every input that
draw reads is byte-identical to hardware's; Xenia's own disassembly of the shader
showed s1 is a baked LIGHTMAP read through a second UV channel — a 16_16 TEXCOORD
fetched with the microcode's compensating .yx destination swizzle. Our
g_SwappedTexcoords mask corrected the pair a SECOND time (CopySwapped already leaves
16-bit pairs in hardware's post-fetch state), so lightmap UVs arrived transposed and
the lightmap's black prop shadows painted the surfaces. The blotch site turned out to
BE the Case 0-2 DebugJump spawn, so reproduction, A/B and confirmation all ran with no
operator: mask-on blotched, mask-off clean at the matched F9 index, new-default binary
clean. Mask now defaults to zero; CZ_VK_TEXCOORD_SWAP=1 is the control arm. §6h's
metric-noise justification and §6n's frame-wide null both resolved in place. Gates:
--smoke OK; A5 exit 0 (3 permutation, 0 real); E2 identity +0.9594 (standing +0.9597).
Also advanced: cMissionTeleportPlayer's trigger disassembled (posts event 0x6A via
0x82188488, record layout known) — parked, since the spawn reaches the site.
`phase5-notes.md` §6bo; gotchas 291-292; `part38-kickoff.md` is live.

## Part 38 (2026-08-12, same day) — the operator evening: the random-texture class
## fixed at its root, and two defects cornered with same-night hardware ground truth

A two-arm operator session did in one evening what the backlog had scheduled across
parts. The part-37 class-closure tour confirmed Dick and the pawnshop clean; the
tanker instead produced the NEXT defect: its cylinder wearing a brick wall, and
"almost everything up close wears a random texture". One live dump pinned the
mechanism — the screen showed content guest memory no longer held — and the defect
was OUR texture cache uploading once per address and never refreshing while
streaming recycles addresses all session. The part-35 repair (guard + revalidate)
was promoted to default after the operator field-tested it across a full evening,
with `CZ_VK_NO_TEX_REVALIDATE=1` the control arm (gotcha 293: the census that had
kept it off was a fact about a short headless route). The shard trees were reported,
chased to the missing Xenos ALPHA TEST, which was built (pipeline-key bit + spec
constant + RB_ALPHA_REF; unknown funcs counted by name) — and honestly recorded as
NOT the foliage's mechanism: the trees are unchanged, alpha-to-mask is the suspect,
and hardware's register state at the foliage draws is on disk (item 0t). The
operator then walked the Big Buck approach on our side (flat building panels at
range, 9 F9s) and delivered R4_world the same night — eight frame-locked hardware
traces of the same approach showing fully textured buildings at every distance,
converting item 00i from "possibly the game's streaming" to OURS-with-oracle and
promoting it to top picture item. Also fixed: the window-close exit path now dumps
the renderer counters (gotcha 294 — an evening's alpha census was lost to it).
`phase5-notes.md` §6bp; gotchas 293-294; `part39-kickoff.md` is live.

## Part 39 (2026-08-12, same day) — the mip chain: an input the renderer declared and
## then threw away for the whole of phase 5

Part 38 handed over two picture items cornered against `Xenia logs/R4_world/`. Both were
worked, and both moved — one by finding a mechanism, the other by killing its suspect.

**Item 00i, the flat-panel LOD look.** The content pairing the kickoff asked for
(gotcha 291: by CONTENT, never by vertex count) landed on the Big Buck shopfront's
153-vertex sign draw, and the bytes hardware's sampler read are **md5-identical** to
ours from a different boot at a different streamed address. So the level-0 input is
exonerated, and with it two of the kickoff's three candidates: the white dummy is bound
once in most of the eight F9 frames, and `mip_min_level` is **0 on all 328,164 hardware
fetches**, refuting "the streaming system raises an LOD clamp we ignore".

What the same census found instead is that `mip_max_level` runs to **nine**, that
**88,689 of 328,164 hardware fetches (27.0%) carry a separate mip-chain address**,
and that `xenos::DecodeTextureFetch` had been parsing those fields since phase 5 while
**no line of the renderer read them** and `CreateImage` hardcoded `mipLevels = 1`
(gotcha 295). Every minified surface in this game has been sampling level 0 since the
renderer existed. The chain is now uploaded, its layout verified level by level against
hardware's own bytes rather than reasoned about — same mean, steadily fewer distinct
colours, decoded out of the trace and looked at — with the PACKED TAIL declined and
counted rather than guessed, and `CZ_VK_NO_MIPS=1` as the same-binary control arm.
1,815 textures take a chain on the outdoor route, and a divergence guard built with it
immediately caught the rule's limit: **254 of 1,818 chains had a level that is not that
texture** (a wrong pitch on levels narrower than a macro tile), so the guard rejects
rather than counts. The A/B was run twice, and the first
result was the bug rather than the feature: with those levels bound it read mean luma
−1.35% (resolved, and agreeing with hardware's darker frames); with them rejected it
reads **+0.36%, unresolved**, and distinct colours −5.45% against a 7.91% spread. The
chain is therefore justified on correctness alone and **item 00i is untouched by it**.
The registered prediction was retracted twice over — once for its sign (filtering REDUCES
distinct colours, gotcha 298) and once for its subject (gotcha 301).

**Item 0t, the shard trees: the suspect is refuted.** *(PART 40 RETRACTION: this
refutation read register 0x2205, which is RB_BLENDCONTROL1, not RB_COLORCONTROL —
at the correct 0x2202 hardware enables the alpha test on 12.2% of those same draws,
and the suspect was right all along. Gotcha 308.)* RB_COLORCONTROL read across all
eight R4 traces — **40,703 draws** — enables neither the alpha test nor **ALPHA-TO-MASK**
anywhere, so the emulation the item asked for would have been built against a mode this
title does not use. Nor is it a shader `kill`: 1 of R4's 208 pixel shaders has one,
where our own bank reports 324 of 324 because XenosRecomp emits its alpha-test clip
unconditionally (gotcha 296 — a saturated count is a question about the generator). The
item now needs one thing: a round-5 trace **standing at a shard tree**, because that
material is absent from R4's bank.

Also: SIGTERM and SIGINT now dump the renderer counters. Gotcha 294 fixed the
window-close path and left the headless twin open — every recipe here ends a run with
`timeout`, so the arm carrying the interesting counters was the one that never printed
them (gotcha 297).

`phase5-notes.md` §6bq; gotchas 295-297; `part40-kickoff.md` is live.

## Part 40 (2026-08-13) — the shard trees solved: RB_COLORCONTROL was at the wrong index

Read all 23 of part 39's draw-ID maps (named the foliage material by footprint shape;
retracted the "hair correction" and the untextured-draw finding against hardware),
extended `CZ_VK_TEX_DUMP` to compressed formats and added the shader-keyed
`CZ_VK_TEX_DUMP_PS` (gotcha 306: an address does not survive a reboot), exonerated the
foliage textures (alpha planes perfect), refuted the mip/exp_adjust/texcoord-swap
theories (gotcha 307: the over-accepting scanner), built a headless viewpoint facing
the trees, and ran the three-arm A/B whose `CZ_VK_NO_DEPTH_FETCH` arm split the defect
in two (darkness = shadow term; cutout separately missing). The caster shader's
alpha-sampling body plus hardware's RB_ALPHA_REF=0.502 on "disabled" tests broke the
case: `kRbColorControl` was 0x2205 (RB_BLENDCONTROL1); it is 0x2202. Part 38's test
had never fired; part 39's refutation measured the wrong register (hardware: 12.2% of
draws alpha-tested). Fixed in xenos.h + the .xtr tool; trees now cut out and lit;
title gate +0.947; gotcha 308. Commits 449bb5c, 81906f5, f865579, ed46db7.

## Status-block archive: parts 28-42 (+ the superseded mid-part-44 closure), moved out of CLAUDE.md on 2026-08-15

These are CLAUDE.md's own 'Where the port was' blocks, verbatim, oldest
first — the state summary each part left behind. The narrative entries
above and the phase notes stay authoritative where they overlap.

Where the port was, briefly, mid-part 44 (superseded same day — kept for the
census records):

* **ITEM 00i CLOSED: fresh hardware shows the flat-at-range class at the same
  rate we do; the "hardware is full everywhere" oracle was a warm loaded-save
  session.** Two censuses (`phase5-notes.md` §6bx + addendum): our MENU frame
  equals B1's title era bind for bind (the 8×8-on-big-meshes draws are
  `flat_color_gray_cm.bct`, 346 bytes, flat BY DESIGN — part 43's "the defect
  is the set APPLY" premise failed its never-run control); and B2 — a FRESH
  hardware session walking into Still Creek, on disk since day one — runs
  2–3% tiny-on-big through the whole town era with the world shader binding
  an 8×8 on thousands of draws, persisting to session end (no promotion wave
  exists fresh). The texture LEVEL MACHINE is fully named and verified healthy
  on our runtime (name hash, DB entry layout, promote/bind/catch-up walks,
  levels: 0 = full set payload, 1 = thumbnail); `CZ_SET_APPLY_PROBE=1` prints
  every gate of it. Operator comparisons must be like-for-like (fresh vs
  fresh, or same save on both). The one open curiosity: WHAT a save carries
  that makes a warm session all-full (skip bits the candidate) — nameable by
  loading the operator's tanker save with the probe, not a defect either way.
  **Big-trace censuses OOM the stock `xtr_draw_bindings.py` three ways** —
  the validated lean variant (rolling memory window + streamed CSV,
  byte-identical on B1) lives in `~/DR2CZ-troubleshooting/part44/`, and a
  multi-GB CSV must never target /tmp (the tmpfs filled mid-run and every
  shell command "died" until space was freed). **`docs/part45-kickoff.md` is
  the LIVE hand-off**; the sledgehammer-pickup FREEZE (signal-15, next
  operator session carries `CZ_WAIT_TRACE=1`) is the top item.

Where the port was, as of 2026-08-10 (phase C part 28):

* **THE ENGINE NARRATES ITSELF NOW, and it cost one byte** (part 28). `CZ_GUEST_DIAG=1`
  clears `0x829EC974` — read by 2,013 diagnostic and assert sites, written by none, and
  shipped as 1. It is a release KILL SWITCH, not the "flag per category a shipped build
  left at zero" that gotcha 215 guessed for thirteen parts (gotcha 266 corrects it). With
  `CZ_GUEST_LOG=1` the outdoor route goes from **0 `[guest]` lines of 11,168 to 1,239**.
  **Reach for it first on any question of the form "what does the title think it is
  doing"** — zone streaming, load timings, heap headroom and the engine's own asserts are
  all behind it. Diagnostic arm only; never quote a frame time from a run with it set.
* **LOD POPS IN LATE — operator report, and it is PARKED BEHIND THE WHITE SURFACES**
  (`open-items.md` 00i). LOD here is STREAMING, not a distance curve: no LOD-distance
  scalar is named anywhere in the executable. The first pass found the per-zone decision
  working (3 zones full, 4 LOD), no streaming failure of any kind, healthy heaps and the
  full 447 MB granted. **The oracle was asked and could not decide it**: hardware's LOD
  swaps are far less visible because hardware's textures are not broken, so while item 00f
  flattens world surfaces to `rgb(180,180,180)` the two arms are not comparable on this
  axis. **Re-ask only after 00f/00g, and do not build the one candidate fix
  (`KeSetBasePriorityThread` is a no-op) before then** — it would target an unproven defect
  and be measured against a picture that cannot report whether it worked.

Where the port was, as of 2026-08-11 (phase A/V):

* **THE GAME MAKES SOUND, AND THE PROLOGUE CINEMATIC PLAYS.** Both from one fix.
  `docs/phase-av-notes.md`; the plan it executed is `docs/phase-av-plan.md`.
  The XMA decoder is `runtime/audio/xma_decoder.cpp` (ffmpeg, lifted from Fable 2) driven
  by a context walk in `kernel/audio.cpp`; output is `runtime/audio/audio_out.cpp` (SDL).
  **The defect underneath was one address**: the XMA context's buffer pointers are
  PHYSICAL — the APU is a DMA device — and our flat map puts the physical arena in a
  window at 0xA0000000, so reading them literally gave a page of zeros that decodes to
  silence and reproduces the symptom exactly. `NtReadFile ... -> 131072 into A2538000`
  next to `[xma] ctx0 in0=02538000 ... 0 non-zero` is the whole finding (gotcha 267, and
  a Case West item on day one). `maxpeak` 0.000000 -> 0.108854, non-silent 0 -> 15,991
  of 18,433.
  **The prologue cinematic's freeze was that same silence**: same binary, one variable,
  longest frozen camera run **10,513 frames -> 159** and presented coverage **15.00% ->
  99.94%**. `CZ_NO_XMA_DECODE=1` and `CZ_NO_AUDIO_OUT=1` are the two control arms.
  **Part 16 had refuted this with a three-configuration arm and the refutation was wrong**
  — read gotcha 268 before trusting any negative result taken on a stub.

Where the port was, as of 2026-08-11 (part 29):

* **CINEMATICS PLAY TO COMPLETION WITH SOUND — operator-confirmed on two of them, and
  the fix was one field.** `open-items.md` 00j is CLOSED. The XMA packet walk advanced one
  packet at a time; a 2 KB XMA2 packet's header carries **`packet_skip`**, the number of
  packets to step over to reach the next packet OF THE SAME STREAM. That is a no-op for
  mono and stereo — this title's music, SFX and one-shot voice lines, where it is always
  0 — and wrong for 5.1, which the 360 decodes as **several interleaved 2-channel streams
  in one packet stream, one XMA context per pair**. That is why the prologue's contexts 5,
  6 and 7 all pointed at input buffer `02584000`, an oddity this project noticed twice and
  never explained. Each of them was decoding the others' packets: **4.916 s produced from
  a buffer holding 1.845 s, 2.66x**, so the rings filled ~3x faster than the title's mixer
  drained them and the whole voice group wedged after one buffer. Gate on the prologue:
  cinematic-era `runs/distinct` **120 -> 1.00 in every quarter**, the scene's audio clock
  **4.906667 s frozen -> 310.7 s of a 316.5 s track**, `audio/cinematics.big` read
  **2 -> 201** times. A second, separate defect was fixed on the way and is recorded as
  having moved the gate by nothing on its own: the walk retired a spent input buffer by
  unconditionally switching to buffer 1, which this title never uses (136 context dumps,
  `in1Ptr` 0 in every one), parking the context unrecoverably.
  **Both are Case West items on day one**, with gotcha 267's physical addresses.
  The mechanism underneath the symptom was the title's own **`Cine.Audio` PID** on audio
  latency (`sub_824741D8`, shipped mode 2), which returns *setpoint minus an accumulator*
  and so hunts backwards when its input stops. `CZ_CINE_TIME` and `CZ_CINE_AUDIO_MODE`
  remain as the instrument and the arm; the gains are shipped values and were never wrong.
* **The 00j gate was diluted 6x and every recorded reading has it.** `runs/distinct`
  over a whole prologue run reads 6.14 because ~1,870 menu frames contribute 1,010 of
  the 1,170 distinct poses; the cinematic era alone is **38.27** and steady state is
  **15 poses at 120**. `tools/frame_loopiness.py` prints era quarters now. Quote those,
  and read draws beside them — the gate cannot tell a stalled scene from a parked
  player.

Where the port was, as of 2026-08-11 (part 30):

* **THE WHITE-SURFACE CHAIN HAS NUMBERS IN IT NOW, AND TWO OF ITS STEPS ARE GONE.** The
  tone curve every one of the 48 emitting shaders ends in is
  `out^2 = (max(0.25x + 0.75, 1) - saturate(1-x)^2) * 0.5` with `x = colour * pc(14).w`,
  and **our constants are hardware's to the digit** — a pre-registered prediction that
  they were not, refuted. So **180 is the value at exactly full exposure**, not a ceiling
  and not a gamma artifact: it is `sqrt(0.5)` written by the shader's own trailing `sqrt`.
  Our translation of `ps_ad65b98593f95926` is **instruction-for-instruction identical** to
  the capture's own disassembly, so the clamp is on an INPUT. Two retractions in place —
  the `k_8_8_8_8_GAMMA` reading of 180, and "these surfaces are not shaded at all" (the
  curve's derivative vanishes at `x=1`, so a 10% spread quantises to one 8-bit value).
  **What is owed is enumerated: the ground shader reads 32 pixel constants and nine have
  been compared.** `tools/xtr_draw_constants.py` reads hardware's;
  `CZ_VK_PS_CONST_SCALE="14.w=4"` is the arm that separates a pinned colour from one the
  curve is hiding. `docs/phase5-notes.md` §6ba, `open-items.md` 00f.
* **A CAPTURE THAT CANNOT ANSWER IS ONE CAPTURE, NOT THE SET.** Part 27 asked `w1_spawn`
  for the constants, got `UNRECOVERABLE`, and recorded that a new capture was needed; five
  of the other six answer, from data on disk for weeks (gotcha 274).
* **`timeout` WORKS AGAIN FOR HEADLESS RUNS WITH SOUND.** Since phase A/V it did not:
  `SDL_HINT_NO_SIGNAL_HANDLERS` sat below the `CZ_NO_WINDOW` early return and the audio
  device is a second SDL entry point, so every such run overran its recipe silently —
  the symptom is a LONGER successful run, which nothing reports (gotcha 272). Any per-run
  duration quoted from an audio-enabled headless run between phase A/V and part 30 is
  suspect.
* **THE XMA DECODER COSTS NO FRAME TIME.** Three runs an arm, alternated, null first:
  every draw-count bin medians 32.0 ms in both arms, largest mean difference 0.2% against
  a 0.6% null. Bound: the workload is pinned at the two-vblank floor in both arms, so this
  says the decoder does not push frames off the cap. A frame rate quoted from a build with
  audio no longer needs qualifying.

Where the port was, as of 2026-08-11 (part 31):

* **THE WHITE PLATEAU IS NOT THE TONE CURVE AT `x = 1`, AND FOUR PARTS OF WORK WERE
  PROSECUTED UNDER THAT READING.** Four whole-frame arms leave the pixels at exactly
  `rgb(180,180,180)` untouched: the sun `c24`, the additive `c67.w` term, the whole
  multiplicative path `c1.xyz` — which blacks out **61.5%** of the frame — and, decisively,
  `CZ_VK_PS_CONST_SCALE="14.w=0.25"`, which engaged on 11,835,619 draws, took the scene
  buffer from mean luma **35.07 to 18.30**, and put **zero** pixels on 119, where that
  curve sends `x = 1` at quartered exposure. A value produced by that curve cannot be
  invariant under scaling its exposure. **`180 = 255 * sqrt(0.5)` is now the coincidence
  to explain, not the explanation** (gotcha 277), and the next step is a PER-DRAW
  instrument — `CZ_VK_DRAW_CENSUS` plus `CZ_VK_SKIP_TEX` — because four arms in a row
  reporting "unmoved" is a fact about the instrument class (gotcha 276).
  `docs/phase5-notes.md` §6be, `open-items.md` 00f.
* **THE SHADOW ATLAS IS FIXED — `open-items.md` item 3, open since part 15.** The title
  packs four 1024x1024 cascades into one 4096x1024 atlas by pre-offsetting
  `RB_COPY_DEST_BASE` by 0x20000 each while leaving the scissor at the origin, and 0x20000
  is exactly +1024 texels in X in tiled address space. `DoResolve` understood the SCISSOR
  form of that idiom and not the ADDRESS form, so three quarters of every shadow lookup
  read zero. **Ours was 86.7% empty; hardware's copy of the same surface, dumped from the
  capture, is 3.5% empty.** Now 53.125% non-zero across all 4,096 columns against 13.281%
  across 1,024 with `CZ_VK_NO_ADDR_TILE_FOLD=1`, and `13.281% x 4 = 53.125%` exactly.
  **A surface you RENDER is still comparable** (gotcha 275) — the check took ten minutes
  and had been available since part 26.
* **THE GROUND SHADER'S 32 CONSTANTS ARE HARDWARE'S, and the constants are exonerated as
  a class.** Every register that is not a function of the camera matches to the printed
  digit, including `pc(21)` — a point light's WORLD POSITION — which is how the run proved
  it was in hardware's own lighting state without being asked to. All seven R2 captures
  answer; part 27's "`w1_spawn` cannot" was true only of `c253..c255`.
* Exposure is now readable per frame (`CZ_VK_EXPOSURE_TRACE`): ONE value is in force
  across a whole frame, to five digits. That had been assumed for four parts.

Where the port was, as of 2026-08-11 (part 32):

* **HALF OF EVERY SHADOW CASCADE IS ZERO, AND A ZERO DEPTH SAMPLE READS AS OCCLUDED.**
  The atlas is **46.8750% zero in every band** — rows 512..1023 minus a 64-column sliver —
  on two routes and two frames, in four bands rendered from four different light frusta,
  so it is structural. The geometry is submitted for all of it (`CZ_VK_DEPTH_ALWAYS`:
  46.875% -> **1.86%**); the bottom half is REJECTED against the zero the EDRAM image was
  created with. **The cause is that a Xenos 4x MSAA surface is twice as TALL in samples as
  well as twice as wide, and only X has ever had the factor**: the title's two clear rects
  for the cascade — `(0,0)-(480,512)` on a 520-pitch 4x surface and `(960,0)-(1024,1024)` —
  tile the 1024x1024 map EXACTLY when both axes are scaled and cover **53.125%** of it when
  only X is, which is the observed coverage to four decimals.
  `CZ_VK_MSAA_WINDOW_SCALE_Y=1` -> **0.0038% zero**, title-screen picture unmoved. It is
  an ARM, not the default, because the SCENE tile's 4x clear wants X scaled and Y not, and
  that has to be reconciled first. `docs/phase5-notes.md` §6bf, `open-items.md` item 3.
* **THE HARDWARE ORACLE PART 31's SHADOW WORK WAS MEASURED AGAINST IS RETRACTED.** The
  16 MB `xtr_draw_bindings.py --dump-texture 1812F000` returns from `w1_spawn` is the
  PREVIOUS FRAME'S COMPOSITED SCENE — detile it and the game's HUD is legible, *"8
  KILLED"*. A `.xtr`'s memory records are snapshots with a time, so a capture **cannot**
  supply any surface the GPU produces inside the traced frame (gotcha 280, correcting
  gotcha 275's second half). The address-fold fix stands; the "hardware's is 96.5% full"
  yardstick never existed. The tool exits 2 on that dump now and prints *"a sound oracle"*
  otherwise; part 27's ground-texture comparison is unaffected.
* **`CZ_VK_NO_DEPTH_TEST` CANNOT ANSWER A DEPTH-PASS QUESTION AND ANSWERS THE OPPOSITE.**
  Vulkan ties depth WRITES to the depth TEST, so on a depth-only pass the arm empties the
  buffer — the very symptom it exists to rule out (gotcha 279). `CZ_VK_DEPTH_ALWAYS` is
  the replacement.

Where the port was, as of 2026-08-11 (part 33):

* **THE WHITE-SURFACE PLATEAU IS SOLVED — open item 00f, open since part 26, and the
  largest picture defect in the port.** The exact-`rgb(180,180,180)` surfaces were the
  shared tone epilogue evaluated at **`x = NaN`**: `max(NaN,K1)=K1` and `saturate(NaN)=0`
  make every NaN input land on `sqrt(K1*K2)` = 180/255, invariant under every constant —
  which is why parts 27-31's four whole-frame arms all read "unmoved" (gotcha 281: the
  laundering happens INSIDE the epilogue, so `isnan(oC0)` was blind by construction).
  The NaN entered at the VERTEX FETCH: this title wraps its fmt16 `k_10_11_11` packed
  normals as TEXCOORD (float4 input) while the runtime bound the attribute `R32_UINT` —
  a pipeline type mismatch (VUID 08733, 10 pipelines, 37 vertex shaders) that delivered
  the packed dword's bits AS floats: NaN wherever bits 30..23 are set, garbage normals
  on every fmt16 mesh otherwise, since phase 5. **One `CZ_VK_VALIDATION=1` run would
  have named it in part 26** (gotcha 282). Fixed in XenosRecomp (`XeUnpack_10_11_11`,
  read-site format branch) + runtime (fmt16 -> `R32_SFLOAT`): plateau **1,092 px -> 0**,
  scene mean luma 35.5 -> 44.7, distinct colours 80k -> 112k, validation 08733 10 -> 0,
  and the crowd's blotchy flat-lit patches are gone. The measurement chain (five paint
  arms, a 786,861-draw range census, and the no-test robust arm caught before its null
  was believed) is `docs/phase5-notes.md` §6bg. **The operator confirmed it the same
  day: all seven part-27 white-surface locations toured and captured on the fixed
  renderer, scene-buffer plateau ZERO in every one**
  (`~/DR2CZ-troubleshooting/part33-operator/`). Owed: a re-measure of the exposure
  discrepancy (ours 1.0 vs hardware 0.33) now that auto-exposure adapts to a correctly
  lit scene — §6ba's question may simply close. The NaN-input footprint was 17x the
  visible plateau, so LOD/00i, NPC part meshes and the shadow three-way should all be
  re-asked on this renderer.

Where the port was, as of 2026-08-11 (part 34):

* **THE 4x MSAA Y FACTOR IS THE DEFAULT — part 32's item 0, shipped, and the shadow
  cascade's two known defects are both fixed and on by default.** A Xenos 4x surface is
  a 2x2 sample grid — twice as tall in samples as well as twice as wide — and for 25
  parts only X had the factor at the window-coordinate site. The reconciliation that
  unblocked it (§6bh): a clear rect is in the CLEAR declaration's own pixel space, so
  both axes scale by the draw's OWN declared sample factors — no rule needs the render
  declaration at clear time, and the Y over-clear past a shorter surface is the same
  approximation the X factor has always applied to the 640x360 post surface.
  `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` is the same-binary control arm (the part-33
  renderer); the part-32 arm variable is retired. Gates: atlas **46.8750% -> 0.0038%
  zero, 512 -> 1024 covered rows** (§6bf's numbers to four decimals); no other surface
  regressed; outdoor era medians **distinct colours +8.30% at 5.2x the null**
  (registered prediction, commit e10df05); validation tally unchanged; capture-E
  **+0.958 identity**; A5 **exit 0, 3 permutation, 0 real**; `truncated=0`; both PM4
  oracles exit 0. **The operator's three-way verdict landed the same day and CLOSES
  open item 3**: one Case 0-2 crowd spot, F9 per arm — the default is *"perfect"* in
  their words (atlas 0.0006% zero at the capture); both control arms show hard-edged
  black false-occlusion blotches on the same truck (46.875% / 75% atlas zero). §6bh's
  verdict table; `~/DR2CZ-troubleshooting/part34-operator/`.
* **§6ba's EXPOSURE QUESTION IS DOWNGRADED TO CLOSED-PENDING-A-MATCHED-LOCATION.**
  `CZ_VK_EXPOSURE_TRACE` on all three outdoor runs: every arm identical (the shadow
  change does not move the controller), frame 3000 reads 0.211 (part 31: 0.2146), era
  range **0.200-0.354**, mean 0.2755 — and hardware's **0.298 / 0.331** sit INSIDE that
  adaptive range. "Ours pinned where hardware reads 0.33" is not a property of the
  fixed renderer. The free completion: the trace beside an operator F9 at `w1_spawn`.

Where the port was, as of 2026-08-12 (part 35):

* **THE WRONG-TEXTURE REPORTS COLLAPSED INTO ONE ITEM, AND IT IS FULLY
  EVIDENCE-BOUNDED: open-items 0s, THE STRIPED-MATERIAL CLASS.** One streamed quality
  level of an asset renders as black/white banded garbage (the tanker close up, Dick
  the survivor at distance, the pawnshop's window boards). An operator session plus
  live-process texture dumps taken seconds after each F9 (`tools/live_texdump.py`,
  gotcha 285) established: guest memory GENUINELY holds the garbage at blotch time,
  and **every reader of the bytes is measured innocent** — five theories died in one
  session (the shadow term; a VFS positional-IO race, fixed on principle in d65874d
  with its prediction honestly retracted, overlap counter 0; a stale texture cache,
  content guard 4 of 92,730,622 hits; the snapshot age fallback; one retracted
  misattribution). The affected textures include CPU-composed impostor sheets that
  exist nowhere on disc and are never resolve destinations. **The next move is named
  (gotcha 286): trace the WRITER** — and the oracle arrived the same night:
  **`Xenia logs/R3_world/`**, four frame-locked single-frame traces at exactly the
  four defect sites, hardware clean at every one, each trace carrying the bytes the
  sheets SHOULD hold. `docs/phase5-notes.md` §6bi; `docs/part35-kickoff.md` is the
  LIVE hand-off.
* **Item 3d (NPC part meshes) is CLOSED** — Dick renders whole on two binaries; the
  missing parts were the shader-cache gap, exactly as the item's re-test note
  predicted. **Item 00i (LOD placeholder pop) is captured** (flat colour panels ->
  full siding, reload_test 30631/30807) and one deliberate Xenia look from a verdict.
* **NtReadFile/NtWriteFile are atomic per handle now** (d65874d) — NT's contract,
  previously seek-then-read on a shared FILE*. `CZ_FILE_RACY=1` is the control arm.
  Credited with nothing visible; kept on correctness.

Where the port was, as of 2026-08-12 (part 36, first half):

* **ITEM 0s IS REFRAMED: THE "JUNK SHEETS" ARE CORRECT TO THE BYTE, AND THE WRITER
  HUNT IS CLOSED BEFORE IT STARTED.** Part 36 ran the R3-oracle comparison first, as
  ordered: our live-dumped 400x240 and 1024x64 impostor sheets at blotch time are
  **md5-identical to the bytes hardware's GPU sampled** for the same material in
  `tanker.xtr`, and decoded properly (`tools/tex_decode.py`, new) they are coherent
  billboard alpha-cutouts — white colour endpoints, content in ALPHA, exactly what
  part 35's junk-scorer was guaranteed to misflag (gotcha 287; the kickoff's own
  warning said to decode and LOOK first). The kickoff's "hardware binds only 3 DXT5,
  0 DXN" was a filtered pass — the full census is 3,514/3,040, hardware draws the
  whole sheet class. Item 0s is now a WRONG-BINDING question (a real asset at the
  wrong streamed quality level — the "weird" 110AD000 texture is structured real
  content absent from hardware's frame), with the blotched-draw identification as
  step one and hardware's 16 small colour resolves (never in our resolve set;
  resolve write-back still unimplemented) as the standing lead for the remaining
  sub-defects. `docs/phase5-notes.md` §6bj; **`docs/part36-kickoff.md` is the LIVE
  hand-off.** Content-match census: 226 of 459 blotch-frame textures byte-identical
  to hardware's frame; the unmatched are unadjudicated, not suspects (gotcha 288).

Where the port was, as of 2026-08-12 (part 36, second half — the reproducibility layer):

* **AN F9 CAPTURE NOW RECORDS WHERE YOU WERE STANDING, AND TEXTURES CAN BE ISOLATED
  WHILE AN OPERATOR PLAYS.** Every picture finding in this port had been anchored to
  "the operator walked somewhere and pressed F9", which nothing can reproduce. The
  capture now writes `capture_<frame>.pose` beside the picture: the player's world
  position, read through the shipped debug console's own path (**the position is
  `obj + 0x1C`**, reached by a five-step lookup `setplayerpos` and `getplayerinfo`
  share), plus the camera constants. Read it with `tools/pose_read.py`.
  `CZ_VK_TEX_FILTER_FILE` isolates or hides a texture **live, no relaunch**, which the
  latched env vars could never do for a defect that picks a different quality level per
  boot — and **streaming addresses are stable across boots** (703 of 712 shared
  addresses held identical content between two sessions), so a census from one boot
  names textures usable in the next. **A teleport is diagnosed but unfinished**: the
  crash was the wrong THREAD (the engine's per-thread context lives in TLS slot 8 and no
  input-polling thread has it — gotcha 289), fixed by hooking the accessor itself; but
  the actor's position fields are OUTPUTS the engine rewrites every frame (gotcha 290),
  so the working placement path is DebugJump's own spawn code, not `setplayerpos`.
  `docs/phase5-notes.md` §6bk-§6bn; **`docs/part37-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 37 — the striped-material class is solved):

* **ITEM 0s'S BLOTCH MECHANISM IS FOUND, FIXED, AND ON BY DEFAULT.** The
  black/white-banded "striped material" garbage (tanker close-up, Dick at distance,
  pawnshop boards) was never in any texture: the runtime's own 16-bit texcoord
  unswizzle mask double-corrected fetches whose microcode already carries the
  compensating `.yx` destination swizzle, so baked-LIGHTMAP UVs arrived transposed and
  the lightmap's black prop-shadow shapes painted the surfaces. Mask now defaults to
  zero (= hardware semantics; the fix), `CZ_VK_TEXCOORD_SWAP=1` repaints the blotch as
  the same-binary control arm. Named by content-ID of the tanker draw (all of whose
  inputs are md5-identical to hardware's), semantics read from Xenia's own disassembly
  of the same microcode, verified by matched-index F9 A/B at the reproduced site — the
  blotch site IS the Case 0-2 DebugJump spawn, so the whole loop ran headlessly.
  `docs/phase5-notes.md` §6bo; gotchas 291 (identify draws by CONTENT, not vertex
  count) and 292 (model the full state chain before adding a runtime correction).
  **`docs/part38-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 38 — the operator evening: the random-texture
class fixed, two defects cornered with hardware ground truth):

* **THE TEXTURE CACHE REVALIDATES BY DEFAULT — the "random texture on everything up
  close" class is fixed and operator-confirmed.** The once-only upload cache served a
  streaming-recycled address's first occupant forever (a tanker wearing a BRICK WALL;
  guest memory holding a pickup atlas by dump time). Part 35's "4 stale of 92M" was a
  short-route census (gotcha 293). `CZ_VK_NO_TEX_REVALIDATE=1` is the control arm.
  This also retro-explains item 0s's per-boot "wrong quality level" lottery. §6bp.
* **The part-37 class-closure tour confirmed**: Dick at distance and the pawnshop
  boards clean on the fixed renderer.
* **The RB alpha test is built and driven** (pipeline-key bit + spec constant +
  RB_ALPHA_REF per draw; unknown funcs counted, never guessed) — and the SHARD TREES
  are NOT it: foliage never fires the RB alpha test. Suspect ALPHA-TO-MASK (bit 4);
  hardware's register state at the foliage draws is in `Xenia logs/R4_world/`.
  New item 0t. `CZ_VK_NO_ALPHA_TEST=1` is the arm for what was built.
* **Item 00i IS OURS AND IS NOW THE TOP PICTURE ITEM**: the operator walked the Big
  Buck approach on our renderer (flat-color building panels at range) and delivered
  `R4_world/` the same night — eight frame-locked hardware traces of the same walk,
  fully textured buildings at every distance. Eight paired oracles for the fix.
* Window-close now dumps the renderer counters (gotcha 294 — a whole session's
  census was lost to the one exit path a human actually uses).
* **`docs/part39-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-12 (part 39 — the mip chain, an input the renderer
declared and then discarded for the whole of phase 5):

* **THE GUEST'S MIP CHAIN IS UPLOADED NOW, AND IT WAS NEVER READ BEFORE.** A Xenos
  fetch constant names TWO addresses: dword1's base holds level 0 alone, dword5's
  `mip_address` holds levels 1..n. `DecodeTextureFetch` parsed `mipMin`/`mipMax` from
  phase 5 onward, **nothing read them**, and `CreateImage` hardcoded `mipLevels = 1` —
  so every minified surface in the game sampled full-resolution texels at whatever rate
  the rasteriser landed on. Hardware carries a mip-chain address on **88,689 of 328,164 fetches (27.0%) across
  all eight R4 traces**, declaring chains up to **nine levels** deep; our own outdoor
  frame declares one on **69.6%** of fetches. The layout was verified level by level against hardware's own bytes (same
  mean, steadily fewer distinct colours) rather than reasoned about; the **packed tail**
  is declined and counted, never guessed. `CZ_VK_NO_MIPS=1` is the same-binary control
  arm; 1,815 textures take a chain on the outdoor route. A divergence guard shipped with
  it caught the rule's limit at once — **254 of 1,818 chains held a level that is not that
  texture**, so it REJECTS rather than counts (gotcha 301). **The A/B was run TWICE and the first
  result was the bug**: with the 254 bad levels bound it read mean luma −1.35% (resolved,
  and moving toward hardware's darker frames); with them rejected it reads **+0.36%,
  unresolved**. So the chain produces no resolvable era-statistic change and is justified
  on correctness alone, and **item 00i is untouched by it** — the packed tail, where the
  deepest minification lives, is still declined. Gotcha 295 is the transferable half: **for every field a decoder
  parses, grep for a READER** — and 298: **"distinct colours" rewards ALIASING**, so the
  registered prediction had its sign wrong. §6bq.
* **Item 00i's level-0 input is EXONERATED** — the Big Buck sign draw's texture bytes
  are **md5-identical** to hardware's from a different boot at a different address — and
  `mip_min_level` is **0 on all 328,164 hardware fetches**, refuting "streaming raises an
  LOD clamp we ignore". Whether the mip chain closes 00i is a registered prediction
  against an era-median A/B, not yet a claim.
* **Item 0t's suspect is REFUTED**: across all eight R4 traces (**40,703 draws**)
  hardware enables neither the alpha test nor **ALPHA-TO-MASK**. Do not build the
  emulation. The item now needs one round-5 trace **standing at a shard tree**.
* SIGTERM/SIGINT dump the renderer counters — gotcha 294's headless twin (gotcha 297).
* **`docs/part40-kickoff.md` is the LIVE hand-off.**

Where the port was, as of 2026-08-13 (part 40 — the shard trees SOLVED, and the cause
was one wrong register index):

* **RB_COLORCONTROL IS 0x2202, NOT 0x2205 — and the whole shard-tree class (item 0t)
  falls out of that one constant.** `xenos.h` guessed the per-RT blend controls
  contiguous at 0x2201..0x2204; the real map interleaves them
  (0x2201/0x2205/0x2209/0x220D) with COLORCONTROL at 0x2202, settled by histogramming
  both indices over an R4 trace (0x2202's values carry the 0xAA alpha-to-mask offset
  signature; 0x2205 never sets the enable bit). Consequences unwound: part 38's alpha
  test NEVER FIRED (its counter read zero in every log, unread), and part 39's
  "hardware never enables the alpha test in 40,703 draws" measured the WRONG REGISTER
  — the truth is **4,975 of 40,703 (12.2%)**: foliage, fences, hair, horizon sheets,
  and 1,787 draws of the shadow-caster shader whose whole body is "sample alpha, clip
  against RB_ALPHA_REF". With the index fixed the trees have cutout leaves and lost
  their dark plates (the caster now clips leaf holes into the shadow map instead of
  stamping the solid quads whose projected shadows WERE the plates). Same-binary arm:
  `CZ_VK_NO_ALPHA_TEST=1`. §6bs, gotcha 308, `open-items.md` 0t.
* **The path there is reusable:** a headless viewpoint FACING the trees (DebugJump
  spawn + two RSLEFT camera holds + synthetic F9 — F9 works in `CZ_FAKE_PRESS_SEQ`),
  then a three-arm A/B in which `CZ_VK_NO_DEPTH_FETCH=1` lit the plates (darkness =
  the shadow term) while still showing opaque cards (cutout separately missing), and
  the F9 atlas snapshot showing the solid diamond cards the caster stamped.
* **`CZ_VK_TEX_DUMP` can finally see DXT textures** (it was gated to 8-bit formats
  for its whole life — blind to nearly every texture in this game), and
  `CZ_VK_TEX_DUMP_PS=<hash>` dumps a material's textures keyed by SHADER, which
  survives a reboot where a guest address does not (part 39's foliage addresses,
  replayed by address, decoded as BARBED WIRE — gotcha 306).
* **Parts 40's second half added two more cutout fixes**: strict GREATER (ref + 1/512
  — the foliage cuts out OPAQUE cards at ref 0.0 and the emitted clip is >=-shaped)
  and EQUAL@1.0 emulated exactly (ref - 1/512; the shadow-caster cutout, 174 draws,
  which was blacking canopy interiors). A2M-without-test stays counted; EQUAL below
  ref 1.0 stays counted.
* **THE FAR FIELD IS THE OPEN FRONT, and `docs/part41-kickoff.md` is THE DISTANCE
  PLAN and the LIVE hand-off**: anisotropic filtering (never enabled), the packed mip
  tail (still declined), item 00i's streaming pop (now pairable against the 81-capture
  ~1 m-spaced operator walk in `~/DR2CZ-troubleshooting/part40-operator/verify/`),
  A2M feathering at distance, then edge fringes/exposure/fog.

Where the port was, as of 2026-08-14 (part 41, first session — the far field: per-fetch
samplers and the packed mip tail, both default):

* **PER-FETCH SAMPLERS ARE THE DEFAULT (d5b8fdc) — the fetch constant's own
  mag/min/mip/aniso fields, honoured for the first time since phase 5.** The world's
  albedo filters at the 4:1/8:1 the title asks for; the shadow atlas gets the
  point/point/point/no-aniso sampler hardware asks for. Confirmed by a three-arm
  same-binary A/B: outdoor sharpness +2.6% with NO overlap at three runs an arm, no
  era regression. `CZ_VK_NO_FETCH_SAMPLERS=1` is the part-40 arm; `CZ_VK_ANISO=N`
  caps the degree. **The GLOBAL-aniso form was tried first and is REFUTED BY
  PICTURE — do not re-buy it**: 16x on sampler 0 speckles the whole frame dark,
  because sampler 0 also serves the shadow-map lookups and hardware fetches that
  atlas with aniso=0/POINT (621-fetch census, §6bt). The sharpness metric read the
  regression as a -19% DROP — a metric can flag a defect while mislabeling it; the
  matched F9 eyeball named the mechanism in one look.
* **THE PACKED MIP TAIL IS DECODED (409777d) — part 39's standing decline, paid.**
  No remembered table: `tools/packed_mip_derive.py` brute-forced the offsets from
  hardware's own bytes over all eight R4 traces — a square packed level of width W
  blocks sits at block (W,0) in the shared tile, 7,466 of 7,515 informative votes,
  both DXT formats. ~4,468 tail levels upload per outdoor run (73 guard-rejected,
  302 mostly-empty — everything underived still declines and counts).
  `CZ_VK_NO_MIP_TAIL=1` is the tail-only part-39/40 arm. Era statistics unresolved
  (like the chain itself in part 39); justified on correctness.
* **`docs/part41-kickoff.md` remains the LIVE plan** — items still open: the
  operator's far-field verdict (their mandate started this part), 00i pairing on
  the 81-capture walk (item 3), A2M dither at distance (item 4), clamp modes /
  edge fringes (item 5 + 1b's clamp half). `docs/phase5-notes.md` §6bt is the full
  record of this session.

Where the port was, as of 2026-08-14 (part 41, second session — the far-field
complaint is LOCATED: it is the DoF COMPOSITE, and the scene under it is sharp):

* **CORRECTED BY THE OPERATOR: their complaint is the FLAT-TEXTURE class (00i),
  not softness** — buildings/objects render with NO surface pattern at range
  (their captures 003053/003368). 00i is back on top; the census tracker failed
  to pair faces across the walk, so the next move is CZ_VK_DRAW_ID at a
  reproduced flat-building view, then the binding comparison against R4 at
  matched distance. What the session ALSO found (secondary, own evidence):
  **the far-field SOFTNESS is the post chain, not sampling** — their 20 F9 captures bisect it: the resolved scene
  surface is crisp at every distance, the DoF composite (lerp by the blur
  surface's alpha) is what the player sees. Our real scene depth (0.83..1.0)
  saturates the game's own CoC formula by ~50 m; hardware runs the SAME shader,
  constants and depth format yet stays legible at 40-60 m — **that compensating
  term is UNLOCATED and blocks any fix** (do not clamp CoC; name the term).
  `docs/part42-kickoff.md` is the LIVE hand-off; §6bu the full anatomy, including
  the retraction of an hour of arithmetic done on a memory record that turned out
  to be the previous frame's PICTURE (gotcha 311 — 280's second disguise).
* One certain sub-defect either way: the DoF gather reads depth AS 8_8_8_8 bytes
  and we serve the float image (part 36's counted-unclaimed 231-fetch class).

Where the port was, as of 2026-08-14 (part 42 — the flat-texture class is a
PROMOTION-DENIAL defect, and the DoF "hardware contradiction" mostly dissolved):

* **ITEM 00i IS CORNERED: a streaming PROMOTION-DENIAL decision, not a rate.**
  Verified pre-post-chain (the 003053 building is patternless in the SCENE
  surface); the flat class is a handful of shared world atlases stuck at 8×8/16×16
  (53 big draws from TWO addresses in one frame); hardware binds tiny-on-big
  **0 times across all eight R4 traces** where we do it in 44/82 walk frames; and
  **standing still for 2.3 minutes promotes NOTHING** — which refutes the rate
  class outright (file-IO latency, and the `KeSetBasePriorityThread` no-op that
  had been the named candidate since part 28 — do not build it for this item).
  **The engine then named it itself** (§6bv addendum): the flat class IS the
  per-zone `COMMON_TEXTURE.tex` vs `COMMON_TEXTURE_LOD.tex` choice — our runtime
  loads zones 1/2/3/7 as LOD at the spawn where hardware's street is fully
  textured. `ForceLODTexForStreamingWorld` refuted by a live read (0x82A57BD7=0).
  Next: the branch that picks the filename, then fix ITS INPUT (never the
  branch). **`docs/part43-kickoff.md` is the LIVE hand-off.**
* **ITEM 0u IS DOWNGRADED — the composite behaves as designed on BOTH platforms.**
  Our constants match hardware's to the digit (`CZ_VK_PSBIND_PC` — the
  instrument already existed); the worked alpha math gives ~95% blur at 50 m on
  both sides; the gather radius is ~0 on both sides (blur640 ≈ the half-res
  downsample); and **hardware's own R4 PNGs are soft at range** — part 41's
  "legible storefront" was signage contrast read against OUR flat walls, i.e.
  00i wearing 0u's clothes. Remaining residues: the fmt6 byte-split depth
  serving (edge weights only) and the gather's pc255.x (ours 0, hardware
  unrecoverable — loads from CPU-written `032B6000`, NOT a resolve destination;
  `xtr_draw_constants.py` now prints load provenance).
* **The cache gate paid again: 430 → 435.** Four dumped-but-never-built shaders
  (only the name-diff gate can see them) plus `vs_c8e86dffb37149dd`, which
  exposed and fixed a XenosRecomp emitter bug — a vfetch with an EMPTY
  destination mask emitted `r0. = ...;` and had been the bank's only
  translation failure. `docs/xenonrecomp-upstream-bugs.md`.

### (moved 2026-08-15, part 45 close-out) Where the port was, as of 2026-08-14 (part 43 — the zone texture-set decision is
fully named, OUR EXECUTION OF IT IS CORRECT, and item 00i waits on capture R5):

* **ITEM 00i INVERTED: the engine ITSELF picks LOD at the spawn, on inputs
  verified live.** The whole chain is named (`phase5-notes.md` §6bw):
  `sub_82270870` picks `COMMON_TEXTURE_LOD.tex` iff the zone's LOD-capable
  flag (rec+0x90C, from the two files' sizes at setup) AND every volume in
  the zone's list is farther than its threshold from the camera `[g+0x40]`
  (per-level boost tables; level 14 = none; force byte 0x82A57BD7 = 0).
  `CZ_ZONE_TEX_PROBE=1` printed every input at decision time and predicted
  part 42's narration line for line; `tools/zone_lod_live.py` reproduces the
  verdict on a live process. Zones 1/2/3/7 are all-far by 31-107 m at the
  spawn, the camera is ALREADY at the spawn when the burst runs, and the
  ordering hypothesis is dead (menu/origin cameras yield MORE LOD zones).
  **The LOD file IS the thumbnail set by design** (27 KB vs 1.3 MB). The
  decision runs ONCE per zone load; no promotion trigger found statically,
  and no headless roam has crossed a LOD threshold to test it live. **The R4
  traces cannot adjudicate this — they are a WARM-session sweep at Big Buck.**
  Capture **R5** is filed (one fresh DebugJump stand-still F4 at the spawn,
  no patching): flat street → 00i collapses to a state-comparison artifact;
  full street → suspects in order: volume skip-bits, volume data, an unfound
  reload path. **Build no fix before R5** (gotcha 5 — every candidate fakes
  the decision). (Its hand-off is superseded: `docs/part45-kickoff.md` is
  the LIVE one.)
* Instrument-log gotcha paid for: the probe's first version printed a
  directory object as %s, salted the log with NULs, and plain grep read the
  whole probe as absent for two runs — `grep -a` / `tr -d '\0'` before
  believing a zero from an instrument's own log (gotcha 25's self-made form).


## Archived status block — MID-PART-46 (superseded the same day by part 46's close-out)

Kept because the two items it lists were both answered AFTER it was written: the
UI text defect was fixed and operator-confirmed, and the tree fix settled on
`CZ_VK_A2M_MODE=1` rather than the per-sample dither this block proposes.

Where the port was MID-PART-46 (superseded by the block above — the operator's two items, both
taken as far as measurement goes: **THE TREE SHARDS ARE MISSING ALPHA-TO-MASK
COVERAGE** (demonstrated by an arm, three byte-identical null controls, and a
repro that now needs no operator), and **THE REPORTED PERFORMANCE REGRESSION IS
NOT REPRODUCED BY ANY OF THE THREE NAMED SUSPECTS**. `docs/part47-kickoff.md` is
the LIVE hand-off.):

* **ITEM 0t, THE TREES: the shards are missing ALPHA-TO-MASK coverage, and the
  remaining work is a named renderer change.** The canopy draws are alpha test
  GREATER at `RB_ALPHA_REF = 0.0` plus A2M over a **DXT4/5** albedo, i.e.
  fractional alpha. At ref 0 the alpha test keeps everything, so A2M is doing the
  whole cutout alone — and the renderer declined to emulate it on a written
  excuse ("the draws hardware sets it on also set the alpha test, so the clip
  covers them") that is false exactly at ref 0, so the leaf fringe the artist
  authored at alpha 0.05..0.5 was written at FULL opacity. An arm supplying the
  coverage removes the hard plates and lands the pre-named property on the
  oracle: canopy **p05/p95 0.291 -> 0.324 against hardware's 0.326**, with three
  byte-identical null controls (default cache, new emitter with the define off,
  define on with the gate declining). `phase5-notes.md` §6ca + addendum,
  gotchas 317-319.
* **AND THE REPRO IS THE TITLE SCREEN, whose hardware oracle was already in the
  repo.** The menu backdrop is the same material — 120 s,
  `CZ_FAKE_PRESS_SEQ=NONE,NONE,NONE,F9,NONE`, no input, near-static camera — and
  `Xenia logs/E_screenshots/E3_title_background_stillcreek.png` photographs that
  exact screen (content box x 0..1399, y 96..880 -> resize to 1280x720). Static
  also makes a `CZ_VK_DRAW_ID` map readable against another run's picture, which
  the moving outdoor route can never do.
* **The surface is 2x MSAA on BOTH machines, which is why the fix is not the
  obvious one.** Our counter reads msaa=1 on 69,390 A2M draws (4x on none), and
  `tools/xtr_draw_bindings.py` now carries `RB_SURFACE_INFO` so hardware can be
  asked: `w1_spawn` says msaa=1 on every leaf draw and all 240 A2M draws. We only
  sample-expand 4x surfaces, so `CZ_VK_A2M_ANY_SURFACE=1` (which dithers at pixel
  granularity) is a DIAGNOSTIC and not a default — it pushes the hard-edge share
  3.13% -> 4.92% against hardware's 0.21%. **The fix is to sample-expand 2x
  surfaces** (Xenos 2x is a vertical pair, so a 1x2 dither is exact and the
  existing resolve averages it); the shader half is built and controlled.
* **Refuted against hardware, do not re-buy**: the leaf render-state read
  (colour control, alpha ref, blend — byte-equal across all 19 round-2/3/4
  traces) and the `k_10_11_11` normal decode (hardware's own streams decode to
  unit length on **512 of 512** sampled vertices, 74.8% at N·L > 0). **Demoted**:
  the denormal/NaN packed-normal suspect. **Method worth keeping**: selecting the
  DEFECTIVE and the CORRECT pixels separately and reading both through the
  draw-ID map showed they were the SAME DRAW, retiring every per-draw input in
  one measurement (gotcha 318).
* **PERFORMANCE: all three named suspects are EXONERATED, and "no regression
  exists" is NOT what that measures.** Part 45's liveness fix: six alternated
  600 s outdoor runs, three an arm, one variable (`assets/shader_spv_pre45`) —
  **every draw bin inside its own measured noise floor**, with the two bins off
  the pacing floor disagreeing in sign (−2.9% and +4.3%); below 3,000 draws both
  arms sit at 32 ms with 94-99% pinned. Part 41's per-fetch samplers and part
  44/45's mip chain/tail, read on the `textures` phase share: **44.6 / 46.0 /
  44.2 against a baseline of 45.4**, all unmoved, every arm shown to have engaged
  by the counter the others carry. GPU quoted, not assumed: P5, 559 MHz, 34%,
  28.1 W. `phase5-notes.md` §6cb + addendum; `tools/part46_perf_ab.sh` and
  `tools/part46_perf_read.py`.
* **What the profiler did say: `textures` is 43-45% of the draw path** (~29-30%
  of the frame) on every arm — structural, not recent. The comparison against
  part 20's 13.6% is explicitly the weak half of that argument (different route
  era, binary and session; gotcha 13). **The next perf move is a measurement on
  the OPERATOR'S configuration** — windowed, their route — not a fourth headless
  arm; an operator report outranks a headless number.


## Archived status block — part 44

Where the port was, as of 2026-08-15 (part 44 — item 00i REOPENED same day as a
MIP-SELECTION OVERSHOOT; the part-43 menu reframe is retracted):

* **ITEM 00i, PART-44 STATE (superseded by part 45's root cause above for the
  menu half; outdoor half under re-measure): a mip-selection overshoot in the scene pass, ~2
  octaves too deep globally.** The "closed as faithful" verdict (now in
  `docs/port-history.md`'s status-block archive) was retracted the same day: the operator recreated the R4 walk and their
  matched capture shows the flat buildings bind FULL-SIZE textures (8
  tiny-on-big of 1,760 draws) — the thumbnail class was never the mechanism,
  and E5 (round-1 screenshot) proves hardware-FRESH is crisp at range. The
  elimination that followed (§6bx addenda 2–3): `CZ_VK_NO_MIPS=1` restores
  the buildings at the matched view (tail arm does not; the SCENE surface
  carries the flatness); the mip DATA is correct (guest chains AND uploaded
  staging verified level by level); every LOD input matches hardware (tfetch
  lod_bias 0 on 1,846/1,846; fetch-constant lod_bias 0 on 56,111/56,111
  bound constants in R4_04; filters/aniso equal, aniso engaged). The new
  `CZ_VK_MIP_TINT=1` paints each uploaded level a code color and shows L1 at
  two meters, L2 at ten, the spawn fence at L2 where texel math says 0.5.
  **`docs/part45-kickoff.md` step 0 is the hunt**: draw-ID the wall at the
  matched pose, compute hardware's implied LOD from R4_04's vertex streams,
  audit the scene pass's derivative environment (MSAA window-scale,
  §6bf/§6bh). The menu retraction and the level-machine reverse-engineering
  below STAND.



## The part-45 CLAUDE.md status block, archived at the part-47 close

Moved out of CLAUDE.md when part 47 closed, per the 2026-08-08 split's rule that
the file keeps only the live part and one part back. Unedited.

Where the port WAS, as of 2026-08-15 (part 45 — the white-surface / flat-prop class
SOLVED at its root: our own synth tool was dropping PS interpolants after PARTIAL
register writes; 217 of 333 pixel shaders sampled their diffuse at ONE TEXEL.
**`docs/part46-kickoff.md` is the LIVE hand-off and the operator has set part
46's order: TREES first, then the PERFORMANCE REGRESSION they report from the
last few days' fixes.**):

* **ITEM 00i's MENU HALF (and with it the flat-prop class) IS SOLVED, and the
  defect was OURS, in `tools/synth_shader_container.py`'s liveness.** The
  part-45 menu lab ran the fourth addendum's plan to completion: the GAS ball
  draw named by CZ_VK_DRAW_ID (draw 1606, vs_d338876a58c8c0ed /
  ps_eb170d16fe949e52); hardware's texture bytes for all four slots dumped
  out of B1 and **byte-identical to ours** (four md5 matches — the red disc
  was in our memory all along); UVs equal on every sampled vertex; every
  recoverable constant equal; dummies refuted by poison. The generated HLSL
  then showed `r0 = 0.0; r2 = 0.0; r3 = 0.0` where the microcode PARTIALLY
  writes those registers (`tfetch2D r0.__xy` keeps .xy!) before sampling the
  diffuse at r0.xy — the old analysis kept `written` as a flat register set.
  Per-component read-before-write (commit fdda6f3, encodings transcribed from
  XenosRecomp's own operand printer): **265 of 333 pixel shaders change, 217
  gain interpolants; the menu ball is RED; the E3 correlation gate flips
  +0.687 (fail) → +0.710 (pass).** Old cache kept at
  `assets/shader_spv_pre45` (CZ_SHADER_SPV = same-binary control arm). Full
  record `phase5-notes.md` §6by; gotcha 316; open-items 00i head updated.
* **THE OPERATOR'S A/B CLOSES THE FLAT-AT-RANGE CLASS TOO** (§6by addendum 2).
  Two launches, one binary, `CZ_SHADER_SPV` the only difference, their own
  route: fixed cache — *"the game looks way better now, the building doesn't
  seem to have issues… almost like OG game"*; pre-45 cache — *"way worse —
  gas station looks bad and building look FLAT DEPENDING ON DISTANCE."* That
  is item 00i's original complaint, toggled by the shader cache. **What drove
  parts 42-44 was substantially this defect.** Their one remaining complaint
  is the TREE CANOPY (hard black shards), which is PRE-EXISTING — it shows on
  part 44's captures too — and is named: `ps_69a5c3be9359b87c` /
  `ps_8602b5fd69289893` at `cc=AA00001C` (alpha test GREATER + ALPHA_TO_MASK,
  56 draws), i.e. part 41's parked A2M item, now top of the picture list.
* **A SECOND OPERATOR COMPLAINT IS SOLVED TO ITS MECHANISM: the UI TEXT LAYER
  IS STALE BECAUSE OF THE STREAM STORE'S GUARD** — item 00c (part 24's ammo
  counter) recurring above the 16 KB bound that fixed it. A matched operator
  A/B decides it: the STATUS screen's KEY ITEMS tab, same save, one env var
  apart — default renders the ATTRIBUTES tab's labels, and
  `CZ_VK_STREAM_GUARD_EXACT=1` renders `Still Creek Map / Zombrex / Shed Key`
  correctly (`phase5-notes.md` §6bz addendum 2). **The FIX is still owed and
  "always exact" is not it**: that arm reads 63.76 MB/frame in the guard
  against 9.28, so it is designed and measured together with the performance
  item below. Part 45's TEAR reading is RETRACTED.
* **(characterisation, still accurate) the UI TEXT LAYER symptoms** (open item 00k, `phase5-notes.md` §6bz + addendum). Colour changes
  MID-WORD, glyphs go missing, and the PREVIOUS screen's text persists (the
  SKILLS tab renders ATTRIBUTES' labels; pause-menu items stay painted over
  gameplay) while STATIC text in the same frames is perfect. Pre-existing, not
  from the fix. 32 captures in `part45-operator/ui_fixed/`. The text layer is
  ONE dynamic vertex buffer sub-allocated per run (§6ab), so one bad copy
  garbles every run. Eliminated: dropped draws (both bounds counters ZERO
  across 54.7M draws) and the ALU constant window. **The garbling VARIES frame
  to frame**, so the leading reading is a TORN buffer, not a frozen cache —
  arms `CZ_VK_FRAMES_IN_FLIGHT=1` then `CZ_VK_STREAM_GUARD_EXACT=1`.
  **The cheap headless check for this whole class, from the operator: press
  START at the title screen and a card appears for a second or two COMPLETELY
  EMPTY** (trim drawn, no glyphs) —
  `part45-operator/ui_fixed/capture_001343.ppm`.
* **PERFORMANCE: the operator reports a REGRESSION over the last few days'
  fixes** — unmeasured, and part 46's second item. Suspects in order of
  introduction: part 41's per-fetch samplers, part 44/45's mip uploads, and
  part 45's own fix (217 shaders gained interpolants).
  `assets/shader_spv_pre45` makes that last one a one-variable A/B. Measure
  with three runs an arm and MEDIANS plus the 16 ms-pinned share, never means
  (gotchas 237/238).
* **Every shading-side measurement through the old cache now has gotcha-172
  exposure, and the first re-measures ran the same day** (§6by addendum 1): the
  fixed-cache spawn gains the QUARANTINE bus lettering + van panel detail
  (part-26 white-prop members visibly fixed; the operator tour will finish
  the list), and **the mip-tint overshoot signature REPRODUCES on the clean
  bank** — a real, separate mechanism. Contact points at one fixed view:
  `CZ_VK_ANISO=0` is ~1 octave deeper (aniso engages and buys its octave),
  `CZ_VK_NO_FETCH_SAMPLERS=1` deeper still — neither arm shallows, so the
  sampler terms are exonerated and the residual +1..2-octave global shift has
  no named cause — **but it no longer has a visible symptom either**, so the
  first move on it is re-running part 44's decisive `CZ_VK_NO_MIPS=1` arm on
  the FIXED cache before quoting that result again. `docs/part46-kickoff.md`
  is the LIVE hand-off.

## Per-part status blocks moved out of CLAUDE.md in part 51 (parts 46-49)

Moved on 2026-08-16 under the 2026-08-08 rule that CLAUDE.md keeps only the live part
and one part back. It had six. The rule exists because that file is loaded into every
session whole, and the cost of breaking it is not size — it is that a reader cannot tell
which block is current (gotcha 13, the same defect as a stale LIVE pointer).

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


## Part 50 status block, moved out of CLAUDE.md at the close of part 52

Superseded by parts 51 and 52. Kept verbatim: its two headline findings — that
`CZ_VK_PROFILE` costs 2-4 ms a frame, and that this port uses 2.46 of 16 cores — are
still live facts, and its item-by-item repricing is the record of how the part-50 plan
was dismantled.

Where the port WAS, as of 2026-08-16 (part 50 CLOSED — **THE PLAN'S TOP TWO ITEMS WERE
BOTH REPRICED BY THE MEASUREMENT THAT PRECEDED THEM, AND ONE OF THEM WAS THE PROFILER
MEASURING ITSELF.** ~~`docs/part51-kickoff.md` is the LIVE hand-off~~ — superseded by
`part52-kickoff.md`; `docs/perf-plan-part50.md` is still the live plan, but **read
`phase5-notes.md` §6cg BEFORE it** — §6cg retires two of its items and corrects every
number in its budget):

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
* **WE ARE USING 2.46 OF 16 CORES — 15.4% of the machine — AND NOBODY HAD EVER ASKED.**
  Thirty parts of frame times and phase shares cannot answer "is this single-core?",
  because a per-phase profiler is written from inside ONE thread and cannot represent a
  core doing nothing (gotcha 338). Measured with `tools/part50_thread_cpu.py`: 37 threads,
  **twenty below 0.5%**, and two carrying 70% of the CPU — a **GUEST** thread at **93.2%**
  (the title simulating, nearly saturated, and not ours to optimise) and **our pump at
  79.0%**, whose stack is `Pm4_Execute -> ExecutePacket -> DoDraw`, i.e. **the PM4 walk and
  the Vulkan recording serialised on one core by construction**. Every item in the live
  plan makes that one core's work smaller and no item asks whether that is the right
  strategy. **Part 51's item 0 is now to find out whether the guest thread is the limiter
  — and whether it is WORKING or SPINNING, which this measurement cannot tell apart.**
  Also: `outside` is not all work — at 79% busy the pump is BLOCKED ~4.7 ms of a 22.3 ms
  frame, so the plan's "guest simulation ~3 ms" inside `outside` is our pump waiting.
* **Gates at close: ALL CLEAN**, E3 best of five **+0.8820**, 4 of 5 agreeing on layout.

---

## Part 51 status block, moved out of CLAUDE.md at the close of part 53

Where the port WAS, as of 2026-08-16 (part 51 CLOSED — **THE BUSIEST THREAD IN THE
PROCESS IS NOT THE GAME SIMULATING, IT IS THE GAME WAITING FOR US — and the largest item
in the plan is dead while the largest item in the frame was never in the plan.**
~~`docs/part52-kickoff.md` is the LIVE hand-off~~ — superseded by `part53-kickoff.md`;
read `phase5-notes.md` §6ch):

* **ITEM 0 IS ANSWERED AND IT INVERTS THE WORRY THAT OPENED THE PART.** Part 50 found a
  guest thread at 93.2% and correctly refused to guess whether it was WORKING or
  SPINNING. It is spinning: **84.15% of that thread is `sub_8283C6C8` under
  `sub_82845160`**, which `phase1-notes.md` **finding 38** identified in phase 1 as the
  **Draw Thread waiting on the ring read pointer** — and the only thing in this process
  that advances that pointer is our own pump. Part 50's table called it "the title
  simulating", which was wrong in the one direction that would have retired the whole
  plan. **Milliseconds off our pump are milliseconds off the frame.**
* **THE METHOD IS THE TRANSFERABLE HALF AND IT IS ONE COMMAND.** Thirty parts of
  per-phase profiling from inside the pump thread, against `perf record -F 999 -p <pid>`,
  which names the top cost in EVERY thread in symbols — the guest's included, because
  every guest function is a real symbol here. It disagrees with the phase profiler about
  our own pump: `DoDraw` is 9.84% of that thread and a CONTENT GUARD is 16.79%. Gotcha
  340; `tools/part51_thread_probe.sh`. Caveat: `perf` charges INLINED code to its
  container, and a 19.4% entry in the present path turned out to be an instrument.
* **THE PLAN'S BIGGEST ITEM (2a, soft-dirty page tracking) IS REFUTED — TWICE — AND
  NEITHER REFUTATION IS THE ONE IT WAS AIMED AT.** The query really is 1.6-4x cheaper
  than hashing the bytes, exactly as argued. But **arming `clear_refs` costs 24.4 ns per
  resident page = 7.5 ms/frame** on this runtime's 1.2 GB, against the ~0.7 ms of hashing
  it removes; and arming **write-protects every page**, so re-touching costs **+773
  ns/page** in minor faults charged to whoever writes next — **the GUEST's threads**.
  Strike the item. Gotcha 341: price the SETUP and the AFTERMATH before the part that
  made the idea attractive.
* **THE ITEM NOBODY HAD: THE PUMP SLEEPS 1 ms BEFORE EVERY RING WALK, AND SOMEBODY IS
  WAITING.** Absent from the plan because every item there makes the pump's WORK smaller
  and a sleep is not work. **The walk stops ~3.1 times a frame whatever the tick period
  is** — invariant across a 40x range — so the tick sets only how long each stop lasts.
  87-100% of those sleeps are immediately followed by a walk that finds real work (37-46%
  in the boot windows, which is the instrument's own negative control).
* **100 us IS THE NEW DEFAULT, and the POSITIVE CONTROL is what proves it.** Three arms,
  three unprofiled runs each: at **3,000-5,000 draws the frame goes 19 -> 16 ms, −15.8%,
  outside its own 5.3% floor**, with the 16 ms-PINNED share **24-36% -> 72-95%**. The
  4 ms control arm is **+34.8% to +56.2% and outside the floor in every bin**, i.e. the
  sleep converts to frame time at ~1:1 — which is what licenses the 2.7 ms in the bins
  where the direct comparison sits inside its noise. `CZ_PM4_TICK_MS=1` is the control
  arm. Costs stated rather than buried: process CPU **2.57 -> 2.75 cores of 16** and the
  pump's duty cycle **79% -> 93%**.
* **AND THE INSTRUMENT THAT RECORDS FRAME TIME COSTS ~3 ms A FRAME (12-20%).**
  `CZ_VK_FRAME_STATS` zeroes a 2 MB bitmap and walks all 921,600 pixels of every
  presented frame, and it was on in **every performance run this project has ever
  recorded** — the operator's lap and part 50's own profiler A/B included. Part 50 wrote
  the rule ("an instrument that can only be read through another cannot measure that
  one") and stopped one instrument short of applying it. **This is a CORRECTION, not a
  saving** (gotcha 337): with both instruments off, the operator's 28.3 ms at 5,000-7,000
  draws is really nearer **22-23 ms, ~43-45 fps**. A/Bs carrying it in both arms are
  unaffected.
* **A sub-millisecond tick broke a health warning on the way through**, on the one line
  whose own comment warns about exactly that: the ring trace's "sat on one wait for over
  a second" was 60 TICKS, which at 100 us means 6 ms, and a healthy run measures 104.
  Both the threshold and the label are now durations (gotchas 98/157).
* **THE OPERATOR HAS NOW JUDGED IT, and both halves are recorded as they came.** On
  "which felt smoother" they **could not tell the arms apart** — that rules out a GROSS
  pacing regression, which was the way this change could fail, and it is not written up
  as "confirmed smoother". On the profiler, the win reproduces on their machine and
  route: **3,000-5,000 draws 21 -> 17 ms with the vblank-pinned share 2% -> 51%**, and
  5,000-8,000 draws 26 -> 24 ms. The mechanism is visible directly — `outside`, the
  column the sleep lives in, reads **7.89 -> 5.37 ms, a 2.52 ms difference against the
  2.7 predicted**, while every other phase moves by a few percent. One run an arm, so
  the statistical weight remains the headless campaign's; this confirmed direction and
  mechanism on their hardware. §6ch §7.
* **Gates at close: ALL CLEAN**, and the two numbers that MOVED both moved for reasons
  the change predicts. `--smoke`; both PM4 capture oracles on B1 (24,527,474 packets, 0
  disagreeing; 0 indirect-buffer disagreements); the switch gate (0 defects); the shader
  dimension census (328 2D + 97 cube, 0 disagreements); `no translated shader` = 0;
  `truncated=0`; deepest file on a plain boot **#83 `cinezombie.big`**. **A5 exit 0 with
  4 permutation windows and 0 real** — part 50 recorded 3 windows, and a tick ten times
  finer re-interleaves the boot's scheduling by construction, which is what a permutation
  window IS. **E3 best of fourteen +0.8043, layout agreeing on every sample above the
  +0.70 threshold**, against part 50's +0.8820 best of five: the title backdrop is an
  ANIMATED scene, so a correlation is a fact about which moment was sampled (gotcha 133),
  and this change alters when frames land without touching what is drawn. Quoted as
  lower rather than tidied up, because a cross-session best-of on an animated reference
  is a weak comparison and should be read as one.

---

## Part 52 — the status block, moved out of CLAUDE.md at the close of part 54

Retired from CLAUDE.md's live status by part 54's block, per the 2026-08-08 split's rule
(keep the live part and one part back; move one out in the same commit that adds one).

Where the port WAS, as of 2026-08-17 (part 52 CLOSED — **FOUR ITEMS SHIPPED AND ALL FOUR
MAKE ONE THREAD SMALLER; NOT ONE LINE OF WORK MOVED ONTO ANOTHER CORE, WHICH IS PART 53'S
JOB. The plan's own verify arm refuted the plan's own fix, and the operator found the
place to measure from — a three-minute soak at 7,200-8,562 draws that is NOT at the frame
cap.** ~~`docs/part53-kickoff.md` is the LIVE hand-off~~ — superseded by
`part54-kickoff.md`; `perf-plan-part52.md` is still the live plan and its §9b records what
part 52 corrected in it; read `phase5-notes.md` §6ci, and **§6ci §5c before planning any
frame-time measurement**):

* **`BindShader` WAS 14.16% OF THE PUMP THREAD AND IS NOW 0.00% — not one sample.** It
  re-hashed the entire microcode on every shader-load packet, ~1,300-2,200 a frame, and
  `perf annotate` put **~71% of its samples on four `imulq`s**: the FNV-1a accumulator, a
  serial multiply per BYTE. The hash cannot be made faster because it IS the shader cache
  key, so it had to be avoided. Worth **~2.4 ms at 6,000-7,000 draws** (3 runs an arm plus
  a null arm: the control costs +15.0% mean / +12.5% median where the null read +0.2%),
  and that is a LOWER bound because the base arm is on a pacing rung there.
* **THE PLAN'S KEY FOR IT WAS WRONG, AND ONLY THE VERIFY ARM COULD HAVE SAID SO.** The
  plan specified `(va, size)` plus the microcode's first and last dword, and argued a
  wrong answer would read as a cache MISS. Measured: **two different shaders alternate
  with identical size and identical probe dwords** — microcode is far too regular for two
  dwords to identify it — and the wrong answer is **another real shader's hash**, which
  IS in the cache, so it would have bound a real, wrong shader silently past every gate.
  The shipped memo compares the whole content with `memcmp` (exact, and still ~30x
  cheaper, because the cost removed was ALU latency and not memory). **Gotcha 342: when a
  cache key is a PROBE, ask what the wrong answer IS, not how likely it is** — and write
  the verify arm even when the argument for the fix sounds complete.
* **THE COUNTER DUMP REPRICED ITEM 2.1 AND THE SOURCE HAD RANKED IT WRONG.** The plan says
  "28 `Count(` sites inside `DoDraw`". Of ~62.5 M plain-`Count` calls, **52,901,332 —
  84.6% — are one site in `VkRenderer_Draw`**, and ten sites are 99.2%; most of the 28
  fire a few hundred times an hour. `VkRenderer_DumpStats` already prints the statistic
  that ranks them. Third part running that a number the project already collects answered
  a question somebody was about to estimate.
* **`outside` IS NOT THE PUMP WAITING.** One `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` per
  report splits it into working and not-running-at-all: the pump is **BLOCKED 0.09-0.12 ms
  of a 16-17 ms frame**. That retires part 50's reading of the same residual as "guest
  simulation ~3 ms", and it means moving work OFF the pump (plan item 1.1) is still the
  right strategy — there is nothing to overlap with, only work to relocate.
* **The pipeline lookup is 110-112 -> 38-43 ns/draw** (~0.43 ms/frame at 6,000 draws), a
  `std::map` of ~400 entries replaced by a hash table with a one-entry front cache that
  serves **67.7-71.6%** of 6,613 lookups a frame. Measured against the PINNED pre-change
  binary run now, and **every other column of the `other` split is unchanged to the
  digit** — a stronger control than any arm, because drift would have moved them too.
* **AND THE THING THAT CHANGES HOW THE NEXT PART MEASURES: the headless outdoor route now
  sits on the frame cap for most of its length.** Both arms of an A/B land on the rung and
  the comparison reads zero whatever the change was worth. **That is an UNMEASURABLE
  result, not a null one**, and reporting it as "no change" is the same error as reading a
  mean off this title's vblank floor. `tools/part52_item_campaign.sh` raises `CZ_FPS_CAP`
  in EVERY arm; what it then reports is a CPU saving, not a frame rate anyone sees.
  `frame_perf_bins.py`'s `pinned%` is defined for a 16 ms ladder — at another cap it reads
  "on the second rung", so name the ladder when quoting it.
* **THE OPERATOR JUDGED IT, RAN THE CONTROL, AND THEN FOUND THE PLACE TO MEASURE FROM.**
  "Performance is better." A chained same-binary A/B (`ARM=ab
  tools/part52_operator_session.sh`) put **exactly one phase column in motion** —
  `outside`, where `BindShader` lives — with every other column inside ±0.21 ms, which is
  what makes one run an arm quotable: drift moves everything. Then **they proposed a
  three-minute SOAK in a heavier place, and it is the best measurement this project has
  ever taken**: one draw bin held **7,773 frames against 6,079** (a walk's best bin holds
  ~1,300), giving **+11.6% mean / +12.5% median at a significance of +211**, with the
  light bins still reading **+0.0%** as the experiment's own null. §6ci §§10-12.
* **THAT SOAK ANSWERS THE STRATEGY QUESTION AND REORDERS THE PLAN.** It sustains
  **7,162-7,529 draws with peaks to 8,562** for three minutes — heavier than any place
  measured here — and **0% of its frames sit on a pacing rung**, so it is CPU-bound and
  the remaining items buy FRAMES there rather than headroom. Uninstrumented it is
  ~16.7-18.7 ms (`CZ_VK_FRAME_STATS` printed its own bill at **3.21-3.23 ms/frame**
  there); the pump is **97.5-97.8% on CPU**. And what dominates it is **`record` at 8.69
  ms of 20.66 — 42%, twice the next phase** — which only item 1.4, parallel command
  recording, addresses. **Take every future A/B there, as a soak.**
* **A SAVING IS A SLOPE, NOT A NUMBER.** The memo's `outside` delta is **1.36 ms at ~5,500
  draws and 2.49 ms at ~7,200**, because it runs per shader-load packet and the soak
  measured **3,010-3,047 loads/frame** against 2,224 on the walk. Three measurements that
  looked inconsistent are one finding at three loads. **Quote the draw count with any
  per-draw or per-packet saving.**
* **AND THE THING PART 52 DID NOT DO, WHICH IS PART 53'S WHOLE JOB.** The plan's §1
  commits to strategy **(b) — move work onto cores that are doing nothing** — and says so
  in as many words. **All four shipped items are (a): they make ONE thread's work
  smaller.** That happened by following the plan's §10 ORDER, which front-loads three
  serial items ahead of the first parallel one; the prose and the table disagree and the
  prose is right. The process still uses **2.24 of 16 cores (14% of the machine) with ~13
  idle**, and at the operator's heaviest place our pump is **97.5-97.8% on CPU** — there
  is no slack left in (a) at that load. **Item 1.1 (parallel content guards) is part 53's
  job**, now measured at **~5.3 ms**. Note what "spread across all cores" can mean here
  before promising it: the PM4 walk is inherently SERIAL because the stream's meaning is
  positional (`pm4.h`), so the target is **3-5 busy threads, not 16**.
* **PRICING ITEM 1.4 MOVED 39% OF IT ONTO ITEM 1.1, and reconciled two instruments that
  had never been compared.** The stream content guard is charged to `record`, because
  `UploadStream` runs inside the `recordVertex` scope while `ProfScope(streams)` wraps
  only the copy — so item 1.1 (priced off the `GuardFold` SYMBOL) and item 1.4 (priced off
  the `record` PHASE) **shared the same milliseconds**. Split: `record 1,007 ns/draw =
  state 141 + vertex 188 + index 161 + GUARD 391 + residual 126`. Item 1.4's real ceiling
  is **~5.32 ms (~4.0 at four workers)**, not the 8.69 `record` implied; item 1.1 rises to
  **~5.3 ms**. Same size, and 1.1 is a pure function with a byte-exact oracle where
  recording owns renderer state and draw ORDER is semantic — **so 1.1 goes first**.
  Gotcha 343: a scope is a region of code, not a subsystem.
* **Gates at close: ALL CLEAN.** `--smoke`; both PM4 oracles on B1; the switch gate (0
  defects); the dimension census (0 disagreements); `no translated shader` = 0;
  `truncated=0`; deepest file **#83 `cinezombie.big`**; **A5 exit 0, 4 permutation
  windows, 0 real**; **E3 best of five +0.8771**, 4 of 5 agreeing on layout. And the
  shader-cache **NAME diff caught one again** — `ps_bd5d8eb053e36a84`, in the dumps, never
  in the cache, never bound by any run, so the miss counter read 0 and the count matched
  at 435 = 435 with different members. **The cache is 436.** Run the name diff in any part
  where `CZ_SHADER_DUMP` was set, which for a performance part is every part.
---

## Part 54 (2026-08-18) — the present was copying the frame three times, and one of those
## copies had never been measured

* **Opened by losing the recon to a race.** `CZ_FAKE_PRESS_SEQ`'s host debug edges fired
  only if the guest polled input inside a fixed 150 ms window; on a miss the DebugJump
  route silently degraded and the run profiled the prologue for seven minutes. Fixed (fire
  on the first poll after the interval is reached), with `CZ_FAKE_PRESS_EDGE_MISS=1` as the
  positive control for the recovery path. A one-run-a-side A/B blamed part 53's frame-cap
  change first and was refuted by three runs an arm. Gotcha 349.
* **`CZ_VK_SWAPCHAIN=1`** — the plan's §7 item, which the plan had filed under "explicitly
  not in this plan" with two reasons that were both wrong. Priced windowed BEFORE it was
  built, because `Host_PresentPixels` is a no-op with no window and `readback` had
  therefore read 0.0% on every headless run in the project's history (gotcha 352).
  −8.3% at 720p, −31.4% at 1440p, against a within-arm null of −0.7%/+0.0%.
* **The picture gate had to be new**, because every existing one walks the readback the
  change removes (gotcha 350) — and the first replacement, a compositor grab, read
  uniformly black because the monitor was asleep (gotcha 351). `CZ_VK_SWAPCHAIN_DUMP`
  correlates the image handed to the presentation engine against Xenia's own screenshot:
  +0.8831 at 1x, +0.8741 at 2x.
* **The headless route came off the pacing rung** — the pump is 93.7% of a core there,
  where part 53 closed at 50.3% — which retires §6ci §5c and makes headless A/B valid again.
* **The symbol budget re-taken**: `DoDraw` 24.43% and the NVIDIA driver 15.13% are the same
  item at 39.6% of the pump (item 1.4); `UploadStream` at 12.84% is in no plan at all.
* Shipped as an ARM, not the default, because a Vulkan window has no `SDL_Renderer` and so
  no host-rendered F4 overlay. The operator's verdict is what it is waiting for.


---

## Part 53's status block, moved out of CLAUDE.md at the close of part 55

Where the port WAS, as of 2026-08-18 (part 53 CLOSED — **THE FIRST WORK THIS PORT HAS
EVER MOVED OFF THE PUMP THREAD. Both content guards now fold on four workers a frame
ahead: `GuardFold` 25.87% -> 0.86% of the pump, the thread 63.4% -> 50.3% of a core, frame
time −12.5% against a null of +0.1%. And the first item whose BILL had to be measured as
carefully as its benefit — 33.2 points of core appeared on the workers where 13.1 left the
pump.** ~~`docs/part54-kickoff.md` is the LIVE hand-off~~ — superseded by
`part55-kickoff.md`; `perf-plan-part52.md` is still the
live plan and **its §9c records what part 53 established in it**; read `phase5-notes.md`
§6cj, and **§6ci §5c before planning any frame-time measurement**):

* **`GuardFold` WAS 25.87% OF THE PUMP THREAD AND IS NOW 0.86%.** It is the largest symbol
  there and the only large cost in this renderer that is PURE — guest bytes in, a
  `uint64_t` out, no Vulkan and no renderer state. Every guard the pump computes now files
  a job for the NEXT frame; the swap dispatches the list to four workers; the pump's
  existing cache lookup also yields the slot, so finding the answer costs nothing extra.
  **88-91% of guards are served by a finished pre-hash** (the plan's pre-registered floor
  was 80%) and **the pool never once blocked the pump**. A miss hashes inline exactly as
  before, so correctness never depends on the prediction.
* **THE FRAME IS 12.5-13.1% SHORTER ACROSS FOUR ADJACENT DRAW BANDS, AGAINST A NULL OF
  +0.1%.** Three runs an arm, alternated, `CZ_FPS_CAP=120` in every arm so neither lands
  on the rung (`tools/part53_item_campaign.sh`): 5,000-5,999 −13.0%, **6,000-6,999 −12.5%
  mean / −13.3% median at significance −34.9**, 7,000-7,999 −13.1%, 8,000-8,999 −13.0%,
  while the campaign's own null reads −0.5%, **+0.1%**, +1.6% in the same bands. Raw
  throughput needs no binning at all: **84,540 frames against 76,845 in the same wall
  time, +10.0%.** **In milliseconds it is a SLOPE** — 1.92 ms at 6,000-6,999, 2.41 at
  8,000-8,999 — because the guard runs per first-touch stream and per guarded texture.
  Quote the draw count.
* **AND THE PART THAT IS NEW TO THIS PROJECT: A (b) ITEM HAS A BILL, AND IT IS NOT SMALL.**
  Moving work onto idle cores RAISES total CPU: **2.53 -> 2.68 cores**, four workers at
  **8.3% of a core each**. The pump lost **13.1 points**; the workers gained **33.2**.
  Neither number is wrong — a memory-latency-bound loop is cheaper interleaved with other
  work than run alone, because on the pump its misses overlapped with register decode and
  driver calls. **So `perf` understates such a loop's isolated cost: a symbol share says
  what to MOVE, not what moving it will cost.** A second, smaller bill lands straight back
  on the thread being shortened — `recordState` 140 -> 183 ns/draw and `otherBegin`
  57 -> 81, neither of them code this change touched, ~0.4 ms/frame of the workers
  evicting the pump's working set from the shared L3. Gotcha 344.
* **THE RACE THIS WIDENS WAS MEASURED, NOT ARGUED.** The guard has read guest memory while
  the guest wrote it since the day it existed; pre-hashing moves that read up to a frame
  earlier, which admits a *coherent old* state where the inline hash would have seen new
  bytes — a one-frame stale mesh, the part-46 class. `CZ_VK_VERIFY_PARALLEL_GUARD=1`:
  **8-76 of ~3.2 M served guards per window, 0.0002-0.0021%**, and that is an UPPER bound
  on harm. Its poison arm reads **100.0000%**, which is what licenses reading the
  unpoisoned number at all (gotcha 30).
* **THE ONE SILENT FAILURE — a slot mix-up, one entry handed another's VALID hash — is
  impossible by construction.** Every result echoes back the pointer and length it was
  computed for; a disagreement is a loud line plus an inline fallback. **0 over every run
  of the part.** Same shape as part 52's memo defect, one subsystem over (gotchas 342,
  346).
* **ITEM 1.3 NEEDED NO WORKER AT ALL — the readback was making a redundant 3.5 MB copy per
  frame.** The staging buffer existed so the picture instruments could walk CACHED memory,
  on a buffer that has been HOST_CACHED since the readback fix. `readback` **5.5-7.3% of
  the frame -> 0.0%**, ~0.78 ms, and a matched-draw pair (6,253 vs 6,255) reads 13.0 vs
  13.9 ms. **The predicate is the MEMORY TYPE, not "is an instrument armed"** — gating on
  the instruments would have shipped a default path that no gate here ever runs, because
  every picture gate sets one of them (gotcha 345).
* **New tooling worth reaching for: `tools/part53_symbols.py`.** `perf report --tid=N` does
  NOT renormalise — it filters the rows and leaves each symbol's share of the WHOLE profile
  — so every hand reading of a per-thread profile in this project has had to correct for
  it, and getting it wrong turns 26% of the thread that IS the frame into 8% of a process
  nobody is optimising. This buckets by TID, folds `symbol+0xNNN` into functions, divides
  by the THREAD, and `--diff` prints two arms side by side.
* **Gates at close: ALL CLEAN, AND RE-RUN WHOLE after the resolution knob, the cap default
  change and `CZ_FPS_LOG` — at BOTH resolutions.** `--smoke`; both PM4 oracles on B1
  (24.5 M packets, 0 disagreeing); the switch gate (0 defects); the dimension census (0
  disagreements); `no translated shader` = 0 **at 1x and at 2x**; `truncated=0`; deepest
  file **#83 `cinezombie.big`**; **A5 exit 0 with 4 permutation windows, 0 real**;
  **0 `PARALLEL GUARD SLOT MIX-UP`**; **E3 at 1x best of five +0.8396 and E3 at
  `CZ_VK_RES=2560x1440` best of five +0.8562**, 4 of 5 agreeing on layout at each — the 2x
  row is what gives the resolution change an oracle that is not the operator's eye. The
  shader cache is **438**, both caches identical in membership.
  **E3 read +0.8808 earlier in the part and +0.8396 here on the same code path**, and the
  capture filenames say why: the cap moved 60 -> 500, so far more frames elapse in the
  gate's fixed 120 s and its five F9 presses land on different moments of an ANIMATED
  backdrop (frames 1,896-3,853 then 5,890-13,874). **A frame-index-addressed sample of a
  moving scene is re-aimed by anything that changes the frame rate** (gotcha 133).
* **THE OPERATOR CONFIRMED IT ON THEIR OWN SOAK, AND IT IS BIGGER THERE.** Two soaks in
  their heaviest place, both items switched together in the control arm: **7,000-7,999
  draws, 24.65 -> 20.33 ms, −17.5% mean / −16.7% median at significance −50.2**, with the
  light bins reading **+0.1%** and **+0.5%** as the experiment's own null. `record`'s GUARD
  **518 -> 13 ns/draw**, `textures` **428 -> 227**, **97.8% of guards served** by a
  finished pre-hash, **0 mix-ups**. And at 4,000-6,000 draws the share of frames pinned to
  the shipped 60 fps cap goes **55% -> 98%** and **29% -> 93%** — frames a player gets,
  not headroom.
* **AND THE READING TO CARRY FORWARD: THE PUMP DID NOT GET LESS BUSY, IT STAYED
  SATURATED.** ~94% of a core in BOTH arms on their machine. What changed is what each
  frame costs it — **23.6 -> 18.75 ms** — so the same pinned thread delivers **24% more
  frames**. Headlessly the pump had slack and the same item read as a thread doing less
  (63.4% -> 50.3%); on the real critical path it reads as throughput instead. Their bill:
  **2.64 -> 3.11 cores**, four workers at 11.1-11.2% of a core each.
* **ITEM 1.3 LOOKED LIKE IT HAD FAILED THERE AND HAD NOT — IT WAS A CONFOUND, AND THE
  RETRACTION IS THE MOST TRANSFERABLE THING IN THE PART.** Their `readback` read 0.58 ->
  0.66 ms, so it was filed as "did not survive"; **its own arm the same evening reads
  0.668 vs 1.135 ms, −0.467 ms, with NO OVERLAP across thirteen windows an arm** (guard
  pool held ON in both sides, `CZ_VK_PRESENT_STAGING` the only difference). Three
  configurations explain everything: pool OFF + 2 copies **0.56 ms**, pool ON + 2 copies
  **1.14**, pool ON + 1 copy **0.67** (and 0.66 in their arm A — agreement to 0.01 ms).
  **The guard pool DOUBLES the cost of the present copies** — four workers streaming
  ~70 MB/frame leave much less bandwidth for a 3.5 MB `memcpy` — and item 1.3 takes that
  back. Their A/B switched BOTH items together, so **an item read NEGATIVE because the
  other item in the same arm taxed it.** No number of repeats would have fixed that; a
  per-item arm did, in fifteen minutes. Gotcha 347. **Both items ship.**
* **AND THE OPERATOR THEN CORRECTED THE CORRECTION — the per-item arm sat at the military
  camp (1,891-4,777 draws) while their soak is 7,000-7,500.** Reading down its columns
  instead of taking its mean: `readback` goes **0.525 -> 0.700 ms with load inside one
  arm**, so **item 1.3's saving is a SLOPE too** — −0.37 ms at 1,900 draws, **−0.59 at
  4,200** — and the −0.467 mean is the mean of a slope over a range that excludes their
  load. The direction is what matters: **their load is above the whole range, so the item
  is worth MORE there than the arm can show.** The "pool DOUBLES the copies" table is
  downgraded to an inference (it compared 40 fps against ~100 fps); what survives is the
  within-arm slope and the pool's traffic at **2.2-3.0 GB/s** in every configuration
  measured. §6cj §12.
* **AND A NUMBER TO CARRY INTO EVERY REMAINING PARALLEL ITEM: the pool's bandwidth
  footprint is large enough to double an unrelated 3.5 MB copy.** Item 1.2 moves texture
  UNTILING, a pure bandwidth job, so it must be priced against the memory system and not
  only against the CPU it frees.
* **AND THE PORT GAINED TWO THINGS AT THE END OF THE PART THAT CHANGE WHAT A SESSION CAN
  ASK. First, `CZ_FPS_LOG=N` — a frame-rate line of one counter and one clock read per
  presented frame**, because every other frame-rate instrument here costs enough to change
  the answer (2-4 ms for `CZ_VK_PROFILE`, 1.9-3.3 for `CZ_VK_FRAME_STATS`), so the only
  uninstrumented configuration this port had produced no number at all. It measured the
  instrument bill directly for the first time: the operator's soak reads **49.8 fps
  instrumented and 69 clean — ~5.7 ms**. **Second, the shipped frame cap moved 60 -> 500**
  (a 1 ms vblank period), on their choice, because part 53 took their frame UNDER the 16 ms
  ceiling the old default imposed and it had started rounding them down to exactly 62.5 fps.
  **The lever is the PERIOD, not the ceiling** — 120 and 250 both still present a 14.44 ms
  frame at 16.0 ms. `CZ_FPS_CAP=60` is now the arm that restores the old default.
* **AND AN INTERNAL RESOLUTION KNOB — `CZ_VK_RES=2560x1440` — WHICH IS NEARLY FREE EXACTLY
  WHERE THE FRAME IS WORST.** The title still renders at 1280x720 in its own coordinates;
  what scales is the rasterisation target, under one invariant: **a surface whose pixels
  come from the RENDER PIPELINE scales, a surface whose pixels come from GUEST MEMORY does
  not.** Integer multiples only — a fraction in the tile scissors is a seam down the middle
  of a screen this title renders in two 640-wide halves, and no counter would report it.
  Operator-judged: *"perfect looks all good."* Measured: light zones **147 -> 96 fps
  (−35%)**, ordinary play −10..20%, and **the heavy end 69-71 -> 66, −4%** — because in a
  crowd we are CPU-bound and the GPU is idle 68% of every frame (gotcha 231). **It is the
  first change in this port that makes the GPU the limiter anywhere.** Its bill is the
  present readback at the scale SQUARED, **3.5 -> 14.1 MB/frame**, which promotes the
  plan's long-deferred swapchain item. §6cj §14.
* **THEIR PICTURE VERDICT: *"look and feel is as usual."*** That rules out a GROSS
  stale-buffer regression — the way this change could have failed badly — and is NOT
  evidence that the widened race never fires: the verify arm puts it at 0.0002-0.0021%,
  and one wrong frame in a 50 fps crowd is not something a human catches. The two
  instruments answer different questions and both were needed.



---

## Part 54's status block, moved out of CLAUDE.md at the close of part 56

Where the port is, as of 2026-08-18 (part 54 CLOSED — **THE PRESENT WAS COPYING THE FRAME
THREE TIMES AND ONE OF THOSE COPIES HAD NEVER BEEN MEASURED, BECAUSE IT ONLY EXISTS WITH A
WINDOW AND EVERY PERFORMANCE RUN HERE IS HEADLESS. A real Vulkan swapchain takes the frame
−8.3% at 720p and −31.4% at 1440p against a null of −0.7%/+0.0%. And the route to the
outdoor world turned out to be winning a 150 ms race.** `docs/part55-kickoff.md` is the
LIVE hand-off; `perf-plan-part52.md` is still the live plan; read `phase5-notes.md` §6ck):

* **`CZ_VK_SWAPCHAIN=1` — the plan's §7 item, promoted by part 53's resolution knob and
  PRICED BEFORE IT WAS BUILT.** The default present path copies the frame three times per
  present — the GPU's image→buffer copy, the pump's `memcpy` into the window's back buffer,
  and the window thread's `SDL_UpdateTexture` — a GPU→CPU→GPU round trip for pixels that
  never needed to leave the card. The arm acquires a swapchain image, blits the finished
  frame into it and presents on a **semaphore** rather than a fence, so the window gets the
  frame while the CPU is still retiring the previous one. `readback` reads **0.0%**.
* **`readback` HAD READ 0.0% ON EVERY HEADLESS RUN IN THIS PROJECT'S HISTORY** — including
  the ones part 53 used to declare its readback item done — because `Host_PresentPixels`
  returns immediately with no window. Windowed it is **8.1-8.7% of the frame at 1280x720
  and 16.4-36.2% at 2560x1440**, the largest single non-draw phase at 2x. The phase was not
  lying; it was reporting a path that was not running (gotcha 352). Two neighbours hid the
  same way: the window thread's own copy (8.8% → 15.0% of a core between the two scales, on
  a thread no instrument here reads) and the GPU's copy, visible only as `submit gpu` rising
  to 14.7% — **the GPU being a limiter at 2x, which is what the resolution knob was
  supposed to produce and had not been seen doing.**
* **THE FRAME IS −8.3% AT 720p AND −31.4% AT 1440p, AND −29.0% AT 1440p INTO THE
  OPERATOR'S OWN MAXIMISED 2560x1417 WINDOW — ALL AT ~2,700 DRAWS, WHICH IS THE LIGHT END.
  READ THE RETRACTION TWO BULLETS DOWN BEFORE QUOTING ANY OF THESE.** Three rounds an arm, alternated, one frozen
  binary, medians per draw band (2,500-2,999 draws, n=7-9 an arm), against the campaign's
  own within-arm null of **−0.7%, +0.0% and +0.0%**. At the maximised window it is −45.1%
  at 500-999 draws, **−29.0% at 2,500-2,999**, −23.3% at 3,500-3,999 and −21.4% at
  4,500-4,999.
* **THE WINDOW-SIZE WORRY WAS REAL, WAS RECORDED AS OPEN, AND WAS THEN REFUTED BY THE
  CAMPAIGN IT ASKED FOR.** The first campaign measured a 1088x612 drawable while the
  operator plays maximised, and the two arms genuinely do scale differently with the window
  — the readback path's CPU cost is a function of the INTERNAL resolution and is
  independent of it, while the swapchain's blit DESTINATION *is* the window. Measured as a
  2x2: **the window costs the readback arm +2.7…+4.9% and the swapchain arm +2.6…+8.6%.**
  The mechanism was right and the magnitude was small. Filing it as OPEN rather than as
  "probably fine" was still correct — only the campaign could separate the two. Gotcha 353
  stands: **a present-path measurement has TWO resolutions and naming only one is naming
  none**, and both arms now log their drawable size for that reason.
* **AND THEN THE OPERATOR'S SOAK A/B RETRACTED THAT HEADLINE'S GENERALITY. EVERY CAMPAIGN
  IN THIS PART SAMPLED THE LIGHT END OF THE GAME.** Both arms in one session, their
  machine, uninstrumented but for `CZ_FPS_LOG`, matched on the draw count that line now
  carries: **2,250-2,499 draws 90.4 -> 114.7 fps (−2.33 ms, −21.1%)** but **6,750-6,999
  draws 67.8 -> 70.2 fps (−0.51 ms, −3.5%)**. Their own report first, before seeing any of
  it: *"3 to 6 fps higher"* — measured **+1.5 to +2.4**, so they were right and slightly
  generous. **In MILLISECONDS the saving is not constant after all**: this file and
  §6/§9 said the percentage falls with load while the millisecond figure holds, and it
  collapses too, 2.33 ms -> 0.51 ms. The best-populated band in BOTH campaigns was
  2,500-2,999 draws and **the operator plays at 6,700-7,300**; their light band agrees with
  the campaigns, so nothing was mis-measured — it was over-generalised. Likely mechanism,
  as a hypothesis: at high load the GPU is busy, so CPU time taken off the pump is absorbed
  by a longer fence wait rather than converted to frames. **Third part in three where a
  soak in the heaviest place answered a question a roaming campaign could not** — gotcha
  355.
* **THE STUTTER THEY REPORTED IS REAL AND IS A SEPARATE WIN: it is the present MODE.**
  Frame-time mean against median IN TRANSIT is **+3.3% for MAILBOX against +5.9% for the
  compositor-paced SDL present**; at the settled soak both are smooth. MAILBOX is a choice
  the SDL path could never make (part 49's 60->30 snap), and this half survives the
  retraction above intact.
* **AND IT IS A SLOPE THE OTHER WAY ROUND FROM EVERY PREVIOUS ITEM.** Parts 52 and 53
  shipped savings that grew with the draw count, because their work ran per draw or per
  packet. This is a FIXED cost per frame, so its share is largest where the frame is
  otherwise lightest: at 2x, **−50.0% at 500-999 draws and −17.8% at 5,000-5,499**. Quote
  the draw count, and expect the trend to run the other way. It also removes **more than
  the phase it zeroes** — zeroing a 24.5% `readback` out of 10.20 ms predicts 7.70 and the
  measurement is 7.00 — the extra being the GPU copy and the pump waiting on the window
  thread's mutex.
* **THE OPERATOR FOUND A REAL DEFECT IN TEN MINUTES THAT NO GATE HERE COULD HAVE: the
  swapchain did not follow the window.** It rebuilt only on `VK_ERROR_OUT_OF_DATE_KHR` and
  merely COUNTED `VK_SUBOPTIMAL_KHR`, so a window enlarged after the first present kept
  being presented from the original 1280x720 swapchain and the compositor stretched it to
  a 1440p monitor — *"it is blurry"*, and it was. **SDL's `SDL_RenderCopy` had always
  scaled the full-size texture into whatever the window currently was, so resizing needed
  no code of ours and appears nowhere in it**: replacing a library's present means
  inheriting jobs it was doing invisibly (gotcha 354). The fix asks the drawable SIZE every
  frame rather than trusting a return code; `CZ_WINDOW_RESIZE_AT=SECS:WxH` is the positive
  control, because **no headless gate can resize a window**, and it proved the rebuild in
  both directions. Their verdict after it: *"Looks a lot nicer now."*
* **IT IS THE DEFAULT AS OF THE CLOSE OF PART 54, ON THE OPERATOR'S DECISION, AND NOTHING
  WAS LOST WITH IT.** `CZ_VK_NO_SWAPCHAIN=1` is the control arm and restores the readback
  path exactly — it is what every measurement before part 54 was taken on, so it is the arm
  future present claims are compared against and not a deprecated path. **The one blocker
  was that a window carrying `SDL_WINDOW_VULKAN` has no `SDL_Renderer` and so could not
  draw the host-rendered F4 debug menu; that was PORTED rather than accepted.** The layout
  is emitted once (`EmitDebugOverlay`) and consumed by two backends — SDL rects, and a
  software rasteriser the renderer uploads and blits over the presented image — because
  writing it twice is how two drawings of one menu drift apart until an operator reports
  that it "looks different in the other mode". The only deliberate difference is an opaque
  panel where SDL's is 88%, since a blit cannot blend.
* **AND THE OVERLAY TOOK FOUR DEFECTS TO GET RIGHT, THREE OF THEM FOUND BY THE OPERATOR
  LOOKING AT THE SCREEN AND NONE BY A COUNTER.** (i) It was first placed AFTER the
  `CZ_VK_SWAPCHAIN_DUMP` copy, so the dump could not show it and — silently — the dump had
  already moved the image to `TRANSFER_SRC`, so the overlay blitted into an image whose
  layout said otherwise: undefined behaviour reachable only with the dump armed, i.e. only
  in the gate. (ii) It was rasterised FULL SCREEN with transparent margins and copied
  whole, and **a blit is a copy, not a blend — so the entire game was black around the
  panel** while `swap: debug overlay drawn` read 5,595 and my own dump check happily
  confirmed 6,466 panel-coloured samples. **The check looked for the thing I had built and
  never asked whether anything ELSE was still there** (gotcha 356). (iii) The panel then
  came out SOLID where SDL's is 225/255; a copy cannot blend, so the blend is now done on
  the CPU against the frame behind the panel, captured one frame earlier — staleness that
  is unobservable behind a menu, against a graphics pipeline that is a lot of machinery for
  one. (iv) It uploaded through `R->staging`, the SHARED staging buffer, at offset zero,
  **where every texture upload also writes and whose copy executes later** — so the overlay
  and textures overwrote each other's bytes (gotcha 357). Operator-confirmed fixed.
* **NONE OF THIS PROJECT'S PICTURE GATES CAN SEE THIS ARM, AND THAT IS THE MOST
  TRANSFERABLE THING IN THE PART.** `CZ_CAPTURE_KEY`, `CZ_VK_FRAME_DUMP`,
  `CZ_VK_FRAME_STATS` and the E3 correlation all walk the present READBACK — the exact copy
  the swapchain removes — so they pass **with the old path still doing all the work**.
  Gotcha 350: **before gating a change, ask which BYTES the gate reads and whether the
  change produced them.** The replacement, `CZ_VK_SWAPCHAIN_DUMP`, reads back the image
  actually handed to the presentation engine and correlates it against Xenia's own
  screenshot: **+0.8831 at 1x (21 of 38 agreeing on layout) and +0.8741 at 2x (16 of 32)**,
  against the E3 gate's standing +0.8396…+0.8808. Vulkan validation clean.
* **THE FIRST ORACLE TRIED WAS A COMPOSITOR GRAB AND IT READ UNIFORMLY BLACK, BECAUSE THE
  MONITOR WAS ASLEEP.** Five 2560x1440 PNGs with RGB extrema `(0,0)` on every channel in
  both arms, `+0.0000` against every orientation, and nothing wrong with the renderer, the
  grabber or the gate. Gotcha 231's trap one subsystem over. `tools/part54_swapchain_picture.sh`
  is still the better gate **by day**; use `CZ_VK_SWAPCHAIN_DUMP` at night. (KWin does not
  implement the `wlr-screencopy` protocol `grim` needs and says so clearly; `spectacle -b -n
  -f` goes through KWin's own interface.)
* **THE ROUTE TO THE OUTDOOR WORLD WAS A COIN FLIP AND IS FIXED.** A synthetic host debug
  edge (`F2`/`F3`/`F4`/`F9`) fired only if the guest polled `XamInputGetState` inside a
  **150 ms window at a fixed wall-clock offset**. On a miss the edge was LOST, the DebugJump
  screen was never requested, and the recipe silently degraded to "press START a lot" while
  the run continued for its full seven minutes and profiled the prologue. It had been
  winning that race since part 39. The edge now fires on the first poll after its interval
  is reached; **`CZ_FAKE_PRESS_EDGE_MISS=1` is the positive control** that forces the
  recovery path, which no ordinary run exercises. **The diagnosis went wrong first in the
  standard way**: one run an arm blamed part 53's frame-cap default change, and three runs
  an arm read 3/3 and 3/3 six minutes later. Gotcha 349.
* **THE HEADLESS ROUTE IS OFF THE PACING RUNG AND THE PUMP IS SATURATED THERE — 93.7% of a
  core, where part 53 closed at 50.3%.** The cap default moving 60 → 500 did that, and it
  **retires §6ci §5c's warning** that a headless A/B here reads zero whatever the change was
  worth. The process uses **3.75 of 16 cores**, up from 2.68.
* **The re-taken symbol budget, and the biggest thing left is the draw path.** Pump thread,
  instruments off, outdoors in a crowd: `DoDraw` **24.43%**, the NVIDIA driver
  **15.13%**, `UploadStream` **12.84%**, `WriteRegisterRun` 9.10%, `UploadTexture` 8.69%,
  `ExecutePacket` 5.77%. `GuardFold` does not appear. **`DoDraw` plus the driver is 39.6%
  and they are the same item** — the driver time IS the `vkCmd*` calls the recording makes,
  so plan item 1.4 (parallel command recording) moves both, and it is still the riskiest
  thing in the plan. **`UploadStream` at 12.84% is not in the plan at all**: part 22 closed
  the stream cache on `ProfScope(streams)` reading 0.0%, and the symbol says that scope is
  not where the cost is (gotcha 343's shape). Splitting it is the cheapest unexplored item.
* **Gates at close: ALL CLEAN, re-run whole after the default flip and the overlay work.**
  `--smoke`; switch gate 0 defects; dimension census 0 disagreements; both PM4 oracles on
  B1; E3 best of five **+0.8415**, 4 of 5 agreeing on layout; `no translated shader` 0;
  `truncated=0`; 0 `PARALLEL GUARD SLOT MIX-UP`; deepest file **#83 `cinezombie.big`**;
  **A5 exit 0, 4 permutation windows, 0 real**; plus the two rows this part had to invent,
  the swapchain image against E3 at **1x (+0.8831)** and **2x (+0.8741)**.
  **And the shader-cache NAME diff caught one for the FIFTH part running** —
  `vs_d34782b5c8ddf527`, dumped by the operator's own play session and never in the cache,
  which no miss counter reported because no run had bound it. **The cache is 439**, both
  caches rebuilt and identical in membership.



## Part 55 status block (moved from CLAUDE.md by part 57, per the two-block rule)

Where the port is, as of 2026-08-18 (part 55 CLOSED — **THE OPERATOR ASKED FOR
MULTITHREADING AND THE BIGGEST THING ON THE CRITICAL THREAD TURNED OUT NOT TO BE
PARALLELISABLE WORK AT ALL: a quarter of the pump thread was `std::unordered_map` and
`std::map`. Deleting it took their frame −13%; a per-window memo on the ALU constant copy
took another −2 to −3%; −18% in total at the load they actually play, with none of the
plan's three parallel items built.** `docs/part56-kickoff.md` is the LIVE hand-off and its
subject is VISUAL BUGS; **performance is parked in `docs/perf-state-parked.md`**; read
`phase5-notes.md` §6cl):

* **THE OPERATOR SET THE SUBJECT AND THE CONSTRAINT.** *"I want us to focus on making it so
  the game properly use multithread and dispose of the load properly"*, plus *"even if we
  really needed the 16 core we should still leave core empty for user background item and
  all. So we should do it smart and depend on amount of core the user has instead of aiming
  for my machine."* Both were answered: the ceiling is 5-6 busy threads and not 16, and
  there is now **one `ThreadBudget` for the whole runtime** sized from PHYSICAL cores
  (counted as distinct (package, core) pairs, intersected with the process's affinity mask),
  minus 2 reserved for the OS and the player, minus 3 already committed, capped at 6.
  **Zero is a first-class configuration** — a 4-core laptop runs the serial path, which is
  correct and not degraded. `CZ_WORKERS=N` is the one knob, and every run prints
  `[threads] machine: 8 physical cores, 16 logical cpus -> budget 3 workers`. Gotchas 358,
  359. **The arithmetic correction matters on its own: every "N of 16 cores" figure written
  here since part 50 counted logical THREADS**, so the process at 3.75 cores was using 47%
  of that machine and not 23%.
* **ITEM C — "split `UploadStream` before assuming anything about it" — WAS FILED AS THE
  CHEAPEST THING ON THE LIST AND IT REWROTE THE PART.** It could not be split with a
  `ProfScope`: the path is taken ~33,000 times a crowd frame and two clock reads at ~20 ns
  is 1.3 ms a frame, larger than several of the phases it would separate (gotcha 7). So it
  was split with `perf` and the DWARF line table the RelWithDebInfo build already carries —
  `tools/part55_srcline.py`, free, and at -O2 it attributes INLINED callees to their own
  lines. **89% of `UploadStream` was the hash-map lookup: 41.24% `std::equal_to`, 36.92%
  `_M_find_before_node`, 10.49% the prime MODULO — 13.1% of the whole pump thread.** The
  same tool then found 17.9% of `DoDraw` in a `std::map` (the shader table, probed twice a
  draw) and ~76% of `UploadTexture` in three more. **Roughly a quarter of the pump thread
  was container lookups**, larger than any item in the plan. Gotchas 360, 361, 362.
* **FIVE TABLES ARE NOW FLAT OPEN-ADDRESSED**, with two details that would have been silent
  defects: the key is passed through a splitmix64 finalizer (with a power-of-two mask the
  low bits ARE the bucket, and these keys' low bits are an endian code and the bottom of a
  byte count), and clearing is a GENERATION BUMP rather than a memset, with the tombstone
  flag in the top bit of the same word so a probe still reads one array.
  **`CZ_VK_NO_FLAT_CACHE=1` restores the maps exactly**; `CZ_VK_VERIFY_FLAT_CACHE=1` runs
  both and compares every lookup — **0 of 48.5 M disagreed** — and its poison arm reads
  **81.7%**, which is what licenses reading the zero at all (gotcha 30). `_int_malloc` went
  from 2.11% of the pump to **0.02%**.
* **THE ALU CONSTANT COPY IS MEMOISED PER WINDOW, AND THE FIRST DESIGN WAS REFUTED BY ITS
  OWN PRE-REGISTERED KILL THRESHOLD.** It copies 8 KB per draw — ~57 MB/frame and over
  5 GB/s at the operator's soak, and the two hottest source LINES on the pump thread once
  the containers stopped hiding them. Predicted ~60% reuse from the packet census
  (`DRAW_INDX` 2,353/frame vs `LOAD_ALU_CONSTANT` 890/frame), "below 30% not worth its
  risk" written down first, **measured 3.6-7.1%** — because constants are also written by
  ordinary type-0 REGISTER RUNS, 40.1% of all packets, which that opcode census cannot see.
  **Splitting the stamp into the two constant windows takes it to 32-36%**, on the
  hypothesis that the VERTEX window is rewritten per draw (a world matrix per object) while
  the PIXEL window sits still — confirmed by the per-half counter on the operator's own
  machine: **VS 2.9%, PS 61.0%**. Gotcha 365: a hypothesis that splits a cost needs a
  per-part counter or the explanation is a story. Verifier **0 of 117,521 and 0 of 119,019**,
  poison **100.0000%**.
* **MEASURED ON THE OPERATOR'S OWN SOAKS, FOUR CHAINED TWO-ARM SESSIONS, AT ~7,000 DRAWS IN
  THE HEAVIEST PLACE THEY KNOW: ~12.8 ms -> ~11.0 (flat containers, −13%) -> ~10.5 (constant
  memo, −2 to −3%) — about −18%, 80 fps -> 95.** Their framing is why the roaming campaign
  was abandoned mid-run: *"I'll do your campaign with soak at the spot that hit the cpu the
  most so we just get 2x 3minutes soak instead of hours of testing in unstable environment
  with autochuck."* `tools/part55_chained_ab.sh` is that harness; arms are one env var each.
* **AND THE HEADLINE HAD TO BE RETRACTED WITHIN THE HOUR, BY THE PERSON WHO WROTE THE RULE.**
  Mid-soak, arm 1 was compared against the PREVIOUS session's control and reported as
  −5.5%; arm 2 of the same session said −2 to −3%, which is what had been predicted before
  the run. **Gotcha 364: an arm's number is meaningless until its OWN control arm has run**,
  and the pull is strongest when the older number is recent, from the same harness and at a
  similar draw count — it looks like a control and it is not one. **Every session also
  landed its two soaks 4-7% apart in draw count**, which moved a headline in both directions
  across the part; project with each arm's own within-arm slope and quote both projections.
* **THE OPERATOR'S "SHOULDN'T GEOMETRY BE IN VRAM" IS A REAL GAP AND THE ANSWER IS NO —
  ~14% SLOWER, and the reason is structural to recompilers.** Images were already
  `DEVICE_LOCAL`; the 512 MB stream store and the per-frame arena were not, so every draw
  fetched vertices across PCIe. Their RTX 3070 exposes `DEVICE_LOCAL | HOST_VISIBLE |
  HOST_COHERENT` over all 8 GiB (Resizable BAR), so the switch is one line — and it took the
  frame from 11.18 to 12.80 ms draw-matched. **A game engine uploads a mesh once and draws
  it a hundred times, so the GPU's fetch dominates; a RECOMPILER re-uploads its shader
  constants every draw because the guest writes its register file every draw** — ~85 MB/frame
  of CPU writes, cached at 30+ GB/s in RAM against write-combined PCIe at a third of that.
  Gotcha 363, plus a section in `reusability.md` written for whoever ports Case West.
  `CZ_VK_VRAM_STREAMS=1` is kept as the arm. **Two corollaries: the calculus FLIPS once the
  per-draw constant upload is gone, and CPU-visible device memory is WRITE-ONLY** — a read
  is an uncached fetch across the bus, and this port found a function at 5.31% of its
  critical thread reading three vertices back out of the buffer, free in RAM and ruinous in
  VRAM with nothing naming the cause.
* **A STUTTER THE OPERATOR REPORTED WAS INVESTIGATED PROPERLY RATHER THAN ATTRIBUTED.**
  Their first session: the flat arm *"is stuttering a lot"* while moving, and its log lost
  ~7 seconds inside one 10-second window. The tables were COUNTED (20 grows, 31.41 ms total
  over a whole run, worst 15.04 ms — three visible hitches, not seven seconds), the headless
  campaigns were asked and DISAGREED with the report (18 frames over 50 ms in the flat arm
  against 39 in the map arm), and the arm ORDER was reversed, which is the control the first
  session could not have. On the re-test every log's worst mean-over-median window is
  1.45-1.47x on a 2 ms menu frame, and their verdict was *"arm 2 has same amount of stutter
  as arm 1 but framerate is higher"*. **The tables were A stutter source and not THE one**;
  pre-sizing them to the measured high-water mark takes the run's grow bill to 1 grow /
  0.06 ms. Two things changed between the sessions, so the credit is NOT awarded cleanly.
* **Gates at close: ALL CLEAN, re-run whole.** `--smoke`; switch gate 0 defects; dimension
  census 0 disagreements; both PM4 oracles on B1; **E3 best of five +0.8763, 4 of 5 agreeing
  on layout**; `no translated shader` 0; `truncated=0`; **A5 exit 0, 4 permutation windows,
  0 real**; deepest file **#83 `cinezombie.big`**; shader-cache NAME diff shows only
  `ps_926c15dd20571cf1`, the known lost-microcode entry — **the cache is 439**, unchanged.
  **Plus the two rows this part had to invent**: the flat-cache verifier (0 of 48.5 M,
  poison 81.7%) and the constant-memo verifier (0 of 117,521 and 0 of 119,019, poison
  100.0000%).



## Part 56 status block (moved from CLAUDE.md by part 58, per the two-block rule)

Where the port is, as of 2026-08-19 (part 56 CLOSED — **A PICTURE SESSION DRIVEN END TO END
BY OPERATOR CAPTURES. It implemented the STENCIL TEST, which this renderer had never honoured
on ~18% of a gameplay frame's draws, and REFUTED TWO of its own hypotheses about the other
defects. Three of the four reported defects remain open and one is now formally
unexplained.** `docs/part57-kickoff.md` is the LIVE hand-off; performance stays parked in
`perf-state-parked.md`; the evidence is `open-items.md` 00o):

* **THE STENCIL TEST IS IMPLEMENTED AND IT WAS ENTIRELY ABSENT.** `stencilTestEnable`
  appeared ZERO times in this renderer while RB_DEPTHCONTROL bit 0 is `stencil_enable` —
  our own comment said so and the code read bits 1, 2 and 4..6 and stepped over it.
  Censused on the operator's own frames: **350 of 1980 and 326 of 1823 draws enable it.**
  Their report identified the consequence — cutting a zombie in half gave *"two full zombie
  and the blood is a square"* — and the mechanism: the game draws the WHOLE body twice and
  masks each copy, so nothing masking it yields both whole bodies AND the unclipped
  cross-section cap, two symptoms from one cause. Verdict on the fix: *"isn't as bad but far
  from perfect"*, with captures showing a correctly shaped lower half and a correct
  decapitation. **What is still wrong is ASKED AND UNANSWERED** — do not guess it from the
  images, three were examined and the defect could not be seen in them.
* **BOTH REGISTER LAYOUTS WERE CONFIRMED BY COHERENCE, NOT FROM A DOCUMENT.** Decoded with
  the assumed bit layout the title's stencil states come out as a matched PAIR —
  `ALWAYS/KEEP/REPLACE` writing reference 254 through `sr=00FFFFFE`, and `EQUAL/KEEP/KEEP`
  painting only where 254 was written. A wrong layout produces random ops, not a mask-write
  beside a mask-test. Same method located the polygon offset registers, and it is the method
  this port had to learn twice (gotcha: RB_COLORCONTROL is 0x2202, and Fable 2 reads the
  offset enables from 0x2205, which is RB_BLENDCONTROL1 here).
* **VULKAN VALIDATION CAUGHT THREE DEFECTS THAT NO GATE, NO COUNTER AND NO PICTURE COULD**,
  and the first is the one to remember: **`dsi.dynamicStateCount` was hardcoded to 3 while
  the array had grown to 4**, so the polygon offset committed earlier NEVER DECLARED its
  dynamic state and was working only on driver leniency — inert, with every gate passing.
  The other two: calling a dynamic-state setter for state a pipeline specifies STATICALLY is
  illegal (VUID-vkCmdDraw-None-08608, and its message says so plainly once READ rather than
  guessed at — two rounds were spent fixing the opposite), and a skip-if-unchanged cache does
  not survive a pipeline bind because binding a pipeline with static state makes the matching
  dynamic state undefined. Validation on the outdoor route is now **6 `topology-08773` and
  nothing else**, against a standing baseline of 20+6. **Gotchas 366, 367, 368.**
* **REFUTED 1 — Z-FIGHTING.** The decal flicker's camera-motion dependence is the classic
  signature, and the polygon offset was a genuine gap in BOTH this port and Fable 2's. The
  census found it real and reaching exactly the right draws (178+48 single-quad alpha-blended
  draws, i.e. decals). **It fixed nothing.** The positive control settled it:
  `CZ_VK_POLY_OFFSET_SCALE=10000` visibly lifts decals in front of walls and zombies, and the
  operator's verdict there was *"when they are not floating they flicker"*. A ten-thousand-fold
  depth bias does not stop it. The offset is KEPT as correct emulation and fixes nothing
  reported.
* **REFUTED 2 — THE MIP CHAIN.** The unifying story was that low mip levels decode wrong
  (colour for the sign, alpha for the bunting) and that mip SELECTION oscillating under
  camera motion explained the decal flicker while a still camera pinned it. It fits every
  symptom and it is wrong: `CZ_VK_NO_MIPS=1` **engaged** (mip-rejection lines 8 -> 0) and the
  operator's verdict was *"gas is still black and everything still is like before"*. The
  plumbing was checked afterwards and is correct. **A hypothesis that fits every symptom
  should be tested first and advocated second.**
* **THE DECAL FLICKER IS NOW FORMALLY UNEXPLAINED**, with its constraints written down: only
  under camera motion, stable in BOTH states when still, not depth, not mip data. **The
  instrument gap that stopped the analysis is that F8's burst carries no PER-DRAW CENSUS**,
  so a decal seen blinking could not be asked whether its draw was issued. That is part 57's
  first build.
* **THE DISTANCE DEFECTS HAVE THE OPERATOR'S OWN STEER AND A NEW FACT.** Their steer: the
  sign/canopy class is *"similar to our last issue that was like that"* — item 00i, which
  part 45 traced to our translation dropping PS interpolants so 217 shaders sampled diffuse
  at ONE TEXEL (a single texel is a flat colour; a dark one is BLACK, which is the symptom
  exactly). And the captures establish that **17 pixel shaders appear only in the far frame
  and 9 only in the near one — this title swaps pixel shaders BY DISTANCE**, so the far view
  is different code rather than the same shader minified.
* **NEW INSTRUMENTS**: **`F8` records every presented frame for a second**
  (`CZ_BURST_DUMP`, read with `tools/burst_read.py`) — built because a flicker cannot be
  shown in a screenshot, and built to DISCRIMINATE (its manifest carries the draw count and
  fingerprints so "issued and discarded" separates from "never issued"). `SAFE=1` on
  `tools/play_session.sh` holds god mode, no-death and ZOMBIES IGNORE ALL HUMANS so a capture
  session is not a fight. The draw census gained **`po=`, `su=`, `dc=`, `sr=`** — the four
  fields that made both register layouts confirmable.
* **Gates**: `--smoke` OK, Vulkan validation **6 `topology-08773`** (below the 20+6
  baseline), E3 **+0.8823** with 4 of 5 agreeing on layout on the polygon-offset build. **The
  full gate sweep on the FINAL stencil build is OWED** — the operator was mid-session when
  part 56 closed and the gates compete for the GPU.


## Part 59 (2026-08-20): the sweep, Night Run 1, and R6 closing the distance class

Three threads in one day. (1) The gate sweep owed since part 58's front=CW default:
all seven gates green (E3 +0.9596 identity, A5 0 real, PM4 oracles clean, dim census,
truncated=0). (2) Night Run 1, the operator-requested overnight campaign: memo value
replicated, guard4 refuted, clip cache partially attributed (+0.20 ms at 5k draws),
and the ALU constant census (owed item 4) — range-copy dead, gather-copy ~10x
(`perf-nightrun1.md`). (3) The operator delivered the R6 gas-sign trace and the
distance class closed the same day: SMALL PACKED TEXTURES — any texture with a
dimension <= 16 packs its whole chain, level 0 included, into one tile at mipAddr=0;
ours read level 0 at the tile origin and skipped the chain. Fixed in cf62229
(`PackedLevelOffset`, verified 69/70 chains + the part-41 square table 378/378);
operator: "Work really well now." Full records: `phase5-notes.md` §6co, `open-items.md`
00r. Prep for part 60 found `options_pc.txt` — a complete shipped PC graphics menu —
in `fecmn.big`. Operator instructions recorded this part: Fable 2 is not a 3D
reference; the next subject is waking that menu (display mode, resolution, shadow
tiers).

## The part-57 status block, moved out of CLAUDE.md by part 59

Where the port is, as of 2026-08-19 (part 57 CLOSED — **TWO OPERATOR SESSIONS ANSWERED ALL
THREE PICTURE ITEMS: USER CLIP PLANES went in and the slicing doubling is FIXED, the GAS
sign was ATTRIBUTED and its shader EXONERATED against the capture's own disassembly, and
the decal "never issued" verdict was REVERSED by one added census field.** `docs/part58-kickoff.md`
is the LIVE hand-off; the full record is `phase5-notes.md` §6cm; per-defect state is
`open-items.md` 00p; performance stays parked in `perf-state-parked.md`):

* **USER CLIP PLANES EXIST NOW, COMPUTED BY THE VERTEX SHADERS** — Vulkan has no
  fixed-function planes, so XenosRecomp (local patch, a gap SHARED with Fable 2) emits an
  `XE_USER_CLIP_PLANES` epilogue dotting the raw exported clip position against six planes
  the runtime publishes at SharedConstants+2080 from PA_CL_UCP_0..5. Null cache
  byte-identical; 104/104 VS carry ClipDistance with the define, 335/335 PS unchanged;
  `assets/shader_spv_clip_a2m` is the play cache (a2m + clip, one change per experiment).
  **The poison pair proved the chain before any zombie was asked** (gotcha 30 both ways):
  CZ_VK_CLIP_POISON blanked the picture on the clip cache — operator: *"cannot see anything
  and can hear main menu"*, validation ZERO non-topology lines — and changed nothing on
  stock. **Verdict: the halves SEPARATE** (plane 0 on 750,632 slicing-arm draws) where part
  56 gave two whole doubled bodies.
* **THE SLICING RESIDUALS ARE SCOPED, NOT GUESSED**: the cut is see-through (the GORE PLUG
  that seals it is clipped away) and a thin slab sometimes doubles. The ten-capture census
  decoded the game's whole technique — depth-prepass + z-EQUAL colour per half, then
  two-sided-stencil gore plugs (front REPLACE/back ZERO, per-piece ref; our pipeline reads
  those fields, retiring 00o's `ds.back` suspicion) — and **CZ_VK_CLIP_BIAS measured the
  margin: +0.01 of a plane's magnitude un-clips the ENTIRE body** (*"no longer see
  through/hollow but now back to two complete zombies"*), so the plug lives on a sub-0.01
  boundary where a systematic SPACE error in our dot eats it. Part 58 derives the right
  space from the captured planes + poses offline — no fitted epsilon.
* **THE GAS SIGN IS ATTRIBUTED AND ITS SHADER IS EXONERATED.** Draw-ID at the operator's
  own far viewpoint: the far disc+letters are `ps=57d441f53fc93ad7` (2,401-vert batched
  far-LOD material) and `ps=86ac6569ea0d700d` (letters) — the part-56 "17 far-only" set
  difference was a red herring, both appear near too. **Our translation of the first was
  read instruction-by-instruction against Xenia's own disassembly: FAITHFUL** — the step
  part 27 named is finally taken. The suspect is now the CONTENT of the tiny far-LOD
  textures (16x16/32x16/64x8 sub-tile tiled DXT1s): a live dump seconds after an operator
  F9 (the follow-up script fires it itself) shows the letters' 32x16 mostly BLACK with
  garbage while the same dump's 256x256 decodes perfectly with the same tool. **R6 — one
  hardware single-frame trace at the far viewpoint — is FILED and decides content vs
  pipeline** (`xenia-capture-requests.md`). Also learned: XenosRecomp silently drops
  getCompTexLOD/setTexLOD (156 of our PS carry the idiom; benign here).
* **THE DECAL "DROP" VERDICT IS REVERSED AND THE TITLE'S TRIPLE-BUFFERING ESTABLISHED.**
  The new burst per-draw census first read "decals not issued on dark frames"; adding va=
  (stream address) showed a 3-address ROTATING RING with content constant per address —
  folded by class, decal draws are issued EVERY frame. A blinking decal is
  issued-and-lost; prime suspect is our cross-frame stream store + frame-ahead guard
  against a 3-frame ring, `CZ_VK_NO_PARALLEL_GUARD=1` is the ready A/B. **The defect did
  not fire in either session** (*"now they are all working"*) — an observation with a
  shelf life, not a fix; nothing shipped this part explains it.
* **New instruments**: the burst census (`CZ_BURST_CENSUS`, `_EVERY`; ring-aware
  burst_read verdicts — its first verdict was WRONG and is retracted in place), census
  fields v0=/va=, `CZ_VK_NO_CLIP_PLANES` / `CZ_VK_CLIP_POISON` / `CZ_VK_CLIP_BIAS`, the
  chained four-arm session script with both poison controls FIRST, and the follow-up
  script's SELF-FIRING live_texdump watcher (fires within ~2 s of every F9 — keep it in
  every capture session). The null-cache diff also caught `vs_c17bbebf65383249` sitting
  in the dumps but missing from the cache — **the cache is 440**.
* **Gates**: all runtime verification ran WINDOWED through the operator (their
  instruction — "do not run it headless, let me do it"): validation clean on the clip
  cache, cache misses 0 everywhere, every arm's engagement counted. `--smoke` passes.
  The headless sweep (E3, A5, PM4 oracles) was not re-run — the changes are inert
  without the clip cache selected; run it when a part changes the default path.

## Part 58 status block (moved from CLAUDE.md by part 60, per the two-block rule)

Where the port is, as of 2026-08-19 (part 58 CLOSED — **THE SLICING SEE-THROUGH IS FIXED
AND IT WAS TRIANGLE FACING. One offline derivation answered the kickoff's space question
(there was NO space error), the operator's shift ladder closed the whole clip branch, and
the same session's captures found the real mechanism — a stencil-masked gore quad whose
mask our inverted front/back complemented. Operator on the fix: "Yes it is perfect now."**
`docs/part59-kickoff.md` is the LIVE hand-off; the full record is `phase5-notes.md` §6cn;
per-defect state is `open-items.md` 00q; performance stays parked in
`perf-state-parked.md`):

* **THE CLIP-SPACE QUESTION IS ANSWERED OFFLINE, WITHOUT THE VIEW MATRIX THE POSES
  FAILED TO CAPTURE** (all ten bvc blocks are the SHADOW pass's ortho — the "biggest
  draw" heuristic caught the shadow ground draw every time). The view matrix cancels:
  dot(P, Proj·v) = dot(Projᵀ·P, v), so the projection alone re-expresses a captured
  plane against view space, where lengths are true meters. **All 88 distinct captured
  planes are UNIT view-space normals (1.000 ± 0.0003 RMS)** under one fitted scene
  projection (fov 42.98°, 16:9 exact — not the pose first-draw's 45°): the register
  planes are exactly clip-space planes, our raw-oPos dot is hardware's dot.
  `tools/clip_plane_space.py`; gotcha 371.
* **PART 57's MARGIN INFERENCE IS RETRACTED AND ITS BIAS ARM RETIRED AS A PROBE**: the
  captured planes have c ≈ −d, so CZ_VK_CLIP_BIAS's `w += eps·|P|` lands entirely in
  the view plane's z-COEFFICIENT — a ROTATION worth 0.8–8 m of boundary at eps=0.01,
  which is what "un-clips the whole body" measured. The meters-true replacement is
  `CZ_VK_CLIP_SHIFT` (Δplane = Proj⁻ᵀ·(0,0,0,δ)); the operator ran the ladder
  0/+0.05/+0.02/−0.02, every arm's counter engaged, **nothing changed → the clip
  branch is CLOSED**. Gotcha 370.
* **THE REAL MECHANISM, FOUND IN THE LADDER'S OWN CENSUSES**: the cut is sealed by a
  6-vert QUAD stencil-tested EQUAL against a per-piece ref that the two-sided passes
  WRITE (front REPLACE / back ZERO; tester refs match writer refs in-frame). A
  two-sided-written mask depends on FACING — and facing had no other consumer in this
  renderer (culling permanently NONE; the title censuses su=00080008, cull off FACE=0,
  on every draw), so the hardcoded CCW front sat unverifiable until part 56 wired the
  stencil, then surfaced as the view-dependent see-through the operator described
  verbatim. One arm settled the coin toss and **FRONT=CW IS NOW THE DEFAULT**;
  `CZ_VK_STENCIL_CCW_FRONT=1` is the same-binary control arm. Gotcha 372.
* **On watch, not chased**: the part-57 doubled-slab sighting did not re-appear in any
  of part 58's three operator sessions; severed pieces despawn in ~3 s (hardcoded, no
  tunable) so the analysis route for any future cut defect is F9 + an immediate F8
  burst. The ±0.05 arms' predicted "visible doubled band" was wrong in presentation
  (ragdoll halves separate; each just gets 5 cm longer) — the engagement evidence that
  counted was the counters.
* **Gates**: `--smoke` OK after every change; all picture verification WINDOWED through
  the operator (three sessions this part). **The headless sweep (E3, A5, PM4 oracles,
  dimension census) is OWED**: part 58 changed a DEFAULT (front=CW), which is exactly
  the condition part 57 named for re-running it — but the operator's standing
  instruction is that they run the game, so it is the first ask of part 59.

## Part 59 status block (moved from CLAUDE.md by part 61, per the two-block rule)

Where the port is, as of 2026-08-20 (part 59 CLOSED — **THREE THREADS, ALL LANDED IN ONE
DAY: the owed GATE SWEEP ran all green; NIGHT RUN 1 (the operator-requested overnight
perf campaign) answered the parked plan's owed items 1/2/4; and the operator's R6 trace
closed the GAS-SIGN / DISTANCE CLASS the same day it was delivered.** `docs/part60-kickoff.md`
is the LIVE hand-off (subject: wake the shipped PC graphics menu); the R6 record is
`phase5-notes.md` §6co; per-defect state is `open-items.md` 00r; performance stays parked
in `perf-state-parked.md`, updated by Night Run 1's §6):

* **THE DISTANCE CLASS WAS NEITHER CONTENT NOR SHADER — IT WAS THE PACKED-TILE LAYOUT.**
  Any texture whose shorter dimension is <= 16 texels packs its WHOLE mip chain, LEVEL 0
  INCLUDED, into one 32x32-block tile (mipAddr=0): a 32x16's base sits at block (0,4),
  ours read (0,0) — the tile's scrap region — and the `mipAddress &&` gate skipped the
  chain upload entirely. 79 distinct textures in one street frame are in the class (the
  whole tiny far-LOD sheet population). Bytes were exonerated first: hardware's letters
  base is BYTE-IDENTICAL to our live dump at the same spot. `PackedLevelOffset`
  (transcribed from Xenia's GetPackedMipOffset, BSD-3 recorded, then VERIFIED — it
  reproduces part 41's brute-forced square table 378/378 and 69/70 R6 chains are
  consistent at its offsets) fixed both halves in commit cf62229. First session: 1,662
  base-offset reads, 2,404 packed levels taken, operator: "Work really well now."
  `CZ_VK_NO_PACKED_SMALL=1` is the control arm. R6 is FULFILLED; no outstanding request.
* **NIGHT RUN 1** (unattended, 24/24 runs engaged, null floor <= 0.4% on draw-banded
  window medians): the constant memo's value REPLICATED (+1.8-2.2% cost to remove),
  `CZ_VK_GUARD_WORKERS=4` is REFUTED (slower than budget-3), the clip cache costs
  +0.20 ms/+3.0% at the 5,000-draw band (partial attribution — part 56's dynamic-state
  calls stay co-suspect), and the ALU CONSTANT CENSUS killed item C's range-copy design
  while pricing the GATHER design at ~10x (median 9 VS / 27 PS registers read of 256;
  a0-indexing on only 22 VS, 0 PS). Two instrument facts with a shelf life: the
  DebugJump landing is BIMODAL (~2,500 or ~5,000+ draws per run), and the uncapped
  headless load runs the GPU at 75-85% — near the limiter — so CPU deltas read
  compressed there. `docs/perf-nightrun1.md` is the record.
* **The gate sweep (owed since part 58's default change): ALL GREEN** — E3 +0.9596
  identity, A5 exit 0 (4 permutation / 0 real), both PM4 oracles 0 disagreements, dim
  census clean, truncated=0, no-translated-shader 0. One trap recorded: a parked
  no-input boot drifts into the attract loop, so the E-gate scans the frame dump for the
  logo card (~frame 5,000) rather than trusting the final frame.
* **Part 60 is prepped and the discovery is made**: `fecmn.big` ships BOTH the live
  Visuals screen (whose ONLY setting is the dead Gamma meter) and **`options_pc.txt` —
  a complete PC graphics menu (Resolution, DisplayMode, VSync, Shadow Low/Med/High,
  more)**. The DebugJump precedent applies. `big_decompress` gained a text oracle for
  the frontend layouts. Operator instruction recorded: do NOT consult Fable 2 for
  renderer behavior — this port's own history is the reference.
* **Gates**: `--smoke` OK after every change; picture verification WINDOWED through the
  operator (three sessions: fix verdict, 1440p play). The sweep above covers part 56's
  owed one too (same binary lineage).

Where the port is, as of 2026-08-21 (part 60 CLOSED overnight — **THE SETTINGS MENU
SHIPPED AND THE WHOLE NIGHT PLAN LANDED: the shipped OptionsPC screen proved to be a
SHELL (layout + strings present, verbs compiled out), so the menu is a HOST panel over
the game's own Options hub — five rows: Resolution (display-clamped, BOTH aspects in
one list per the operator's morning revision — 1280x720/1680x720/2560x1440/3360x1440
on their display, selecting sets scale+aspect for the next launch), Display Mode,
VSync, Shadow Quality (LIVE tiers), Frame Cap (LIVE, microsecond vblank period).** `docs/part61-kickoff.md` is the LIVE hand-off (subject: the
operator's morning verdicts); the night record is `phase5-notes.md` §6cp; gotchas
373-375):

* **Aspect-fit presentation is the DEFAULT** — black bars instead of stretch on both
  present paths, one shared computation (`Host_AspectFitRect`); `CZ_VK_STRETCH=1` is
  the control arm.
* **Shadow tiers are wired and LIVE**: the cascade pass is identified by its measured
  EDRAM pitch (1040 = a 1024-wide surface; census — nothing else uses it), the same
  predicate scales draws and resolves, High = scene scale (bit-identical control),
  Med/Low = half/quarter floored at 1x. `CZ_VK_SHADOW_TIER` wins over the file.
  Headless halves verified (atlas at 1x vs 2x host, 18.8M draws counted, frames
  intact); Low-vs-High blockiness is the operator's.
* **21:9 works headlessly end to end**: every render-pipeline X extent * 21/16
  (TRUNCATING — gotcha 373), the 16:9-perspective discriminator patches the scene
  projection's fov (350k patches on the outdoor route), UCP planes compensated so
  gore cuts stay put, cube maps stay square via a face blit. **The planned HUD
  mechanism was refuted by measurement — the title's UI sits under the recognized
  perspective and centers ITSELF** (gotcha 374; two independent measurements land on
  the centering prediction). Same attract backdrop 16:9-vs-wide shows new flank
  geometry, not stretch. `CZ_VK_WIDE` env arm; `aspect=1` next-launch.
* **Frame cap row applies within one pump tick** — the vblank period is a live atomic
  in MICROSECONDS (90 fps needs 5,555 us; the old ms knob would have served 100 —
  gotcha 375); env pins it and the menu is refused loudly under a pin; CZ_FPS_CAP=30
  still reproduces shipped pacing bit for bit. Stated cost: a live change leaves the
  guest's cached refresh stale until restart.
* **Gates at close**: `--smoke` green after every commit; A5 diff exit 0 (4
  permutation / 0 real); validation-armed wide boot shows ONLY the pre-existing
  point-list PointSize VUID class (6, same as the 16:9 control — now a named cheap
  open item); E3 identity re-run at close (see §6cp / the closing commit).
* **THE OPERATOR DAY THAT FOLLOWED (2026-08-21, §6cq): verdicts in, three Resolution
  revisions, one freeze fixed, one game completed.** Bars + 21:9 good; the Resolution
  row now lists THE DISPLAY'S OWN MODES with a RATIONAL scale core (Y by H/720, X by
  W/1280, truncating — 1920x1080 and every >= 16:9 display mode from 720..2880 rows
  works; res_w/res_h are the settings keys, legacy files convert). The live
  shadow-tier flip FROZE the game — vkDeviceWaitIdle does not cover the RECORDING
  command buffer (gotcha 376) — fixed by deferred image retirement
  (RetireImage/DrainRetiredImages; stress arm CZ_TEST_TIER_FLIP, 48 live rebuilds
  clean under validation, operator re-tested the same spot). "The menu opens on
  720p" was the ordered-ladder WRAP from the last entry (gotcha 377; ladders clamp
  now). The operator then COMPLETED THE WHOLE GAME with CZ_SHADER_DUMP armed: cache
  440 -> 449. Owed: the shadow Low-vs-High LOOK, the clamp-fix confirmation, and
  first sight of the eight late-game shaders. **THE NEXT PLAN IS
  `docs/rt-and-fov-plan.md`** (RT shadows/AO/lighting/GI rows + FOV slider, operator
  instruction). Commit-hygiene note: items 4+5 share one commit (893749b).

Where the port is, as of 2026-08-21 night (part 61 CLOSED, and **PART 62's SAME-NIGHT
CORRECTION IS THE HEADLINE: the operator's first live session refuted part 61's
slider in one sentence ("only impact the UI... in game it doesn't do anything"), and
the miss-dump found why — THE WORLD'S DRAWS CARRY A VIEW-PROJECTION COMPOSITE P*V AT
c0-3, not the raw projection; part 61's fov patch AND part 60's wide patch only ever
matched the raw form (= UI + frontend). Commit 943227d recognizes both forms: the
fov slider is COMPOSITE-ONLY (world moves, HUD pixel-static — the operator's exact
requested scope) and 21:9 GAMEPLAY IS UNSTRETCHED FOR THE FIRST TIME (it had been
stretched ~34% since part 60 — only the frontend was ever truly widened).**
`docs/part62-kickoff.md` was the hand-off (superseded by part 63's close; see the block above); records `phase5-notes.md`
§6cs (+ retractions in §6cr/§6cp), gotchas 378-rewritten/379/380):

* **Two forms, cleanly split, all measured**: raw projection = ONE bit-identical
  matrix game-wide (vfov 45.00°, zn 0.1/zf 1000, ~2% of draws — UI and frontend);
  world = P*V composites (gameplay camera vfov 41.64°, per-camera zoom, 95%
  depth-writing). `SceneXformForm()` recognizes composites by P's surviving
  structure (unit view row, 9/16 row-norm ratio, orthogonality, z-row
  proportionality); shadow orthos / skinning affines / cube-face cameras fail it by
  construction — all enumerated in the miss dump BEFORE the recognizer was written.
* **Verification method lesson (gotcha 378 rewritten)**: part 61's "whole scene
  widened" was CAMERA DRIFT between two processes — a projection change is verified
  by a SAME-RUN flip (`CZ_TEST_FOV_FLIP=N`; meandiff alternates ~4/~40 across flip
  boundaries with the camera held). Composite-only run: 17.4M composite patches,
  raw counter ZERO, HUD pixel-identical across states.
* **The FOV slider (5b9fbba + 943227d)**: sixth panel row, -10..+30, LIVE; null
  byte-identical; UI ratio measurements stand (0.716/0.714 vs 0.7174 at +15 — now
  the CZ_VK_FOV_RAW arm's signature). RT stage 0 (4cc4f4a): RTX 3070 carries all
  ray-query extensions — hybrid RT AVAILABLE; stages 2-4 wait on stage 1's census,
  whose hardest question §6cs answered free (world meshes are WORLD-SPACE streams —
  BLAS-ready; the view matrix is extractable from any composite).
* **THE VERDICTS CAME IN THE SAME NIGHT AND FORCED TWO MORE MECHANISMS (§6ct +
  addendum): the slider is now GAME-SIDE** — the roaming camera's fov is a
  behavior param node read at ONE getter site (sub_8246BF48 / lr 0x8246E31C);
  the hook enforces authored+N there, so the game renders AND CULLS wide, HUD
  and cutscenes untouched (8d3de05; the node is STATE — capture authored at
  first sight, enforce absolute, never +=). **In wide mode the substitution
  also over-widens by k=9W/16H in tan space and the composite wide patch flips
  to row1*k** (22195be) — the 21:9 flank-culling gap (view 34% wider than the
  frustum since the unstretch) closes; cutscenes become a 21:9 crop by design.
  **Operator: "It works."** Cost: turn-stutter from the over-widened frustum's
  first-sight upload bursts — operator-deferred, filed in perf-state-parked.md.
  Renderer-side fov patch = measurement arm only (CZ_VK_FOV); CZ_NO_GAME_FOV=1
  is the game-side control arm. Still open, non-blocking: gore cut off zero,
  aiming behaviour, the cutscene-crop look.

Where the port is, as of 2026-08-21 late night (part 63 CLOSED — **RT STAGE 1, THE
GEOMETRY CENSUS, IS DONE AND THE VERDICT IS GO FOR STAGE 2 (RT shadows), CHEAPER THAN
THE PLAN BUDGETED.** `docs/part64-kickoff.md` is the LIVE hand-off (subject: RT stage 2 —
ray-traced shadows, LOW tier first); the record with every number is `phase5-notes.md`
§6cu + addendum; the instrument is `CZ_VK_RT_CENSUS=1` (instruments.md); zero renderer
changes):

* **The world's geometry is BLAS-ready almost by accident**: position data is **100%
  float3 at 28-32 B stride** (the short4 possibility does not exist here), **96.8%
  triangle strips, 100% 16-bit indices**, and it lives in the SAME persist-cache
  buffers the raster path uploads — one usage bit away from BLAS build input, zero
  copies. The only data transformation stage 2 needs anywhere is strip→list index
  expansion (2.7-5.9 MB of u16 cumulative).
* **98.1% of world bytes are BLAS-once stable** (full-FNV cross-frame content check;
  the rewritten class is 134 map-wide smallware streams, 1.4 MB); streams are
  WORLD-SPACE (§6cs confirmed by the census's bounds scan surviving its refutation
  check — the three ±6.3M outliers are named 12 B-stride junk streams), so every TLAS
  instance is the IDENTITY transform. TLAS scale: ~2,100-2,600 instances typical,
  worst observed frame 7,034 world draws / 3.46M drawn tris (÷2 for the 640-wide
  tiles). Roam churn ~13 new stable streams (~150 KB)/s — async-build trivial.
* **The skinned exclusion is STRUCTURAL, and it is 3.0% of scene draws**: actors carry
  an AFFINE at c0-3 (§6cs), so the composite gate is itself the rigid-world filter;
  the form-0 bucket split by the pitch-1040 predicate reads 90.1% shadow cascade /
  8.1% skinned / 1.3% cube faces. Zombies cast no RT shadows in stage 2 (OG cascade
  stays the fallback) and the panel note can now quote the size of that hole.
* Three census runs (two standing at the camp crowd — replicating to within 2% on
  every cumulative number — one 600 s EXPLORER roam), `no translated shader` = 0 on
  all three; gates at close: `--smoke` green, A5 exit 0, E identity (see the closing
  commit). Carry-overs unchanged from part 62 (turn-stutter parked, fov small
  verdicts, panel input leak).

Where the port is, as of 2026-08-22 (part 64 CLOSED — **RT SHADOWS: ROUTE (a) WAS
BUILT END TO END, PROVEN AS A MECHANISM, AND THEN CLOSED AS UNWORKABLE BY
MEASUREMENT. ROUTE (b) IS PART 65's SUBJECT, by the operator's decision.**
`docs/part65-kickoff.md` is the LIVE hand-off; the record is `phase5-notes.md`
§6cv — **§7j is the verdict, §7f is the operator's session, §7e is a retraction
that matters**; the backlog entry is `open-items.md` 0v; the arms are in
`instruments.md`; the lessons are gotchas 381-386):

* **Proven, and not to be re-derived**: the shadow ATLAS SNAPSHOT is where the
  title's shadow term reads and needs NO shader patch — `CZ_VK_SHADOW_FILL=0.0`
  takes the outdoor median luma **80.61 → 61.43** and `=1.0` reads **81.46** (two
  polarities, opposite directions, ~11k outdoor frames an arm); the convention is
  STANDARD (near = occluder); and the WHOLE ray path reaches that term —
  `CZ_VK_RT_POISON=1` reads **61.18** against the direct fill's 61.43, i.e. the
  same measured extreme through BLAS + TLAS + ray query + composite. The plumbing
  engages and holds (~1,400 BLASes, ~33 MB, zero pool flushes, zero collisions).
* **Closed as unworkable: route (a) SELF-SHADOWS BY CONSTRUCTION.** Writing the
  shadow MAP means every receiver inside it is compared against itself, and there
  is no receiver-side offset to apply. **Five independent knobs — occluder set,
  union vs replacement, the light-matrix binding, a bounds gate, and a 6.7x bias
  sweep — all land at 64-66 median outdoor luma against OG's 80.61**, floor 61.2.
  The operator named the mechanism in one sentence (*"shadow squares following
  where the player is and normal shadow still on"*) after three headless
  statistics had failed to, and stills show every LIT surface greyed. **Do not
  re-open this by tuning.**
* **Three real defects were found and fixed on the way and NONE moved the
  picture** — keep the fixes, re-buy none, expect none to be an answer: junk
  ±6.3M-unit geometry entering the BLAS (gated); the title's own cascade being
  **52.8% EMPTY** because it draws CASTERS not receivers (now the default set);
  and the light matrix bound by RECENCY when the cascade pass carries several
  distinct c0-3 per slice (**0 slices with one against 28,704 with several**),
  now bound by DATAFLOW with the scene pass as the oracle.
* **AND MANY MID-SESSION NUMBERS WERE RETRACTED (§6cv 7e).** They were read from
  `CZ_VK_FRAME_STATS` files still being written and from cumulative counters
  mid-run: the same arm reads 63.71 at 4,663 outdoor frames and 66.34 at 6,484,
  or 72.00 at 1,212 and 66.40 complete. **A partial read on these routes measures
  a different PLACE, not a noisier version of the same one** (gotchas 384-386;
  386 is the build that measured 80.61 against a control's 80.61 because the
  feature was silently inert).
* **Shipped and staying**: the operator's settings revision — **ONE `SHADOW`
  row**, LOW/MEDIUM/HIGH then RT LOW/MEDIUM/HIGH, where an RT value REPLACES the
  raster shadow and the raster tier is remembered underneath. RT is OFF by
  default, so nothing the operator plays is affected.
* **Route (b) is the live direction**: compute the shadow factor per RECEIVING
  PIXEL in screen space and patch the atlas-sampling pixel shaders to read it —
  the defect is impossible by construction, and it is the only route that can do
  soft or per-pixel shadows. Everything part 64 built is reusable unchanged.
  **Step 1 is a CENSUS of which shaders fetch `1439B000` and at which slot; build
  nothing before it returns a list.**
* **Gates at close** (final binary, RT off = the shipped default): `--smoke` OK;
  **A5 exit 0**; unlowered switches 0 defects; shader dim census 0 disagreements;
  `no translated shader` = 0. Carry-overs unchanged from parts 62-63.

Where the port is, as of 2026-08-22 (part 65 CLOSED — **RT SHADOWS, ROUTE (B) IS
BUILT END TO END AND HAS NEVER BEEN LOOKED AT. The one thing owed is an operator
session, and it is scripted.** That session RAN, in part 66 — five of them — and
`part66-kickoff.md` WAS the hand-off then; the live one is named above.
The record is `phase5-notes.md` §6cw; the backlog entry is `open-items.md` 0v; the
arms are in `instruments.md`; the lessons are gotchas 387-390):

* **The census answered step 1 offline, against HARDWARE, in minutes** — the
  binding "fetch slot N holds the shadow atlas" lives in a register, not in the
  microcode, so it looked like it needed an instrumented run; a `.xtr` carries
  hardware's register file per draw, so twenty capture files already on disc
  settled it (gotcha 387). **126 pixel shaders, 140 (shader, slot) pairs**,
  42,620 draws — against the plan's guess of "a dozen". Slots 0-7 and 9 all
  appear. `tools/shadow_shader_census.py`, `config/rt_shadow_slots.json`.
* **Only TWO use shapes exist and both are monotonic and saturating**: `pcf4`
  (116 uses — four ±0.5 taps compared `> receiverDepth`, bilinearly weighted) and
  `tap1` (24 uses — one centre tap feeding `saturate((receiver−sampled)*k−bias)`).
  So **one substitution serves all 140**: 1.0 at every atlas tap reads as LIT
  everywhere, 0.0 as OCCLUDED, whatever the weighting.
* **The weights are patched too, and that is what buys SOFTNESS.** The emitted
  2x2 products come in THIRTEEN swizzle pairings, so no pattern match covers
  them — but every one is a product of two of {a,b,1−a,1−b}, so forcing
  `getWeights2D` to 0.5 makes every product 0.25 whatever the swizzle and the
  shader computes a plain mean (gotcha 389). Five levels, stated as the ceiling.
* **Shipped**: `tools/patch_rt_shadow_hlsl.py` + a `CZ_HLSL_PATCH` hook in
  `build_shader_spv.sh` (the shared recompiler untouched);
  `runtime/gpu/rt_factor.hlsl`; the renderer wiring (a variant module per shader,
  `PipelineKey::passFlags`, the pass triggered by the title's OWN first
  atlas-sampling draw and invalidated at every resolve). The tier ladder is real
  now — RT LOW half-res 1 ray, MEDIUM full-res 1 ray, **HIGH four rays over the
  sun's disc**, which route (a) could never express.
* **Three of part 64's hardest problems do not exist on route (b)**: the
  slice↔matrix pairing (only the sun's DIRECTION is used), the depth convention
  (nothing is written to a depth buffer) and the occluder set (preferring the
  title's own casters was a property of writing the map).
* **The null is byte-identical**: with `CZ_VK_RT=0`, or no variant cache, or the
  SHADOW row on a raster tier, nothing runs and the stock modules are bound. The
  EDRAM depth gains `SAMPLED_BIT` only on a ray-query device.
* **Found and fixed on the way, unrelated to RT**: the operator's play cache
  (`shader_spv_clip_a2m`) was TEN SHADERS SHORT since 2026-08-19 — absent, not
  stale, so their draws were SKIPPED in every session for three parts (gotcha
  390); and the last sidecar-less cache entry was rebuilt from microcode part 64
  recovered.
* **The first validation boot with RT armed CRASHED, and the layer named all
  three causes** — an AS build inside a render pass, its barrier, and a
  `vkUpdateDescriptorSets` with a null `dstSet` (route (a)'s descriptor set,
  which route (b) never creates). Fixed; the RT arm and a plain `CZ_VKDRAW=1`
  boot now produce **identical validation output**. This is the fifth defect
  `CZ_VK_VALIDATION=1` has caught that nothing else could.
* **Engagement, headless, boot to title — NOT a picture claim**: 7,131 factor
  passes, **1,921,744 draws served through the 126 variant modules**, 243-573
  TLAS instances, sun (-0.381, 0.812, -0.443), zero `noTlas`/`singular`. The
  scene-composite binding check reads **2,343 frames with ONE composite, 0 with
  SEVERAL** — the check part 64's light matrix failed outright.
* **Gates at close** (RT off = the shipped default): `--smoke` OK; **A5 exit 0**
  (4 permutation windows, 0 real); `shader_dim_census.py` clean on every cache;
  each RT cache differs from a plain rebuild in **exactly the census's 126
  modules** — no extra, none missing. Carry-overs unchanged from parts 62-64.
