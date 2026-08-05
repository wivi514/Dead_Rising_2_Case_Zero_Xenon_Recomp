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

The full list lives in the Fable 2 and Asura's Wrath CLAUDE.mds and is not duplicated
here. The ones that have already proved relevant to *this* title, plus the ones that are
about to:

1. `mftb` is compiled to `__rdtsc()` — force-include a 49.875 MHz timebase over PPC TUs.
2. XenonRecomp ends functions at `bdzlr`/`bdnzlr` (conditional returns!) — a truncated
   CRT memset corrupts silently; fix with `[main].functions` size overrides.
3. **Jump-table mis-detection — and a zero is a detection failure, not a fact.**
   XenonAnalyse found **zero** tables in this 8.8 MB code section; our scanner found
   **232**. Never trust a zero, and hand-audit anything the scanner is unsure of. A
   missed table emits `return;` without restoring non-volatiles.
4. All optimization flags OFF until a run works.
5. Kernel stubs must fail honestly, never fake success (Fable 2's XMA context bug cost
   weeks). Corollary: a stub that returns an error but leaves its **out-parameter**
   untouched is worse than no stub — the guest often ignores the status and reads the
   buffer anyway.
6. The alias/weak-link seam (`PPC_FUNC` overriding `__imp__sub_X`) is how all hooks and
   whole-image probes are built.
7. A probe expensive enough to stall the game manufactures the stability it reports —
   every instrument needs its own control.
8. `log_level=3` is the *minimum* for named kernel calls in Xenia, and still not the whole
   surface: `kHighFrequency` exports need `log_high_frequency_kernel_calls=true`, which
   defaults off and hid 40 of 288 imports on Asura's Wrath.
9. Put the guest arenas where the console puts them, **and round every size the kernel
   reports the way the console rounds it** — the guest's own heap manager builds its map
   from those numbers.
10. The image is the authority on what a title imports — `ppc/ppc_recomp_shared.h`, not
    the previous port and not the capture.
11. A recompiler's `default: __builtin_unreachable()` converts every static-analysis gap
    into an arbitrary-code jump. Already patched in the shared XenonRecomp checkout.
12. A function-coverage capture is a **two-sided** oracle: forwards recovers missing entry
    points, backwards localises a control-flow divergence. Treat its function boundaries
    as ranges, never identities.
13. A capture request is a hypothesis with a shelf life — re-read it against the current
    ledger before running it, and re-read the delivered *notes* against the ledger before
    believing their conclusions.
14. Adjacency in *our* arena is evidence about our allocator, never about the title.

New here, and transferable to any XBLA port:

15. **`encryption = 1` does not say which key.** Xbox 360 XEXs use either the retail key
    or the all-zero devkit key, and nothing in the header distinguishes them. XBLA titles
    commonly use the devkit key; this one does. Stock XenonRecomp hardcodes retail and
    **returns an empty image with no diagnostic** — base 0, size 0, zero sections — which
    reads as a broken TOML or a corrupt file, not as a wrong key.
16. **A container's "compression" field can mean a codec, or a table of zero-runs, and
    the wrong reader does not error.** `compression = 1` (basic) is a list of
    `(data_size, zero_size)` pairs; `compression = 2` (normal) is LZX. `decrypt_xex.py`
    reads a normal-compression header as a basic block table and produces a confident,
    entirely fictional block list. Both template ports would have accepted it silently.
17. **A long register-save ladder contains a short one.** `__savevmx_64`'s 46th rung is
    `li r11,-0x120` followed by 17 more pairs and a `blr` — an exact match for an 18-pair
    `__savevmx_14` scan, at an address 0x170 *inside* a function the same scan just
    called 516 bytes long. What caught it was the cross-check that the four vector
    ladders must be **contiguous**; the individual matches all looked fine.
18. **One title can use two different base registers for its ladders.** The gpr ladders
    here are r1-based, the fpr ladders r12-based. Asura's Wrath's notes warn the base
    register varies between titles; it varies within one too.
19. **v14–v31 use classic VMX encodings, v64–v127 use VMX128** (`stvx` = opcode 31,
    `stvx128` = opcode 4), and VMX128 spreads the 7-bit register number across the
    instruction so the high bit lands in bit 2 of the low half. A matcher that doesn't
    mask that bit stops halfway through the 64-rung ladder — at register 96, which looks
    like a plausible ladder length.

From round 1's captures (details in the findings ledger):

20. **An emulator's licence state is part of the capture config, and its default is
    wrong for a paid XBLA title.** Xenia's `license_mask` defaults to 0 → the game boots
    its **trial**, which is not a subset of the full game (finding 1). Nothing in a disc
    title's methodology catches this.
21. **A coverage trace's "functions" include every branch target the emulator saw
    executed** — so a recovered jump table's case labels arrive looking exactly like
    undiscovered entry points. Here 870 of 1,090 candidates. Adding one **splits the
    switch's parent**, turning its remaining cases into bare `return;` — the exact defect
    the repair tool exists to fix, reintroduced by the tool meant to improve coverage,
    and the two then fight to a stable non-zero error count instead of converging
    (finding 5a). Two "functions" 4 bytes apart sharing an end address are one function.
22. **An analysis image has a *stage*, and a byte pattern is only valid for one stage.**
    XenonRecomp overwrites every import thunk with `nop;nop;nop;blr` during load, so a
    scanner looking for the on-disk `mtctr r11; bctr` form finds **zero** in a dumped
    loaded image — and zero silently promotes all 244 thunks to "missing functions"
    (finding 5b). Asura's Wrath never hit this because its image came from
    `decrypt_xex.py`, which never runs the loader.
23. **A tool that reports convergence is making a claim; check it against the thing it
    claims to have fixed.** `fix_switch_function_bounds.py` printed "0 new this round"
    through a whole fixpoint loop while three errors persisted, because it computed a
    function's end from the *widened* start and emitted entries that ended where the
    real function began (finding 5c).
24. **Check a log's line shapes before filtering on one.** Xenia emits `d>` `i>` `G>`
    `A>` `!>` `F>` and unprefixed continuations. `VdSwap` is logged at `i>`, so a
    `d>`-only filter reports `VdSwap = 0` — a clean, small, wrong number that reads as
    "this title never swaps" (finding 4).

From round 2 (closing phase 0.1):

25. **A grep that cannot match is not a clean result.** The bootstrap claimed "zero
    `// ERROR:` comments in the generated code". The recompiler emits `// ERROR {:X}` —
    **no colon** — so the pattern never had a chance to match and the check was never
    run. The true count was 31. Before believing a zero, confirm the pattern can match
    *something*; the cheapest version is to grep the emitter, not the output.
26. **A dropped direct branch is an unimplemented instruction wearing a different hat.**
    When a branch target is not the exact start of a known function, XenonRecomp emits a
    bare `// ERROR <addr>` comment and nothing else. No stdout diagnostic, exit 0, and
    the C++ compiles — the control transfer just never happens. Measure it with
    `tools/find_dropped_branches.py` (finding 13).
27. **The direction of a dropped branch names the defect and the repair, and they are
    opposites.** Backward → a loop header was declared a function and split a real one;
    remove the start. Forward → the function was truncated before its outlined cold
    block; widen it. Applying either repair to the other class makes things worse.
28. **The coverage oracle's mid-body trap is not limited to switch labels.** A loop
    header is a branch target too, appears in no switch table, and passes every
    case-label filter. Nine got through here. No pre-hoc heuristic separates them from
    genuine indirect-call targets — two of the nine looked completely ordinary — so the
    coverage tool *proposes* and the dropped-branch check *disposes*. Run them in that
    order, always (finding 5d).
29. **An "implemented" instruction can still be impossible.** `VADDUWS` emitted
    `simde_mm_adds_epu32`, which does not exist in simde and has no SSE equivalent at any
    level. It had presumably never been exercised. A recompiler case is only proven by a
    title that uses it *and* a compile that consumes it (finding 14).
30. **A vector test that has never failed has not been shown capable of failing.** Vector
    lowering hides two invisible conventions — the whole-vector byte reversal (which
    swaps pack operand order) and saturation edges. Both are silent wrong *values*, not
    crashes. Write the differential test against scalar PPC semantics, then break the
    implementation on purpose and confirm the test screams.

From phase 0.2 (the first compile):

31. **The recompiled image needs Clang, not a preference but a requirement.**
    `ppc_context.h` builds `PPC_FUNC_PROLOGUE()` on `__builtin_assume`, which GCC has no
    spelling for — so GCC fails in *every one* of the 57,822 function bodies. Select the
    compiler **before** `project()`; after it, CMake has already locked one in.
32. **A force-included `#define __rdtsc()` breaks the system headers, not the guest.**
    The `mftb` shadow must not be in scope when `<immintrin.h>` declares its own
    `__rdtsc(void)`, or the build dies inside a system header with an error naming
    neither this project nor the guest. Include `<x86intrin.h>` first to set the guard,
    then `#undef`/`#define`.
33. **`PPC_FUNC` vs `PPC_FUNC_IMPL` is a linkage difference that only surfaces at link
    time.** `PPC_FUNC_IMPL` is `extern "C"`; the image's references are C++-mangled,
    because `ppc_recomp_shared.h` declares imports with a plain `extern PPC_FUNC(x)`.
    Define kernel-import stubs with **`PPC_FUNC`**. Guest functions escape this only via
    their `__attribute__((alias(...)))` weak alias — the same seam every hook uses.
34. **The save/restore ladders are not missing symbols.** All 236 are declared in the
    shared header and called everywhere, and XenonRecomp defines them itself as
    `__imp____savegprlr_14` with a weak alias. Stubbing them gives 236 duplicate symbols.
    Only `__imp__`-prefixed names are genuinely undefined.
35. **Link the image with `--whole-archive` for the phase 0 gate.** A normal
    static-library link pulls in only referenced objects, so an unreferenced TU carrying
    an undefined symbol links cleanly — and the gate passes while proving nothing.

From phase 0.3 (the `.xtr` decoder, closing finding 10):

36. **A capture's notes prescribe a *method*, and skipping it manufactures a finding
    about the title.** Gotcha 13 says to re-read a capture's notes before believing
    their *conclusions*; this is the other half. B1/B1b's notes said to align over the
    fixed boot+movie prefix and ignore the idle tail. Comparing whole runs instead
    produced "16% of frames agree — NOT content-deterministic", a confident claim about
    the game that was purely an artifact of including 619-vs-409 frames of *a human
    deciding when to press exit*. Same data, correct window: 0.42%.
37. **An emulator-side bookkeeping field can be deterministic in aggregate and
    non-deterministic per frame.** `MemoryRead` counts agree to 0.37% in total and align
    on only 17.7% of frames, because Xenia's dirty-tracking decides *when* to record, not
    *whether*. Folding them into a content fingerprint dropped frame agreement from 42.7%
    to 16.0%. A field's total being stable is not evidence that its distribution is.
38. **Gate on per-era aggregates, never on absolute frame index.** Even over the correct
    window, frame-exact agreement between two *hardware* runs is only 80.0% — phase drift
    concentrated at lag +3. A frame-indexed GPU gate would report ~20% divergence against
    a correct renderer.
39. **One opcode can be recorded differently from all the others, and the shape of the
    disagreement names the cause.** Spread across many opcodes → the bit layout is wrong.
    All of one and none of any other → that opcode is special. `INDIRECT_BUFFER` is stored
    one dword short (the size lives in the following `IndirectBufferStart`), so a replay
    tool that trusts either length feeds the command processor a malformed packet.
40. **An unexercised bounds check is not a working one.** The ported `.xtr` `step()`
    computed a next-offset past EOF without checking it. B1 never triggered it; B1b did,
    on the first run, because these captures stop mid-command. "It has always worked" was
    a statement about the inputs.
41. **A check that always fires is a check people learn to ignore.** The response to a
    known-benign alarm is to encode the knowledge (`xtr.PM4_SHORT_RECORDED`), never to
    widen the tolerance — widening it also silences the unknown cases it was built for.

From phase 1 (the runtime; details in `docs/phase1-notes.md`):

42. **A generated stub cannot obey the out-parameter rule, and the fix is to say so.**
    "A stub must fill its out-parameter or not exist" is right, and a generator working
    from a list of *names* has no signature to obey it with. Guessing by name prefix
    would be wrong silently. State the limit in the generator, name the failure modes,
    and keep the escalation short — a real signature in `imports.cpp`. Two recognisable
    symptoms: a guest trusting an untouched out-buffer (corruption far from the call),
    and **a segfault on or near `0xC0000002`, which always means an unimplemented import
    was asked for a pointer** rather than an NTSTATUS.
43. **A ported diagnostic can carry an assumption about the memory map it came from.**
    Both template ports bound their guest-stack scans with `addr < 0x80000000`. Our
    guest thread blocks come from the o1heap arena at `0x88000000`, so the bound failed
    on the first word and every stall trace printed `callers:` with nothing after it —
    an instrument that looks like it ran and found nothing. Gotcha 25 in a new place.
    Every inherited constant that is a *range* deserves the question every inherited
    *address* gets.
44. **When the safe value is a property of the generated function list, compute it from
    the function list.** Minted export thunks must land inside the dispatch table's
    CODE range. Asura's Wrath hardcoded an address that was 4.7 MB outside it and the
    stub never ran (its finding 54); Case Zero has no free constant at all — 16 bytes
    between its last mapped function and the end of the range. Scanning for the longest
    unmapped run costs one pass per process and cannot go stale.
45. **Two captures of "the same drive" need not be nested.** A5 is A1's drive with
    high-frequency logging on, and it is *nearly* a superset — but 11 names appear only
    in A1, the whole storage-device-selector path that drive did not enter. Treating the
    richer capture as authoritative everywhere manufactures divergences.

46. **The contract of an async call includes its notification, not just its data.**
    Our `NtReadFile` filled the buffer, filled the `IO_STATUS_BLOCK` and returned
    success — and hung the boot, because this engine signals completion through the
    **APC routine** and passes `event = 0`. Dropping the APC left every observable
    looking correct, which makes it *harder* to find than an outright failure. Two
    sub-traps: the APC routine's low bit is a kernel flag, not part of the address,
    and its `IO_STATUS_BLOCK` argument is a **guest** address, not the host pointer
    the marshalling layer hands you.
47. **A `kHighFrequency` export can be the one that matters.** The read above is
    invisible in A1 and in every other level-3 capture; only A5 shows it. A capture
    set needs at least one high-frequency arm or its quietest exports are unfalsifiable.

48. **"Fill your out-parameters" is only safe on top of a correct signature.** Writing
    an out-parameter a call does not have turns a harmless leftover register into a
    wild store — and it fails inside your own kernel, in the very place the rule
    exists to protect. `NtClearEvent` takes one argument; `NtSetEvent` takes two.
    Check arity against a capture before adding the write.
49. **Device-struct offsets are per-title; the image states its own.** The previous
    port reads the GPU ring's kicked-write-pointer mirror at `dev+11088`; Case Zero's
    is at `dev+10956`. Both are recoverable from the single store to `CP_RB_WPTR`
    (`0x7FC80714`) in the image. Copying the other port's number reads an arbitrary
    field as a ring position.
50. **Against an intermittent failure, an arm is not a measurement — a rate is.** A
    single-run A/B over a nondeterministic crash confidently named the arm that
    happened not to fire, and produced a clean, decisive, wrong conclusion. Re-run at
    a longer duration, every arm survived. Gotcha 7's other half.

51. **A rate measured once is a fact about that afternoon.** The same binary and drive
    measured ~2 crashes in 10 one session and 6-7 in 10 the next. Re-establish the rate
    whenever anything around an intermittent fault changes — including the instrument
    you added to watch it.
52. **Three page-0 policies turn a null fault into a diagnosis.** Trap-everything says
    only "something touched null". Allowing null *reads* (the console's behaviour) and
    re-running says whether it was a read or a write, and mapping page 0 fully says
    whether the pointer was merely null or genuinely wild. Here the faulting address
    walked `00000000` -> `00000002` -> off the end of the guest space, which rules out
    the benign console-tolerated null read and names a bad base pointer instead.

From finding 28 (the XAM surface):

58. **A stubbed *query* can steer the whole boot, and one value can be worth dozens
    of implementations.** `XamGetSystemVersion` is a feature gate in seven places;
    as an honest-failure stub it returned `0xC0000002`, which compares ABOVE every
    threshold, so the title took the dynamic-import branch at all seven and asked for
    XAM ordinals we do not have. Returning 0 — "this system does not have those newer
    entry points" — is the truthful answer AND the one the capture shows. Raise a
    version gate only together with the exports it unlocks.
59. **A predicate-shaped import has no honest failure value — gotcha 5's blind spot.**
    `RtlCompareStringN` returned `STATUS_NOT_IMPLEMENTED` and the guest tested it with
    `cmpwi r3,0`, so `0xC0000002` was not an error it could notice: it was a valid
    answer meaning "not equal". Every string comparison in the title silently returned
    "different". When a return value is a comparison or a boolean, "fail honestly" is
    not available and implementing it is the only correct option.

From findings 30-32 (the profile, the version gate and the notification queue):

60. **When the capture cannot show a structure, the guest's own walk of it can.** No
    Xenia log records what an import *wrote*; it prints pointer arguments as they were
    before the call. But the code that reads the buffer back names every offset —
    `sub_825E4E88` gave us `+16` = setting id, `+32` = value, stride 40 — and the sizes
    the title itself computes (3 settings -> 0x80, 2 -> 0x58) pin the header at 8
    bytes. Two witnesses, neither of them a guess about the SDK.
61. **A predicate stub is loudest where it is quietest.** `XNotifyGetNext` returning
    `0xC0000002` reads as TRUE, so the title believed a notification had arrived on
    every one of ~10,000 polls a boot and read the id and param out of stack slots the
    stub never touched. Gotcha 59's rule again — and the corollary is that a
    predicate's out-parameters must be filled on the FALSE path, which is the path
    taken thousands of times.
62. **An inherited constant stops being free the moment the image branches on it.**
    `XboxKrnlVersion` was 2.0.14448.0 in both template ports; Case Zero's rumble path
    tests `build < 5611` and Xenia's config says 1888, so the capture takes a legacy
    branch our value skipped. Matching the capture is what a gate means. And the
    change was A/B'd rather than asserted: 82/85/82 vs 85/82/85 over three runs each,
    i.e. no measurable difference — one arm per value would have "proved" a win that
    is not there (gotcha 50).

From finding 33 (the position-57 divergence, closed — and it was not the DVD cache):

63. **A probe answers the question you point it at, and pointing it downward
    confirms the symptom.** Our boot probed `\Device\Image`, then
    `\Device\Harddisk0\partition0`, then compared a path against `"cdrom0:"` — a
    coherent DVD-cache trail, every reading accurate, and the whole subsystem
    innocent. What found the cause was walking OUTWARD to the first caller whose
    behaviour differs from the capture. The cheapest form of that question is not
    "why did this fail" but **"does hardware even get here"**, and one grep of A1
    answered it: hardware never opens `\Device\Image` at all.
64. **A one-instruction tail-call thunk hides an import from its caller's
    disassembly.** `sub_825D7A50` is literally `b XamContentGetLicenseMask`, so the
    deciding branch reads as `sub_825D7A50()` and the import name appears nowhere near
    it. When a call site's callee is a 4-byte function, look through it.
65. **When a predicate's polarity will not hold still, print it.** `sub_82829098` has
    four routes to a zero return and two of them are failure paths; three static reads
    gave three answers. One instrumented run gave the right one. Printing costs less
    than reading it twice and cannot be wrong.

From finding 34 (the XAM message/task block):

66. **A dispatcher's real interface is the set of messages its callers can send, and
    that set is in the image.** Replaying the `r3`/`r4` setup before all 18
    `XMsgStartIORequest`/`XMsgInProcessCall` call sites recovered all 25 `(app,
    message)` pairs in one pass — far better than discovering one per run. Then check
    the static surface against a run: the boot sends exactly two, both handled, zero
    unknown. Static scan and dynamic check agree, which neither proves alone.
67. **An implemented import that has never been reached is a prediction, not a
    result.** Eight of finding 34's nine are guest-derived, none is a guess, and none
    has executed. Gotcha 30 applies to code as much as to tests — say which ones ran.
68. **Adjacency in a first-occurrence gate is not causation — check the thread id and
    the line number.** A first-occurrence gate flattens a multi-threaded timeline into
    one sequence, so two positions sit next to each other whenever nothing *new*
    happened in between, however far apart and however unrelated they are. Position 84
    (`MmMapIoSpace`, audio thread, A1 line 54,145) and position 85
    (`XamShowDeviceSelectorUI`, frontend thread, A1 line 111,694) are 57,500 lines and
    two threads apart; calling the first a blocker for the second was wrong. One grep
    for the line number and thread id settles it before any work is planned on top.

From finding 35 (the early `RtlNtStatusToDosError`, closed):

69. **A pseudo-handle is a constant, not an address, and a handle scheme built on
    addresses cannot tell the difference.** `GetCurrentThread()` returns `0xFFFFFFFE`,
    which has bit 31 set and therefore sailed through our "a handle IS the object's
    guest address" check before being rejected as dead. The scheme already excluded
    `0xFFFFFFFF` explicitly — it knew about -1 and not -2, which was the entire bug.
    Any runtime encoding handles as addresses inherits this.
70. **Grep the captures for the offending *constant*, not just the import the
    backtrace named.** `NtDuplicateObject(FFFFFFFE)` occurs twice; the same constant
    reaches `ObReferenceObjectByHandle` **eleven** times, where our identity mapping
    was handing `0xFFFFFFFE` back as an object pointer. Fixing only the site the
    backtrace pointed at would have left the more frequent and more dangerous one
    wrong, and silent.
71. **One displaced import can manufacture several gate windows.** A5 showed three
    real mismatch windows; two of them were the wake of this single early call, and
    fixing it turned them into recognised permutations. Count causes, not windows.
From finding 27 (the null base pointer, resolved — and it was none of the above):

53. **A scanner's own count is not a measurement of the thing it scans for.**
    `find_jumptables.py` reported 232 tables through all of phase 0; the true number
    was 234. A scanner reports what it found and is structurally incapable of
    reporting what it silently rejected — here, two tables whose `cmplwi` bounds check
    the compiler had hoisted outside the search window, leaving no case count. The
    check has to be an independent question asked against the image, which is
    `tools/find_unlowered_switches.py`. Gotcha 3's rule ("a zero is a detection
    failure, not a fact") applies to *every* number a detector prints, not just zero.
54. **An unlowered `bctr` is the third silent defect class, after unrecognized
    mnemonics and dropped branches — and the quietest.** XenonRecomp emits
    `PPC_CALL_INDIRECT_FUNC(ctr); return;`, which calls the case body and then returns
    with no epilogue. Nothing is printed, it compiles, and the case body even computes
    the right answer because it gets the same `PPCContext`. The only symptom is that
    the caller resumes with the callee's r14..r31 — and the crash lands frames away,
    on a value that looks like a null pointer from our kernel and is not.
55. **A tool that recovers missing entry points can turn a loud failure into a silent
    one.** The missed table above should have crashed *at the dispatch*, because
    `PPC_CALL_INDIRECT_FUNC` on a case-label address normally finds nothing and jumps
    to null. It did not, because the coverage oracle had already added those labels as
    functions (gotcha 21), so the dispatch resolved and the damage became invisible.
    Gotcha 21 said adding a case label splits its parent; this is the other half.
56. **An "intermittent" crash can be perfectly deterministic.** Six crashing runs here
    were **byte-identical** — same registers, same host pc, same stack pointers. Only
    *whether the boot reached the site within the timeout* varied. Diff the crash
    reports before theorising about races; it costs nothing and it reframes the hunt.
57. **In a recompiled frame the register dump is stale and the host pc is not.** The
    compiler keeps `PPCContext` fields in host registers between calls, so `r3`/`r4`
    at the fault named the wrong objects entirely. `addr2line` on the RAW host pc gave
    the exact `ppc_recomp.NNN.cpp` line. The same staleness in `ctx.r1` puts one extra,
    already-returned frame on top of the guest back-chain — read frame #0 as advisory.

From finding 36 (the audio driver, and the first fully clean A5 gate):

72. **A callback's argument can be a pointer to the context, not the context.**
    Case Zero's registered render-driver callback is `lwz r3,0(r3); b <body>` — it
    dereferences before doing anything, so a driver that passes the registered
    context straight through hands the body a pointer one indirection too shallow.
    The body then reads its wait objects and its ring buffer out of whatever sits
    there. What proves it is deliberate is the object's own constructor: the
    registration is slot 3 of a SECOND vtable, so its `this` is `obj+4` and the
    `addi r9,r31,-4` that builds the context recovers `obj+0` — while the body
    measures every field from `obj+0`. Body wants obj, registration supplies obj,
    thunk does one load.
73. **A guest-supplied pointer must never be dereferenced without a check, however
    impossible the null looks.** `WaitDispatcher` read `header->Type` unguarded, so
    ANY guest passing a null dispatcher object to ANY `Ke` wait took the host down
    at address 0 — and the crash reporter labels that "outside the guest space, a
    host-side bug", a true statement that names our kernel rather than the guest
    that provoked it. On hardware it would bugcheck, so there is no faithful
    behaviour to copy; there is only the rule. Corollary: do not *poll* an unusable
    object either — this call site is an infinite wait, so polling wedges the thread
    for the life of the process.
74. **An allocation the runtime owns must get out of the guest's way.** Taking the
    XMA context array from the bottom of the physical arena moved the title's own
    447 MB reservation by 20 KB — a change to an unrelated subsystem bought for
    nothing (gotcha 9). A1 shows Xenia doing the opposite: the title gets physical
    0x03D93000 and the context array sits one page past the END of that reservation.
    `AllocPhysical` grew a `topDown` flag.
75. **A duration is part of a gate's configuration, and "not reached" is not "not
    reachable".** At 30 s the A5 gate stopped at position 114 and the XMA path
    looked blocked. The same binary reaches 119 at 90 s. Before theorising about
    what is blocking a gate position, run it longer — and then run it again:
    gotchas 50-51 are usually quoted about crashes, but they are really about single
    runs of anything. How far a multi-threaded boot gets in a fixed wall time is a
    distribution. One 90 s run reached position 84 and the next did not; the rate is
    5 of 7.
76. **The XMA decoder's register file is console knowledge, and this title drives it
    directly.** `0x7FEA0000`, LITTLE-endian (`lwbrx`/`stwbrx` on a big-endian
    machine): `+0x1800` holds the context array's PHYSICAL address, `+0x1A80` is a
    one-bit-per-context kick bitmap indexed `>> 5`. Contexts are 64 bytes and the
    index is `(MmGetPhysicalAddress(ctx) - base) >> 6`, so they must come from one
    contiguous array at the address you published. Publish it big-endian and every
    index is nonsense, silently — the index only ever picks a hardware bit.

From finding 37 (the frontend was waiting for input, and the arm that proved it):

77. **"Finished and waiting for a human" and "stuck" are the same picture from
    outside.** A boot that has reached its title screen shows a steady frame rate, a
    flat file count and an unchanging kernel-call profile — and so does one that has
    deadlocked there. No passive instrument separates them, because the difference is
    not in what the program is doing, it is in what it is waiting for. The only
    discriminator is to supply input and see whether it moves.
78. **An instrument that manufactures progress has to be loud.** `CZ_FAKE_START_MS`
    presses START for the guest, and a run that quietly had it on would show the boot
    advancing past the title screen and invite the conclusion that the import you just
    wrote unblocked it. Off by default, announces itself on every press, and never on
    for a gate run. Same discipline as `CZ_NO_AUDIO_PUMP`, higher stakes: this one
    fabricates *evidence*, not just behaviour.
79. **Re-measure your own premise before acting on it, not just the capture's.**
    Gotcha 13 says a capture request is a hypothesis with a shelf life. So is a note
    in your own status section. "Our run reaches `mainmenu.tex` and stops" was true
    when written and had quietly stopped being the interesting fact: the runtime now
    loads 64 files through to the title screen's 3D scene and renders it at ~34 fps.
    Acting on the stale version would have sent someone hunting a file-loading bug
    that no longer exists.

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
  their headers. `import_call_sites.py` is the one to reach for when implementing a
  kernel import: the capture has no return values, so the guest code that consumes the
  result is the specification (finding 29).
- `docs/` — **`xenia-capture-analysis.md` is the numbered findings ledger and the first
  thing to read**; `big-archive-format.md` is the cracked container format;
  `xtr-decoder.md` is the GPU stream format + the determinism method;
  `bootstrap-2026-08-04.md` is the day-1 findings record,
  `xenonrecomp-upstream-bugs.md` the local recompiler patches,
  `xenia-capture-requests.md` the (unfulfilled) ground-truth requests,
  `runtime-plan.md` the phase plan, `phase1-notes.md` the phase 1 record (what the
  runtime work found that neither the plan nor the kickoff predicted).
- `Xenia logs/` — captures land here (gitignored); keep an index in
  `Xenia logs/Xenia_Run_Content.md`, which **is** tracked.
- `runtime/` — the host runtime, phase 1 in progress. Target is `cz_runtime`; the
  phase 0.2 link gate survives as `cz_runtime --smoke`.
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
    calling the title's callback at 5.333 ms/frame) and the XMA context array +
    its MMIO register file. Finding 36; every structural claim is quoted from the
    guest function that states it.
  - `kernel/{vfs,file_imports}.*` — the file layer. In phase 1 rather than phase 2
    because A1's 22nd distinct kernel call is already an `NtCreateFile` (finding 16).
  - `kernel/import_stubs.cpp` — generated; honest-failure returns, not aborts.
  - `cpu/crash_report.cpp` — the guest state on any fault. Its host pc is the one
    field that is never stale (gotcha 57); `addr2line` it.
  - `cpu/guest_probe.cpp` — argument probes on named guest functions via the alias
    seam, behind `CZ_ARG_PROBE`. Kept as the worked example of tracing a bad value
    back to its producer; it is what closed finding 27.
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

Build the runtime (needs `clang++`; ~90 s on 16 cores for a cold image build):
```
python3 tools/gen_import_stubs.py                 # after any change to the import set
cmake -S runtime -B runtime/build -G Ninja
cmake --build runtime/build -j$(nproc)
./runtime/build/cz_runtime --smoke                # the phase 0.2 link gate, still live
```

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

Runtime instruments, all off by default and free when off:
```
CZ_MEM_TRACE=1     every virtual-memory call with its arguments AND its answer
CZ_FILE_TRACE=1    every open/read, including the not-founds
CZ_WAIT_TRACE=1    name any infinite wait that outlasts 5 s, with guest callers
CZ_CS_TRACE=1      name the owner of a critical section a thread cannot get
CZ_STALL_TRACE=N   every N-th sleep, dump the sleeping thread's guest call sites
CZ_PEEK=addr[,n]   dump guest memory as the XEX shipped it, before any guest code runs
CZ_NULL_PAGE_READABLE=1|rw   null reads succeed (as on console) / page 0 fully mapped
CZ_RING_TRACE=1    the ring words once a second, incl. the MMIO dword we do NOT use
CZ_VBLANK_MS=N     interrupt cadence (default 16); the control for timing symptoms
CZ_PM4_NO_CP_INTERRUPT=1   consume the ring but never raise source 1 (the ISR control)
CZ_PM4_RESYNC=1    scan past a parser stall instead of reporting it (off on purpose)
CZ_PM4_STOP_ON_WAIT=1      stall the ring at an unsatisfied wait, as hardware does
CZ_ISR_TRACE=1     the scratch mirror the guest ISR reads, at each interrupt
CZ_ARG_PROBE=1     the guest-function argument probes in runtime/cpu/guest_probe.cpp
CZ_KCALL_WHO=A,B   dump the guest call stack the first time these imports are called
CZ_AUDIO_TRACE=1   XMA context allocation + every 512th driver frame WITH its peak
                   amplitude, so "the pump runs" and "the game makes sound" stay
                   separable
CZ_AUDIO_FRAME_US=N  the driver frame period (default 5333 = 256 samples @ 48 kHz)
CZ_NO_AUDIO_PUMP=1 register the client but never invoke its callback — the control
                   arm for every claim about driving the audio callback
CZ_FAKE_START_MS=N synthetic START press every N ms. A MEASUREMENT ARM, NOT A
                   FEATURE — it manufactures progress, so it announces itself on
                   every press and must NEVER be on for a gate run (gotcha 78)
```

`CZ_KCALL_WHO` is the companion to the phase gate: the gate says *that* our
first-occurrence order diverges, and the most informative divergences are imports we
call which hardware never calls at all. Only the call site explains those.

Re-derive the save/restore helper addresses:
```
python3 tools/find_save_restore.py assets/game/default_image.bin
```

Read a GPU capture (`tools/xtr.py` is the format; the rest are thin CLIs over it —
see `docs/xtr-decoder.md`):
```
python3 tools/xtr_walk.py stats  "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/xtr_walk.py limits "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/xtr_pm4_census.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" --verify
python3 tools/xtr_determinism.py \
    "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" \
    "Xenia logs/gpu_B1b_boot_repeat/58410A8D_stream.xtr" --labels B1 B1b
```
`--verify` is the only check in the census that *can* fail — always pass it.

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
  Xenia's `dump_shaders`: 455 raw Xenos microcode blobs, already in hand.
- **No Bink** (finding 7). Movies stream through an in-house "Movie Player Object"
  reading `.big` cinematic archives. Grep `.big`, never `.bik`.

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
  Xenia's `dump_shaders` gave us 455 raw Xenos microcode blobs, which is XenosRecomp's
  input, so the renderer is unblocked anyway.

## Current status & next steps

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
files through to `prologue_menu\prologue_z01.big`, rendering at ~34 fps and ~1,982
draws/frame against A1's title-screen ~2,540 — and sits there. Supplying one synthetic
START press (`CZ_FAKE_START_MS`, a measurement arm, never on for a gate) advances the
A1 gate from an 84-deep prefix to **85**, `XamShowDeviceSelectorUI`, five log lines
after the press. 2 of 2 conclusive pairs; the runs that reached the title screen
without a press did not advance. It then stops at position 86 on the phase 2
save-data enumerate stubs — a gap we chose.

Two things fell out. The standing note ("reaches `mainmenu.tex` and stops") had gone
stale and would have sent someone hunting a file-loading bug that no longer exists
(gotcha 79). And about a third to a half of long runs never reach the title screen at
all, stalling with the main thread parked in the renderer's frame fence while holding
two critical sections — a real defect, now the top of the list.

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

Next, in order:

1. **The intermittent load stall** (finding 37) — about a third to a half of long
   runs never reach the title screen, stopping at 47 or 57 files with the main thread
   blocked in the renderer's frame fence (`sub_827CC6A8` waiting on `[obj+2480]`)
   while holding two critical sections two other threads spin on. The other side of
   the fence is `sub_827D3898`, the Draw Thread body. Now the biggest thing between
   us and a reliable boot.
2. **The surviving crash**, guest thread `00000F2C`, `lr=8284B708` / `82829BEC`. Now
   localised: `ppc_recomp.176.cpp:10244`, a `bctrl` in `sub_8284B568` at guest
   `8284B704` where `ctr = [r31+16] = 0` — a null indirect call through a vtable slot.
   The crash reporter's "LIKELY null indirect call" heuristic did NOT fire (it wants
   `si_addr == nullptr` *and* `ctr` inside the image); widening it to `ctr == 0` would
   name this instantly.
3. **Prove the still-unexercised imports** (gotcha 67 — implemented is a prediction,
   not a result). Finding 34's eight remain unrun; `XamTaskSchedule` in particular
   runs guest code on a new thread and never has. Of finding 36's seven, **five run
   and two do not** — both teardown paths (`XAudioUnregisterRenderDriverClient`,
   `XMAReleaseContext`), because the boot never shuts audio down.
4. The save-data layer proper — `XamContentCreateEnumerator`, `XamEnumerate`,
   `XamGetPrivateEnumStructureFromHandle`, `XamContentCreateEx`, `XamContentClose` —
   deliberately left out of finding 34 as the phase 2 file layer.
5. Audio output and XMA decoding (phase 5). The kick bitmap at `0x7FEA1A80` currently
   lands in ordinary flat memory and is inert; a real decoder needs that aperture
   trapped as MMIO or the kick is written and never noticed.

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
