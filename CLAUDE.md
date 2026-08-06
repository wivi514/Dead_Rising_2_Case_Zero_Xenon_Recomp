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

From finding 38 (the load stall — traced end to end, not yet fixed):

80. **A GPU capture records every packet's own length, which makes it an oracle for
    your command processor's arithmetic.** Xenia's `.xtr` writes a
    `PacketStart {base_ptr, count}` per packet, `count` being the boundary hardware
    actually used — inside indirect buffers included. `tools/pm4_packet_lengths.py`
    checks our rule against all 24,527,474 of B1's and prints the opcodes that
    disagree. It cost an hour to write and it retired, in one run, a fix I had
    already made, a buffer-dump analysis that appeared to confirm it, and two
    follow-up theories. Write the oracle before the theory.
81. **The instrument that is missing is the one whose silence you read as health.**
    Our wait trace covered `NtWaitForSingleObjectEx` only. The Draw Thread waits on
    four handles in `WaitForMultipleObjectsEx`, so the single most important wait in
    the title was the one wait the trace could not see, and its absence from the
    stall dump read as "that thread is fine". Half a wait surface answers "who else
    is stuck?" with silence that looks like "nobody".
82. **A thread stuck without being in any kernel wait is spinning in guest code, and
    nothing inside the runtime can see it — it is not calling you.** An outside
    debugger can, but only if its host thread ids can be joined to your guest ones.
    One always-on line per thread (`guest thread tid=... host tid=...`) is what turns
    `gdb -p` from anonymous stacks into named guest functions.
83. **The address a driver REGISTERS with the kernel need not be the address it
    POLLS.** Case Zero registers `block + 0x3C` with `VdEnableRingBufferRPtrWriteBack`
    and dereferences `block + 0` in its free-space wait. Publishing the read pointer
    honestly into the registered slot therefore satisfies nobody. A hardware
    watchpoint on the polled word in a HEALTHY run names its real writer in one hit —
    do that before reasoning about what "should" write it.
84. **A parser that stops early must say so.** "A packet claiming more than the buffer
    holds: stop, do not guess" is the right policy and it was silent, so the defect it
    was detecting looked like nothing at all. What it was dropping was the last packet
    of the buffer — which in this title is the driver's ring-progress fence, i.e. the
    one packet whose loss parks a thread forever.
85. **When a walk desyncs, the position it fails at is data, not the bad packet.** The
    header in our first truncation report was `3F800000` — the float 1.0. Keep a trail
    of the last few packets and dump the untrusted tail, or every report points at an
    innocent dword.
86. **The control for "did my change do this" is the old binary run NOW, not the old
    binary's remembered numbers.** The A1 gate started permuting positions 71-73 in
    half our runs where the committed binary had been 6-of-6 clean, which reads as an
    unambiguous regression. `git stash`, rebuild, re-run: the committed binary does it
    too, and with both binaries built side by side and runs ALTERNATED, the committed
    one permuted 1 of 6 and the new one 0 of 6. Those positions were always
    scheduling-sensitive. Gotcha 51's corollary — and acting on the remembered number
    would have reverted a correct change to fix a defect that did not exist.

From finding 39 (the stall was our own `VdSwap`, and it presented as a parser bug):

87. **A kernel export that writes into a guest-supplied buffer owns the whole
    reservation, not just the part it has content for.** Our `VdSwap` wrote 12 dwords
    of a 64-dword block and returned; the caller advances its write pointer by the
    full 64 whether or not the kernel fills it (`addi r11,r29,256`, `r3` never read),
    so the 52 dwords we left alone were submitted as packets. Command buffers are
    recycled, so those dwords were the *previous frame's* command stream — real
    headers at wrong offsets. Hardware fills them with 52 × `0x80000000`, a type-2
    one-dword no-op, the only encoding that crosses arbitrary dwords uninterpreted.
    Gotcha 5's other edge: a stub that does *part* of a job leaves a hole shaped like
    real data, and it surfaces in a subsystem it was never near.
88. **An oracle for your arithmetic does not clear your inputs, and a clean parser
    gate is not a clean parser.** Two capture-derived gates said our command
    processor was right — every packet length against hardware's own boundaries on
    24,527,474 packets, and every indirect buffer's start address and internal
    boundaries chained on all 28,726 — while it desynced dozens of times a minute.
    Both were true. The bytes were wrong. When every check of a computation passes
    and the result is wrong, stop checking the computation.
89. **A capture is a vocabulary, not just a transcript.** B1 uses only **225 distinct
    packet headers** across 24.5 M packets. Walking a suspect buffer and flagging the
    first header that is not in that set put the desync at one place — immediately
    after `XE_SWAP`, in all six dumped buffers — after six truncation reports had each
    named a different innocent position. A tight vocabulary is a cheap, extremely
    strong classifier, and any format with a capture has one.
90. **Check what a distinctive record *is* before reasoning from it.** B1's single
    zero-header packet, which finding 38 used to reject the zero-as-no-op reading, is
    an `INDIRECT_BUFFER` whose header the trace records as zero — count 2 (the short
    recording 0x3F always gets), an address in word[1], and `IndirectBufferStart` with
    that base as the next command. So the capture contains **no** genuine zero header
    and is silent on the question, which two sessions treated as settled in opposite
    directions. One anomalous record deserves its own identification, not a conclusion.
91. **A change that measurably helps can still be the wrong change.** Reading zeros as
    no-ops walked two of six broken buffers to completion — a real, reproducible,
    partial improvement — because our unwritten tails were zeros wherever the heap
    happened to be fresh. It addressed the *shape* of the corruption, not its source,
    and the A/B over stalls duly showed nothing (3 of 10 vs 4 of 10). Partial
    improvement with no effect on the symptom is a signature worth recognising.

From finding 40 (the 1-in-40 crash, re-measured to zero — and the barrier hole found
on the way):

92. **A barrier that compiles to nothing is a COMPILER bug, not a hardware one, and
    x86's strong ordering is the trap.** XenonRecomp lowered `sync`, `lwsync` and
    `eieio` to `// no op`. For the hardware half that is right on x86-64 — TSO already
    gives every ordering `lwsync` promises — which is exactly why it looks safe. It is
    not: every guest access is a plain C++ load/store through `base`, and a construct
    that emits no code constrains clang not at all, so at `-O2` "fill the buffer,
    lwsync, publish the index" may become "publish the index, fill the buffer". Fix
    them differently or get it backwards: `lwsync`/`eieio` -> `atomic_signal_fence`
    (compiler barrier, no instruction), `sync` -> `atomic_thread_fence` (a real fence,
    because store-load is the one ordering x86 does NOT give).
93. **An inherited crash rate is a hypothesis about a binary that no longer exists.**
    Task #11 recorded 1 crash in 20-40 runs; the same test on the current binary is
    **0 of 20**, because finding 39 changed what a run spends two minutes doing.
    Gotcha 51 says a rate is a fact about an afternoon; this is the stronger form —
    re-measure before you characterise, or you will spend a session hunting a fault
    the code no longer has.
94. **A diagnostic can be silent on precisely the case it was written for.** The crash
    reporter's "LIKELY null indirect call" test required `si_addr == nullptr`, which is
    never true when `ctr` is *zero*: the dispatch-table lookup is never reached, so the
    fault address is whatever the lookup computed. The one shape it existed for was the
    one shape it could not see. Widening it is one line; proving it needed
    `CZ_CRASH_TEST=nullcall`, because gotcha 30 applies to diagnostics as much as tests.
95. **A long rate run leaves behind a free control.** Twenty 120-second boots produce
    twenty complete logs, and every log-based gate you own replays over them for
    nothing. Four hand-run A1 gates said 3-of-4 permuted where the session had earlier
    seen 2-of-2 clean — an obvious regression, and wrong. Gating the 20 saved logs per
    arm gave **13 clean / 7 permuted on BOTH binaries, identically.** Gotcha 86 says
    the control is the old binary run now; this is the cheap way to have run it.
96. **If you have written a tool twice from scratch, keep it the first time.** A PPC
    disassembler over the loaded image was hand-rolled and discarded in two consecutive
    sessions. It is `tools/gdis.py` now. The host toolchain genuinely cannot do this —
    `objdump` has no PowerPC target here, `llvm-objdump` has no `-b binary`, and
    `llvm-mc` loses instruction alignment at the first unknown VMX128 encoding and
    keeps printing plausible garbage after it.

From finding 41 (the critical-section yield spin):

97. **A "yield loop" is a busy loop wearing a polite name, and it bills to the half of
    the profile nobody reads.** `sched_yield()` on an otherwise-idle multicore host
    returns immediately — there is nothing to yield to — so `while (!cas) yield();`
    burns a full core. Case Zero blocks two threads forever by design (on console they
    just sleep), and ours spun: **317% CPU, of which 85 of 106 seconds were SYSTEM
    time.** User-time profiling shows those threads computing almost nothing, which is
    true and is exactly why the waste survived a whole port. Parking them: 121%, 4.5 s
    system. A guest thread the title parks needs somewhere in the runtime to park.
98. **When a wait changes from spinning to blocking, every count-based instrument on it
    goes silent.** The `[csspin]` trace fired every 4 M failed attempts, a fine proxy
    for elapsed time while the loop spun and meaningless once it parks — a parked
    waiter reaches 4 M attempts approximately never, so the trace would have gone quiet
    precisely when a wait got long enough to be worth reporting. Re-express the gate in
    a unit that survives the change: elapsed time. And the replacement stats counter
    printed **nothing across six runs** because its report was gated inside a rare
    branch, which read as "no contention" — gotcha 25 in our own tooling for the second
    time in three sessions.

From phase 3 (the window, the present seam and real input):

99. **A window is a thread, and it is *the* thread — so move the guest, not the
    window.** SDL's video subsystem must be initialised, pumped and presented from
    the thread that created the window, and until phase 3 that thread was busy
    running the guest entry point. The guest is the side that moves, because a
    recompiled title's threads are already an abstraction you own (every other guest
    thread here has always run on a spawned `std::thread`) while the windowing
    system's are not. What the main guest thread must keep is its **order** — still
    the first guest thread created, still the first thread id — because that is the
    property a first-occurrence gate can see.
100. **Route the present through the command stream, not around it.** Presenting from
    inside `VdSwap` would have worked and would have been wrong: findings 38-39 were
    entirely about the gap between "the kernel wrote a packet" and "the command
    processor executed it", and a present seam fed from the kernel export keeps
    counting frames straight through a GPU desync. Driving it from `pm4.cpp` case
    `0x64` means a present can only happen at a stream position the parser actually
    reached, and the window's frame count is the same number the ring trace prints
    rather than a second counter that can disagree.
101. **A packet number is a contract, and both obvious implementations are wrong.**
    XInput changes `dwPacketNumber` only when the state changes, and a title may skip
    re-reading the pad when it has not. Ticking it every poll defeats the field's
    purpose; holding it constant while the buttons change hands the guest a press it
    is *correct* to ignore — and that failure presents as "input does not work" when
    it is really "input works and was properly filtered". Move the number in exactly
    one place, on an actual state change.
102. **Between SDL and XInput only two things are conversions, and no filtering is.**
    The buttons are a rename (`SDL_GameController` is the 360 pad generalised). The
    stick **Y axis is inverted** — get it wrong and the game works perfectly except
    that up is down, which reads as a guest bug — and the **triggers are scaled**
    0..32767 to 0..255. A deadzone is NOT the runtime's to apply: the console hands a
    title raw axes and every title has its own, so filtering here invents an input
    characteristic hardware does not have.
103. **A gate that needs a human is a capture request in disguise.** Phase 3's gate is
    "the boot advances on a *real* press", which by construction cannot be
    self-served: there is no synthetic press that is not the arm the gate exists to
    retire. That makes it the same class of work item as a Xenia capture — it has to
    be scheduled with the operator, and everything that does not depend on it should
    be finished and committed first so the only thing waiting is the press.

From the save-data layer (A1 positions 86-92, built straight after phase 3):

104. **When the capture cannot answer, the title's own SDK wrapper can.** Every
    question about a content enumerator is about a RETURN value, and Xenia logs only
    arguments (finding 29). But this title statically links the XDK's own
    `XamEnumerate` (`sub_825D9460`) and its task body (`sub_825D9358`), and reading
    those two functions gave the entire protocol with no guesses: item size 0x138,
    app id at priv+0, message id at priv+4 (which the guest *checks*), the 32-byte
    message, and the exact HRESULT that means "no more". The corollary is the good
    news: whatever the guest only ever hands straight back to you — here priv's whole
    layout — is YOURS to define.
105. **A stubbed query can make a whole subsystem's data invisible with no error
    anywhere.** `XamGetExecutionId` looks like bookkeeping; it is the save
    enumeration's title-id FILTER. An item whose title id does not match is not
    rejected loudly, it is SKIPPED — so an enumeration of nothing but wrong-id items
    is byte-for-byte indistinguishable from an empty one. Both halves failed here at
    once (the stub never wrote its out-parameter, and the enumerator wrote 0 for the
    id). What separated "the title accepted our save" from "the title threw it away"
    was one line printing `XMsgCompleteIORequest`'s arguments — which A1 supplies
    verbatim to compare against.
106. **The save directory is part of a gate's configuration.** A1 was captured with no
    save present. With one, our boot calls `XamGetExecutionId` between positions 90
    and 91 — a call hardware never made because it had no item to filter — and the
    gate reports a divergence. Nothing is wrong in either run. Same shape as gotcha 20
    (`license_mask` defaulting to the trial): run the A1 gate with an EMPTY save root,
    and read a leftover save as a configuration difference, not a regression.

107. **"The capture's next call" is not automatically the next thing to implement.**
    A1's position 93, `KeQueryBasePriorityThread`, has been implemented since phase 1.
    Its only game-side caller is a work-queue drain that runs **only on the failure
    path** (`cmpwi r30,0; bge <skip>`), reached once in an entire hardware boot. So
    matching it means reproducing a failure hardware had, not building a feature.
    Before planning work against a gate position, find its call site and ask what
    CONDITION reaches it — the name says nothing about that.
108. **"Never entered" and "entered constantly with nothing to do" need a probe, not a
    reading.** One `PPC_FUNC` hook on the suspect function and one on each of its
    callers answered in two runs what the call graph could only make plausible: the
    drain is never entered, and three of its seven callers are. `CZ_QUEUE_PROBE` in
    `runtime/cpu/guest_probe.cpp` is the worked example.
109. **A log line that is capped is not a count.** `NtCreateFile` successes are printed
    only for the first 64 (`n < 64 || FileTrace()`) while failures are printed always,
    so "the boot opens 64 files" — quoted in this project since finding 37 — is the
    cap, not the number. Check the emitter before quoting a number off a log, the same
    way gotcha 25 says to check it before believing a zero.

From phase 5 (the renderer; details in `docs/phase5-notes.md`):

110. **A field's width is part of the field, and the symptom lands in another
    subsystem.** A vertex fetch constant's dword1 is `endian:2, size:24, unused:6`, and
    reading the size as `d1 >> 2` with no 24-bit mask turned an 85-dword stream into
    67,108,885 dwords. What that produced was not a wrong picture: it was 2,225,992
    draws reported as "vertex stream outside the physical arena", which reads as an
    addressing or memory-map bug three layers from the actual mistake.
111. **When two tools disagree about an index, the authority is neither of them — it is
    the data structure being indexed.** A vfetch's constant index is
    `const_index * 3 + const_index_sel`, which gives 95/94/93 for the exact shaders
    Xenia's disassembly prints as vf0/vf1/vf2. Reading either tool harder cannot settle
    it. Dumping the POPULATED fetch slots at a draw settles it in one run: the guest
    writes slot 0, the shader that asks for slot 0 by our reading finds it, and Xenia's
    disassembler is displaying `95 - index`.
112. **The frame is a resolve DESTINATION, not the render target.** One title-screen
    frame issues ~20 resolves into the same EDRAM — a 1280x720 main pass, a
    640x360→32x1 downsample pyramid, glyph atlases — and exactly one whose destination
    is the address `VdSwap` named. Presenting the render target instead shows every pass
    overlaid in the top-left at its own size, which looks EXACTLY like a viewport
    scaling bug and is not one: every viewport in the stream is correct. The register
    that supplies the missing identity is `RB_COPY_DEST_BASE`.
113. **Passes communicate through guest memory, so a resolve has to become a
    texture.** Every intermediate resolve here sets `RB_COPY_CONTROL`'s two clear bits
    and the front-buffer one does not — i.e. each pass wipes the EDRAM behind itself.
    A consumer therefore reads its input from the resolve's destination ADDRESS, and a
    renderer that never wrote those pixels anywhere serves it whatever the guest's
    allocator left there. Keying a host image on the destination address and serving
    fetches from it is the whole mechanism; writing the pixels back to guest memory
    would mean tiling them so the consumer could untile them again.
114. **A snapshot must not be cached on the fetch constant.** Its contents change every
    frame while its fetch constant does not, so the ordinary texture cache would freeze
    the first frame's version of that surface forever.
115. **Build the shader cache from YOUR OWN dump, not the emulator's.** The cache key is
    a hash of the microcode, and the emulator dumps it with the emulator's idea of where
    the shader ends. Any disagreement about the length is not a slightly wrong picture —
    it is a total, silent cache miss. Dumping from our own `IM_LOAD` handler makes the
    key agree by construction, AND turns the emulator's dump into a free oracle: 120 of
    our 121 boot-era blobs are byte-identical to A1's, which is the first check this
    port has ever run on that packet's size field.
116. **A capture's blob count is a FILE count.** "455 raw microcode blobs" is 455 files;
    A1's 120 are a strict subset of A2's 335, so there are 335 distinct shaders. Two
    documents quoted the file count as a shader count for a whole phase.
118. **An opcode's FREQUENCY is a statement about the renderer's architecture.**
    `SET_BIN_MASK_LO` has been recorded since phase 1 as this title's most frequent
    type-3 opcode — 2,353,460 of B1's 8,283,322 — and treated as a predication detail
    to get right. It was telling us the thing that mattered most: the Xbox 360's EDRAM
    is 10 MB and a 1280x720 colour+depth target does not fit, so Case Zero renders its
    scene in **two 640-wide tiles** into a 640-pitch EDRAM surface and resolves each
    into its half of one 1280x720 destination. 930 draws and 494,667 vertices a frame
    were being rendered and thrown away for want of that fact.
119. **A copy's SURFACE and its REGION are different registers.** `RB_COPY_DEST_PITCH`
    is the destination surface; `PA_SC_WINDOW_SCISSOR` is the part of it this pass
    covers, in screen coordinates. Copying the surface's extent when the region is a
    tile puts every pass's content in the top-left corner at assorted sizes — which
    reads as a viewport scaling bug, and survived one whole correct-but-incomplete
    diagnosis (gotcha 112) before the draw-count-per-pass trace exposed it.
120. **Count the draws PER PASS, not per frame.** It is the number that separates "this
    pass rendered nothing because it had no draws" from "this pass had 930 draws and
    produced black" — two completely different investigations that look identical in a
    snapshot. Adding it reframed the entire hunt in one run.
121. **Tiles of one surface must share a key.** The second tile's `RB_COPY_DEST_BASE` is
    pre-offset into the SAME allocation: `06BF8000 - 06BE4000 = 0x14000`, exactly the 20
    macro-tiles that 640 pixels of a 4-byte tiled surface occupy. Keyed on the raw base,
    one surface looks like two and a consumer fetching its real base gets only the left
    half.
122. **`numFormat=integer` is not a shader-side detail.** A normalized vertex format
    divides by the type's range, so an integer 32 arrives as 0.125 and a shader that
    `floor()`s it to index something reads element 0 every time. Vulkan's
    USCALED/SSCALED are exactly the missing concept — an integer delivered as its own
    value into a FLOAT input, which a `*_UINT` format would not be.
123. **Size a constant buffer from the register the guest writes, not from a tool's
    documentation.** XenosRecomp's README says the pixel-shader constant window is 224
    float4; the shaders it GENERATES read up to `c255`. Our 224-register buffer meant
    `c255` loaded 512 bytes past its end. Case Zero's scene shaders use c255 as the
    tone map's scale and bias in their FINAL instructions, so a wrong c255 does not
    tint the scene — it collapses every pixel to a constant. The guest says the true
    size in `SQ_PS_CONST`: base 256, size 255, i.e. 256 registers.
124. **A per-(vs, ps) draw census plus the capture's disassembly is the pair that
    localises a shading bug.** The census says which shader does the pass's work; the
    disassembly beside every blob in the capture says what that shader was supposed to
    compute. Neither is useful alone, and together they took "the scene is flat" to a
    named constant register in one run.
125. **`g_SwappedTexcoords` corrects something the RUNTIME does, not the guest.**
    Dword-swapping a vertex stream is right for 32-bit components and transposes the
    halves of every 16-bit pair, so a `16_16` attribute arrives YX. The shaders un-swap
    it when the runtime sets the matching bit — and a mask left at zero silently swaps
    the components of every 16-bit vertex attribute in the title.
126. **An experiment consistent with BOTH hypotheses has tested neither.** The first
    fetch-slot probe (gotcha 111) caught a shader that asked for slot 0 twice while
    both slot 0 and slot 95 were populated — so it was consistent with either reading,
    and it still produced a confident answer. The unambiguous version is an arm that
    inverts the convention and looks at the result: inverted, the scene renders 0.0%.
    The conclusion was right; the evidence for it was not what it appeared to be.
127. **A metric taken from an ANIMATED scene is not a metric.** Case Zero's title
    screen renders a live 3D background (capture E says so outright), so "the scene
    surface is N% non-black at frame 600" is a different camera angle every run. A
    single-run A/B on it produced a confident "this change made it worse" that
    alternating 3-against-3 showed was pure noise — 100.0/64.1/97.5 against
    64.4/94.8/79.6. Gotchas 50, 51 and 86 in a new place, and quoting them all over
    this project did not prevent it. Large structural changes (0% -> 70%, 3 colours ->
    848) are still readable; anything smaller needs a pinned camera or B1's own
    per-era aggregates.
128. **A hand-patched shader cache is a five-minute experiment.** `CZ_SHADER_SPV=<dir>`
    points the runtime at any cache, so "is this generated line doing what it looks
    like?" can be answered by rebuilding one shader with the line changed and running
    the same binary. It retired the `sges` idiom (`abs(x) >= 0.0`, the compiler's
    "set w = 1") as the cause of exploded geometry: forcing it to a literal 1.0 changed
    the picture by 0.1%.
129. **A metric that passes perfectly and tests nothing is worse than a noisy one.**
    Aligning rendered frames by content — the method that works on the capture pair —
    gave 257 of 257 frames bit-identical with zero delta, and it was 257 copies of a
    BLACK IMAGE. For a scene animated off wall-clock time, exact alignment selects for
    stasis, which is precisely the content least able to reveal a difference. The same
    100.0% came back for arms that visibly change the picture. The noisy version at
    least did not invite belief.
130. **Aggregate over the era; and use the MEDIAN, not the mean.** Several hundred
    frames sample the whole animation cycle, so the median is stable (1.36 pp over five
    runs of one binary) while the mean is not (64.3–70.8 over the same runs) — the mean
    is pulled about by how long a run spent in each part of the cycle.
131. **Measure the surface the defect lives on, not the one you present.** The first
    version of the metric compared the presented frame, which at this title screen is
    mostly UI: a change touching 476,858 draws a run moved it 0.1 pp. It could not see
    the defect it was built for.
132. **A threshold derived from the runs being compared cannot fail.** It widens to
    accommodate whatever difference is present. `frame_compare.py` quotes its 1.5 pp
    band as a constant measured once from five baseline runs.
139. **A probe that fires on the first occurrence samples the BOOT.** `CZ_VK_DRAW_PROBE`
    reported "every pixel-shader constant is zero" for a post-processing shader whose
    constants are a perfect Gaussian kernel by the title screen — because a shader's
    first three draws happen before the guest has uploaded the constants it will use.
    Watching the register itself settled it in one run (no zero writes at all after
    frame 400). Bound every state probe by FRAME as well as by count; this is the same
    trap as bounding it by vertex count and finding only the small early meshes.
From phase C part 2 (the movie deadlock, and how it was finally named):

141. **When you interpose on a stream, the question for each packet is not "can I
    emulate this?" but "WHO READS IT?".** Redirected emission is right for content,
    whose only consumer is your renderer, and wrong for anything the title itself
    reads back. Case Zero's callback hand-off is an arming of two GPU scratch
    registers, three `WAIT_REG_MEM`s that hold the GPU until that mirror is visible
    in memory, an `INTERRUPT`, and a re-poison — and the ISR reads the callback out
    of GUEST MEMORY at delivery time, so the whole thing is an ORDERING contract
    against the CP and against the guest's own stores. Four increasingly clever
    emulations of it all raced the poison. Emitting the block into the stream whose
    reader owns it took two dozen lines and no semantics at all.
142. **A probe is worth writing when it can run on BOTH arms — that is what makes it
    an oracle rather than a description.** Three sessions of hypotheses about
    interrupt races ended with one flag (`CZ_FENCE_PROBE`) hooking the five functions
    that produce the fence/callback protocol, run once on the PM4 control arm and
    once on the D3D arm, and diffed: the control arm's ISR delivered `sub_8284AAD0`
    138 times and ours delivered it ONCE. The missing call had never even been named
    before that line printed. Instrument the seam both arms share, not the arm you
    suspect.
143. **A "no callers / no writers" scan result is a statement about STATIC form, not
    about behaviour, and one hardware watchpoint settles it.** `dev+0x2B04` is
    incremented by a `stw` a scan finds instantly and decremented by nothing the whole
    8.8 MB image contains — because the decrementer holds the pointer in a register.
    `gdb -p`, `watch *(unsigned int*)((char*)g_memory.base + <va>)`, two continues,
    two backtraces: `sub_8284A960` under the worker's token interpreter, in one run.
    (Read the values as big-endian: guest 2 prints as 33554432.) The same trick names
    the caller of an indirect-only function.
144. **Name a function by what its ARGUMENT does, not by its shape.** `sub_82846288`
    was recorded for a phase as "fence/throttle-shaped" on the strength of its call
    rate and its comparison of two cursor fields. It is the callback armer, and the
    thing that said so was `tools/gdis.py --find-uses` on the ADDRESS of the callback
    it arms — the constant `0x8284AAD0` appears at exactly three call sites, all
    passing it as an argument to this function. A 32-bit constant is never one
    instruction, so grepping the image for it misses every one.

145. **A spin on a counter is a claim about a VALUE, so print the value.** Two
    sessions described `sub_82846210` as "waiting for the async segment count to reach
    zero" and planned work around who was failing to decrement it. The word held
    **-552**, and the loop tests `!= 0`: it was not slow, it was unsatisfiable, and the
    real imbalance ran the other way (6 increments against 18,900 decrements). One
    `%d` in a probe line reframed the whole hunt. The corollary is that a counter
    described as "never returns to 0" deserves the question "in which direction?"
    before anything is built on it.
146. **A fence word pinned to a CONSTANT while its emitted counter climbs is replay,
    not latency.** `[wb+0]` froze at `0x00000795` at exactly the frame the ring went
    runaway, and the obvious reading — the GPU has fallen behind — is wrong in a way
    that sends you to the renderer. A stream being re-executed keeps rewriting the same
    stale `EVENT_WRITE` value, so the word does not lag, it repeats. Lagging and
    repeating look identical in a single sample and completely different in two.
147. **Redirected emission moves BOUNDARIES, not just packets — and a boundary has a
    reader too.** Part 2's rule ("emit where the reader lives") was stated about
    packets. A segment's extent is `[dev+0x3B20] .. [dev+0x30]+4`, and the content that
    would normally separate a hand-off block from the next segment boundary is exactly
    what phase C redirects away. So the block can end up inside the segment its own
    wake-up resubmits, which is a loop with gain one and no seed.
148. **A retired hypothesis is retired against a binary, and moving code can hand it
    back its premise.** `CZ_PM4_STOP_ON_WAIT=1` was measured and retired for the arm
    block's trailing `WAIT_REG_MEM` — while that block was in the private scratch,
    where the walker's own wait handler never stalls and the flag *could not apply to
    it*. The measurement was honest and the conclusion was about nothing. Re-run it the
    moment the thing it gates moves. Gotcha 13's shelf life, applied to our own notes.
149. **The cheapest way to ask "who reads this?" is to label every address you print.**
    Phase C's whole difficulty is that one dword can live in the ring (read by the
    title) or in our private scratch (read only by us), and a bare `%08X` cannot tell
    you which. Tagging every cursor in the probe with ` SCRATCH` turned three separate
    mysteries into one glance — and it is what found that 405 of 405 callback armings
    were in the wrong stream, and that the frame-end submit was reasoning about a
    buffer the title never sees.

From phase C part 4 (the replay is the flywheel, not the fault):

150. **A feedback loop with gain one is a PIPELINE while its pointer advances and a
    runaway the moment it stops — so "this loop regenerates its own wake-up" is not
    by itself a defect.** Case Zero's GPU→ISR→worker→ring hand-off does exactly that
    on hardware and on our PM4 control arm: each frame's arm-carrying segments produce
    the interrupts that drive the NEXT frame's walks, because the guest arms with a new
    token buffer every frame. Three sessions read the regeneration as the bug. It is
    the design; what breaks is that the phase C arm's guest stalls, the pointer stops
    moving, and the same buffer is walked forever. Before theorising about a loop, ask
    whether its state advances — and the cheapest form of that question is to print the
    ITERATION'S IDENTITY (here the token-buffer pointer) and flag repeats.
151. **A conditional inside an instrument can make every measurement of it a
    measurement of nothing.** `CZ_PM4_STOP_ON_WAIT` was gated on `depth == 0` — the
    ring itself — and every wait it was aimed at lives inside an INDIRECT BUFFER, i.e.
    at depth ≥ 1. It was measured and retired TWICE, once on the grounds that the
    packets were in the wrong stream and once on the grounds that they had moved to the
    right one, and on both occasions the flag could not fire. Gotcha 148 says a retired
    hypothesis is retired against a binary; this is the sharper form — read the arm's
    own code before quoting what its absence proved. Two symptoms to recognise: an arm
    whose "on" run is byte-identical to its "off" run, and an arm with no counter
    saying how many times it engaged.
152. **A stall below the top of a nested walk needs a resume plan, or the retry is a
    replay.** Unwinding an indirect buffer and re-entering it next tick re-executes
    every packet before the stall — including the arm and the INTERRUPT the stall
    exists to hold back — so the fix makes the symptom. Record (buffer, dword) per
    depth and resume there; and do not count a deliberate stop as a truncation, or the
    one live alarm finding 39 left behind sits permanently nonzero.

From phase C part 5 (the missing CPU side of the hand-off was a display controller):

153. **A register the title only ever READS cannot be found by hunting for its
    writer.** Every previous hunt in this project started from a store — the ring kick,
    the counter's decrementer, the callback armer — and `CZ_PM4_MEM_WATCH` is a
    store-side instrument by construction, so it answered "who writes this word" with
    3,089 writes of the value 1 and complete silence about the zero. The zero came from
    the CPU, behind an MMIO **load** at `0x7FC86544` that appears exactly once in 8.8 MB.
    What found it was scanning for the aperture BASE (`lis rX,0x7FC8`) and enumerating
    every load and store built on it: this title's entire GPU MMIO surface is **five
    instructions**, four registers, and only one of them is read.
154. **One hardware status bit can gate an entire subsystem, and its absence looks
    exactly like nothing at all.** Behind that bit is the guest's own swap queue —
    16 records of `{surface, due tick}`, a vblank tick, a walker — and with the bit
    clear the walker has no caller, the tick stays 0 for the life of the process, and
    the queue grows to 1,540 entries with **one** ever retired. No error, no log line,
    no wait that visibly fails; the only symptom is a GPU wait somewhere else that
    never completes. Fable 2 hit the identical gate at the identical address, which is
    the real lesson: when a hand-off has a CPU side you cannot find, read what the
    previous port did about the display controller before theorising about packets.
155. **An interrupt can be addressed to a SET of hardware threads, and a
    single-threaded ISR acknowledges exactly one of them.** Case Zero's arm block
    writes a six-bit CPU mask; the ISR clears `1 << PCR[0x10C]`; the block's trailing
    `WAIT_REG_MEM` holds the command processor until the whole word is zero. The mask
    DEFAULTS to CPU 2, which is where our pump has run since phase 1 — so the common
    case was right by accident and the one caller that names CPU 4 deadlocked the ring.
    A runtime with one ISR thread has to take the interrupt once per named CPU,
    reporting each in its PCR; anything else is one acknowledgement short. And the ISR
    body is per-CPU too (the D3D job ring is at `dev + cpu*0x6C + 0x2C40`), so the CPU
    number decides *which worker* gets the kick, not just who acknowledges.
156. **A brake and its release are one mechanism; half of it is worse than neither.**
    The CP stalling at `WAIT_REG_MEM` and the vblank handler clearing the word it polls
    are this title's entire frame pacing. With only the release, the title free-runs and
    its flip queue overflows (head 27, tail 1,074). With only the brake, the ring parks
    at frame 1. With both, head equals tail, one record retires per frame, and the boot
    reaches the title screen with `truncated=0`. Two changes, each of which measures as
    a regression on its own.

140. **"Which pass consumed it" is not a question a global counter can answer.** The
    renderer counted 450,488 texture fetches served from resolve snapshots and could
    not say whether the pass that writes the front buffer was one of them — which was
    the entire question. A per-pass record turns a frame into a DEPENDENCY GRAPH, and
    that graph localised a dead post-processing chain to its first broken link in one
    run.
137. **A negative control is what stops a comparison tool inventing results.** The
    frame-signature tool judged its confidence as "the gap between the best and second
    orientation, as a FRACTION OF THE SPREAD". On two real cases it was right. On the
    negative control — the title screen against the ESRB card, two unrelated images —
    it reported "THE FRAME IS TRANSFORMED: flip-horizontal, 52% of the spread", because
    four nearly equal correlations make a tiny gap a large fraction of a tiny spread. A
    SMALL SPREAD IS NOT EVIDENCE OF A CLEAR WINNER; it is evidence that nothing
    discriminates. The fix is two gates — an absolute correlation floor and an absolute
    gap — and the general rule is that a comparison tool needs a pair of inputs that
    SHOULD NOT match, or its confident answers are unfalsifiable.
138. **Normalise by the geometry, not by the content.** Cropping to the non-black
    bounding box looked like the obvious way to compare frames of different sizes, and
    it destroyed the comparison: capture E's shots are mostly black, so the bbox finds
    the LOGO rather than the screen, and two images then get squashed to the grid by
    different amounts. Cropping both to the largest centred 16:9 rect instead — the
    game area's real geometry, checkable on E4, the one shot bright enough to reveal it
    — took the correlation from 0.067 to 0.947.
136. **A texture's component SWIZZLE is runtime data, so the runtime must apply it.**
    The Xenos fetch constant carries it in dword3; a shader compiled without the fetch
    constant cannot bake it in. Ignoring it makes a single-channel font atlas sample
    alpha as a constant 1.0, so every glyph is fully opaque and text renders as solid
    blocks of the right size in the right place — which reads as a font bug rather than
    a texture-decode one. Free to fix: a VkComponentMapping on the image view.
135. **An aggregate over pixel VALUES is blind to a transform of the picture.** The
    whole frame was rendered vertically mirrored for the entire phase — a Xenos vertex
    shader emits D3D-convention clip coordinates (+y up) and Vulkan's NDC is +y down —
    and not one instrument here could see it, because a flip preserves coverage, mean
    luminance, distinct-colour count and the full histogram EXACTLY. The purpose-built
    A/B metric scores a flipped frame as identical to a correct one. It took an
    operator looking at the Blue Castle Games logo and saying "that is upside down".
    Catching a transform needs a REFERENCE (the E screenshots, or eyes), never a
    statistic — and this is also why "the content is in the upper-left corner" was
    recorded three times without being diagnosed.
133. **One frame of an animated scene is ONE SAMPLE — and that applies to LOOKING, not
    just to measuring.** Three runs of an identical binary and an identical shader
    filter produced three completely unrelated pictures of this title screen, one
    nearly black. A per-shader bisection built on single snapshots duly localised the
    exploded geometry to a shader whose control — the same shader recompiled unmodified,
    byte-identical SPIR-V — renders just as cleanly. A picture feels like direct
    evidence in a way a percentage does not, which makes this the easier trap of the
    two, and it is the THIRD claim this phase had to retract to the same cause.
134. **Before debugging an animated scene visually, make it deterministic.** A pinned
    camera, or a guest clock advanced per frame rather than from the host TSC. Without
    it a bisection cannot converge, because its evidence is resampled every run.
117. **The picture is the one claim that needs an image, so make it self-servable.**
    Every other gate in this project is a log diff. Dumping frames AND every resolve
    snapshot from a headless run is what turns "does it look right" from an operator
    task into something checkable in the session that caused it — and the per-snapshot
    dump is the only instrument that can tell an early wrong pass from a late one,
    because the frame is the last link and a wrong frame is consistent with both.

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
- `docs/` — **`xenia-capture-analysis.md` is the numbered findings ledger and the first
  thing to read**; `big-archive-format.md` is the cracked container format;
  `xtr-decoder.md` is the GPU stream format + the determinism method;
  `bootstrap-2026-08-04.md` is the day-1 findings record,
  `xenonrecomp-upstream-bugs.md` the local recompiler patches,
  `xenia-capture-requests.md` the (unfulfilled) ground-truth requests,
  **`d3d-translation-plan.md` the renderer-architecture pivot (2026-08-06): plan,
  recon, licensing, and the per-phase build-out records — the first read before any
  renderer work**, with `d3d-kickoff.md` / `d3d-phase-c-kickoff.md` /
  `d3d-phase-c2-kickoff.md` / `d3d-phase-c3-kickoff.md` /
  `d3d-phase-c4-kickoff.md` /
  `d3d-phase-c5-kickoff.md` /
  **`d3d-phase-c6-kickoff.md` (current)** the hand-offs,
  each superseding the last,
  `phase5-3d-plan.md` the superseded PM4-side plan for the 3D background (its Step 0
  instrument and Step 1 findings survive),
  `runtime-plan.md` the phase plan, `phase1-notes.md`, `phase3-notes.md` and
  **`phase5-notes.md`** the
  per-phase records (what the runtime work found that neither the plan nor the
  kickoff predicted), `phase1-kickoff.md` / `phase3-kickoff.md` /
  **`phase5-kickoff.md`** the per-phase hand-off prompts. **Read
  `docs/phase5-kickoff.md` before starting phase 5.** Each kickoff's most valuable
  section is its list of the parts of that phase that **already exist** and would
  otherwise be rewritten from the plan text — write that section for every future
  phase.
- `Xenia logs/` — captures land here (gitignored); keep an index in
  `Xenia logs/Xenia_Run_Content.md`, which **is** tracked.
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
    calling the title's callback at 5.333 ms/frame) and the XMA context array +
    its MMIO register file. Finding 36; every structural claim is quoted from the
    guest function that states it.
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
boot dump (which is the authority on the byte range, because the cache key is a hash of
it — gotcha 115):
```
python3 tools/xenia_ucode_to_cache.py \
    "Xenia logs/A1_boot_title_fullgame/shaders" \
    "Xenia logs/A2_gameplay_stillcreek/shaders" /tmp/ucode      # 335 distinct
(cd runtime/build && CZ_NO_WINDOW=1 CZ_SHADER_DUMP=/tmp/ucode ./cz_runtime)  # +1 of ours
tools/build_shader_spv.sh /tmp/ucode assets/shader_spv          # 336, zero failures
```

Build the runtime (needs `clang++`, **SDL2 and Vulkan**; ~90 s on 16 cores for a cold image
build). SDL2 is required rather than optional-with-a-fallback, because a build that
silently lost its window would look exactly like a run whose input stopped working;
`-DCZ_WINDOW=OFF` is how you say "headless on purpose" out loud. Vulkan is required for
the same reason and is safe to require, because the renderer is off at RUN time unless
`CZ_VKDRAW=1`:
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
CZ_CS_TRACE=1      name the owner of a critical section a thread cannot get, every 4 s
                   of waiting. Gated on ELAPSED TIME, not on a spin count — the count
                   version fell silent the moment contended waits started parking
CZ_CS_STATS=1      every 100,000 critical-section enters, how many were contended and
                   how many reached each backoff phase. The instrument for the only
                   risk finding 41 carries (latency on ordinary locks): 2 of 1.6 M
CZ_CS_NO_BACKOFF=1 restore the pure yield spin RtlEnterCriticalSection used to do —
                   the same-binary control arm for every claim about the backoff. With
                   it on, the two threads the title blocks forever burn a core each
CZ_STALL_TRACE=N   every N-th sleep, dump the sleeping thread's guest call sites
CZ_PEEK=addr[,n]   dump guest memory as the XEX shipped it, before any guest code runs
CZ_NULL_PAGE_READABLE=1|rw   null reads succeed (as on console) / page 0 fully mapped
CZ_RING_TRACE=1    the ring words once a second, incl. the MMIO dword we do NOT use
CZ_VBLANK_MS=N     interrupt cadence (default 16); the control for timing symptoms
CZ_PM4_NO_CP_INTERRUPT=1   consume the ring but never raise source 1 (the ISR control)
CZ_PM4_RESYNC=1    scan past a parser stall instead of reporting it (off on purpose)
CZ_PM4_STOP_ON_WAIT=1      stall the ring at an unsatisfied wait, as hardware does.
                   Until phase C part 4 this was gated on `depth == 0` and therefore
                   could not affect a single one of this title's hand-off waits, all
                   of which are inside INDIRECT BUFFERs — so both of its retirements
                   measured a no-op (gotcha 151). It now stalls at any depth and
                   RESUMES at the recorded dword next tick rather than re-walking the
                   buffer. Part 4 measured it parking at frame 1 on a WAIT for
                   `mirror+4` to read zero; part 5 supplied both missing writers (the
                   display-controller gate and the per-CPU acknowledge) and with them
                   the SAME flag runs a frame-paced boot to the title screen with
                   truncated=0. Still off by default until it has a rate rather than a
                   run behind it
CZ_NO_VBLANK_GATE=1  do NOT assert bit 0 of the display controller's gate at
                   0x7FC86544 — i.e. the pre-part-5 runtime, in which the guest's own
                   vblank ISR never runs its swap-queue walker. The same-binary control
                   arm for the gate: with it on, `dev+0x4174` stays 0 for the whole run
                   and the 16-record flip queue grows to 1,540 with nothing retired
CZ_SWAPQ_TRACE=1   the swap queue once a second: the gate, the vblank tick, records
                   retired, head/tail, the head record's own surface and due tick, the
                   GPU/CPU rendezvous word at [mirror+4] and the per-CPU acknowledge
                   bitmap at [mirror+0]. head==tail is the healthy shape; a tail
                   climbing away from a pinned head is a queue of flips nobody drains
CZ_ISR_SINGLE_CPU=1  deliver each graphics interrupt ONCE, as whatever CPU the pump
                   was constructed with (2) — the pre-part-5 behaviour. Default is one
                   delivery per bit of the arm's own six-bit CPU mask, with PCR+0x10C
                   reporting that CPU, because the ISR acknowledges by clearing
                   `1 << PCR[0x10C]` and an arm naming CPU 4 could never be
                   acknowledged otherwise (and the ISR's job ring is per-CPU too)
CZ_PM4_IB_TRACE=1  the first 64 INDIRECT_BUFFER packets with their raw address/size
CZ_PM4_DUMP_TRUNCATED=path dump the first 6 indirect buffers whose walk stopped short,
                   for offline re-walking (finding 38)
CZ_PM4_IB_VERIFY=1 snapshot every indirect buffer before walking it and compare after,
                   naming the first dword that moved. The instrument that killed the
                   "the guest is writing under us" theory: 84,808 buffers, 0 dirty.
                   Doubles the reads, so read a CLEAN result as the strong one
CZ_PM4_ZERO_IS_NOP=1       read a zero dword as a 1-dword no-op. Kept as an arm, and no
                   longer interesting: the zeros were our own unwritten VdSwap padding
                   (finding 39), and B1 turns out to contain no genuine zero header at
                   all, so the capture never had an opinion either way
CZ_NO_SWAP_PAD=1   leave VdSwap's 52-dword tail unwritten — the pre-finding-39 defect,
                   kept as the same-binary control arm for the load stall. With it on
                   the command processor walks the previous frame's packets
CZ_MULTIWAIT_APC=1 run pending APCs at the multi-object waits and report
                   STATUS_USER_APC. Correct NT semantics, off by default because
                   nothing has yet shown this title needs it
CZ_THREAD_TRACE=1  one line per guest thread with its HOST thread id, so gdb's stacks
                   can be joined to our logs (also implied by CZ_WAIT_TRACE/CZ_CS_TRACE)
CZ_ISR_TRACE=1     the scratch mirror the guest ISR reads, at each interrupt
CZ_ARG_PROBE=1     the guest-function argument probes in runtime/cpu/guest_probe.cpp
CZ_QUEUE_PROBE=1   the audio work-queue drain (sub_828576D8) and its seven call
                   sites — the instrument that showed A1's position 93 is behind a
                   failure path we never take (finding 49). Reports the first entry
                   of each, then goes quiet
CZ_JOBQ_PROBE=1    the graphics command-stream interpreter (sub_8284B568) on entry:
                   its shared object's callback/cursor state and the token buffer it
                   is about to walk. The last line before a crash IS the fatal call
CZ_FENCE_PROBE=1   the WHOLE producer side of the D3D fence/callback protocol, in
                   one flag and on BOTH arms: the fence-block emitter (828459D0),
                   the segment submit and its worker-vs-ring fork (82845AC0), the
                   close/kick (82845DE0), the callback armer (82845BA0) and the
                   graphics ISR itself (82844D38). Capped at 40,000 lines shared.
                   Run it on the PM4 arm AND on CZ_D3D_DRAW and DIFF — that
                   comparison found the missing worker kick after three sessions of
                   hypotheses about interrupt races. Session 15 added the CONSUMER
                   half, which is what the producer side alone could never show: the
                   sentinel handler / only decrementer (8284A960), the frame-end async
                   submit and only incrementer (8284B9C0), the counter spin itself
                   (82846210) and the ring submitter (828455C0) — and every cursor
                   argument is labelled SCRATCH or not, because "who reads what this
                   emits" is unanswerable from a bare address.
                   Session 16 added the three fields that retired phase C part 3's
                   ranked hypotheses: `[fence] submit` prints the fork's real inputs
                   (`incr` = r7, the counter delta, and `queue` = r8, which token
                   stream), and `[fence] kick` is a hook on sub_8284AAD0 ITSELF — the
                   ISR callback that pushes a token buffer onto the D3D worker's job
                   ring — printing the buffer pointer and flagging a kick that repeats
                   the previous one. Two arms, one diff: arms:deliveries is 768:766 on
                   the control arm and 12:856 on the draw arm
CZ_FENCE_RINGSUB=N how many sub_828455C0 calls print EVERY entry of their submission
                   list (default 4000), rather than the first two dwords of the first
                   eight. A replayed segment states its own identity nowhere else —
                   this is what showed the control arm submitting each frame's
                   arm-carrying segments about twice and the draw arm submitting one
                   of them 1,100 times
CZ_D3D_NO_RESERVE_KICK=1  suppress the guest's segment close/kick when the reserve
                   fires mid-redirect — the pre-fix arm for phase C part 2. With it
                   on the boot deadlocks at cinematics.big again (measured: file #56
                   vs #60, 5 worker kicks vs 36,747), which is what makes the fix a
                   measurement rather than an assertion
CZ_D3D_REDIRECT_PRESWAP=1  put sub_82841AD0's callback-arm block back in the
                   private scratch — the pre-fix arm for phase C part 3. That function
                   is named "PreSwapResolve" in the Phase A table and RESOLVES NOTHING;
                   with this on, all 405 of a boot's armings land in the scratch again
CZ_PM4_MEM_WATCH=hex  every write to one guest word, from BOTH streams (pm4.cpp and the
                   phase C walker each print their own line, so the log says which
                   stream wrote it). Pointed at the ISR mirror's callback slot it is
                   what proved the command processor was replaying the hand-off block
                   2.7 million times against the guest's 405 armings
CZ_FENCE_PROBE=N   as CZ_FENCE_PROBE=1, but N sets the shared line budget. Set it high:
                   a stall this probe exists to explain is at the END of a boot, and a
                   saturated budget is a floor, not a count (gotcha 109)
CZ_CRASH_TEST=nullcall  call through a zero ctr on purpose, to prove the crash
                   reporter names it. A self-test, not an arm — it announces itself
                   and the crash it causes is deliberate (finding 40)
CZ_KCALL_WHO=A,B   dump the guest call stack the first time these imports are called
CZ_AUDIO_TRACE=1   XMA context allocation + every 512th driver frame WITH its peak
                   amplitude, so "the pump runs" and "the game makes sound" stay
                   separable
CZ_AUDIO_FRAME_US=N  the driver frame period (default 5333 = 256 samples @ 48 kHz)
CZ_NO_AUDIO_PUMP=1 register the client but never invoke its callback — the control
                   arm for every claim about driving the audio callback
CZ_FAKE_START_MS=N synthetic START press every N ms. A MEASUREMENT ARM, NOT A
                   FEATURE — it manufactures progress, so it announces itself on
                   every press and must NEVER be on for a gate run (gotcha 78).
                   Kept now that real input exists: it is the control for "was it
                   really my press that moved the boot"
CZ_SAVE_DIR=path   where saves live (default: a SIBLING of the package directory,
                   assets/save/ — never inside assets/game/, which is extractor
                   output). An EMPTY save root is part of the A1 gate's configuration
                   (gotcha 106): A1 was captured with no save present
CZ_NO_WINDOW=1     no window, no present seam, no pad — XamInputGetState answers with
                   its documented neutral pad. The same-binary control arm for every
                   phase 3 claim. (`cmake -DCZ_WINDOW=OFF` is the build-time form,
                   for a machine with no SDL.)
CZ_VKDRAW=1        phase 5's renderer. OFF by default, so the same binary is also the
                   phase 3 binary — the control arm for every renderer claim
CZ_SHADER_DUMP=dir one file per distinct microcode blob at IM_LOAD, named by the hash
                   the renderer looks up. The input to tools/build_shader_spv.sh
CZ_SHADER_SPV=dir  override the shader cache location
CZ_VK_STATS=N      the renderer's named-counter block every N frames. Every path that
                   declines to draw something has a counter, because a renderer that
                   draws 80% of a frame looks exactly like one that draws all of it
CZ_VK_FRAME_DUMP=dir   every 64th presented frame as a PPM — the renderer checked
                   WITHOUT a window, which is what makes the E-screenshot comparison
                   self-servable instead of an operator task
CZ_VK_SNAP_DUMP=dir    EVERY resolve snapshot of one frame. The frame is the last link
                   in the chain, so a wrong frame is consistent with any pass being
                   wrong; this is the only instrument that says which
CZ_VK_RESOLVE_TRACE=1  each resolve's destination, extent and clear bits, against the
                   front buffer VdSwap named. The trace that found finding 5 below
CZ_VK_VIEWPORT_TRACE=1 every DISTINCT viewport setup, once each
CZ_VK_FETCH_PROBE=1    which vertex fetch slots the guest has actually populated
CZ_VK_STATE_PROBE=1    the distinct values of the state registers the renderer ASSUMES
                   rather than reads: the two constant-window bases, the render-target
                   format and the cull mode. Four assumptions checked in one run, and
                   it retired "no culling is a simplification" — this title does not cull
CZ_VK_INDEX_ENDIAN=N   force one index swizzle code for every draw. The arm that
                   retired index endianness: the packet's own code beats both overrides
                   by two orders of magnitude
CZ_VK_FORCE_COLORMASK=1  treat every draw as writing all four channels — the arm that
                   retired "38.6% of draws have an empty colour mask, so the register
                   index must be wrong" (it is a real depth-only pass; frame identical)
CZ_DETERMINISTIC_CLOCK=1  the guest clock advances a fixed quantum per PRESENTED
                   FRAME instead of tracking the host TSC, covering BOTH guest time
                   sources (mftb and interrupt time). A MEASUREMENT ARM — it changes
                   what the guest observes about time, announces itself, and must
                   never be on for a gate run. PARTIAL: it halves the distinct camera
                   count and takes one pair of runs from 0.2% to 49.6% identical
                   cameras, but a third run still diverges (phase5-notes §6p)
CZ_VK_FRAME_STATS=file  one line per presented frame: draws, vertices, a draw-stream
                   fingerprint, a camera fingerprint, and the output's coverage, mean
                   luminance, distinct colours and pixel hash. The input to
                   tools/frame_compare.py
CZ_VK_FRAME_STATS_SURFACE=hex  ALSO measure that resolve surface each frame. Not a
                   refinement — the metric does not work without it, because the
                   PRESENTED frame at the title screen is mostly UI and a change
                   touching 476,858 draws moved it 0.1 pp. 06BE4000 is the scene
CZ_VK_ONLY_VS=hex[,hex] / CZ_VK_SKIP_VS=hex[,hex]  render only, or all but, those
                   vertex shaders' draws — the bisection arms. NB the picture they
                   produce is a random sample of an animated scene: judge them with
                   tools/frame_compare.py, never by eye (gotcha 133)
CZ_VK_DRAW_PROBE_MINVERTS=N  bound the draw probe to meshes of at least N indices. A
                   shader's first three draws are usually its smallest, and a defect
                   that only shows on large geometry is invisible in them
CZ_VK_SHADER_CENSUS=1  draws per (vs, ps) pair. With the capture's disassembly beside
                   every blob, this is the pair that localises a shading bug
CZ_VK_DRAW_PROBE=hash  one draw's actual matrices and the vertex data it will read
CZ_VK_STATE_PROBE=1    the state registers the renderer ASSUMES rather than reads
CZ_VK_FETCH_SLOT_INVERT=1  read vertex fetch constants at 95-slot — the arm that
                   settled the fetch-slot convention unambiguously (inverted: 0.0%)
CZ_VK_INDEX_ENDIAN=N   force one index swizzle code for every draw
CZ_VK_NO_TEX_SWIZZLE=1  ignore the fetch constant's component swizzle, i.e. the
                   pre-fix behaviour where a single-channel font atlas samples alpha
                   as a constant 1.0 and all text renders as SOLID BLOCKS
CZ_VK_NO_FLIP_Y=1  render with a positive-height viewport, i.e. the pre-fix vertically
                   MIRRORED frame. The arm for the flip that made the title screen
                   appear; note no numeric instrument in this project can tell the two
                   arms apart (gotcha 135)
CZ_VK_NO_TEXCOORD_SWAP=1   suppress the 16-bit texcoord unswizzle mask
CZ_VK_PRIM_RESTART=1   honour 0xFFFF as a strip separator. OFF because the guest
                   declares VGT_MAX_VTX_INDX=65535, i.e. 0xFFFF is a LEGAL index
CZ_VK_RESOLVE_TRACE=N  from frame N: each resolve's destination, extent, copy window,
                   clear bits and the DRAW COUNT of the pass it closes
CZ_VK_VALIDATION=1 the Khronos validation layer. Slow at ~900 draws a frame, and it has
                   twice named an API misuse that was being investigated as a renderer bug
CZ_INPUT_TRACE=1   every pad packet published to the guest, with its button mask.
                   An instrument, not an arm: it fabricates nothing, and it is the
                   witness that a real press reached XamInputGetState. Silent on a
                   keyboard-only run until a key is actually pressed; noisy with a
                   physical stick attached, because XInput's packet number moves on
                   raw jitter too and we do not filter (gotcha 102)
```

`CZ_KCALL_WHO` is the companion to the phase gate: the gate says *that* our
first-occurrence order diverges, and the most informative divergences are imports we
call which hardware never calls at all. Only the call site explains those.

A/B the renderer. **This is the only sound way to claim a renderer change helped** —
two of this phase's three "measured improvement" claims turned out to be noise from the
title screen's ANIMATED 3D background, because a single run of an unvalidated metric is
not a measurement (gotchas 50/51/86). Aggregate over the era; never compare by frame
index (gotcha 38):
```
for a in base arm; do
  (cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_VK_FRAME_STATS_SURFACE=06BE4000 \
      CZ_VK_FRAME_STATS=/tmp/$a.txt timeout 85 ./cz_runtime >/dev/null 2>&1)
done
python3 tools/frame_compare.py /tmp/base.txt /tmp/arm.txt
```
Baseline band is **1.36 pp** of median surface coverage over five runs of one binary;
the tool calls anything inside 1.5 pp "no detectable difference". It has been shown
capable of failing (gotcha 30): `CZ_VK_PRIM_RESTART=1` reads 17 pp outside the band.

Check a frame against capture E, and NAME the transform if it is one. This is the
instrument the phase-5 blind spot needed: the frame was rendered vertically mirrored for
a whole phase and no aggregate could see it (gotcha 135). Exit 1 = the frame is
transformed:
```
python3 tools/frame_signature.py \
    --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" /tmp/frames/frame_000448.ppm
```
And the pixel A/B against a noise floor measured from the same runs — imported from
Fable 2, and preferred over `frame_compare.py`'s quoted band whenever there are >= 2
runs per arm:
```
python3 tools/frame_matched_diff.py --a runA1 runA2 --b runB1 runB2
```

Disassemble the guest image. **Reach for this before reading `ppc/`** — a recompiled
function is a translation, and most questions ("what writes this field", "which branch
does this predicate take", "how many arguments does this call site really pass") are
about the original. The host toolchain cannot do it: no PowerPC target in `objdump`, no
`-b binary` in `llvm-objdump`, and `llvm-mc` silently loses instruction alignment on
the first VMX128 encoding it does not know:
```
python3 tools/gdis.py 8284B568 --count 120        # a function
python3 tools/gdis.py 8284B6C0 --to 8284B710      # a window around a faulting insn
python3 tools/gdis.py --find-uses 0x7FEA1800      # every lis/addi pair building a
                                                  # constant, with context — a 32-bit
                                                  # constant is never one instruction,
                                                  # so grepping the image misses them
```

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

Check the command processor against the boundaries hardware itself used. The first
covers each packet's LENGTH (24.5 M packets); the second covers a WALK — every indirect
buffer's start address and every internal boundary, chained from its first dword (28,726
buffers), which is the half a per-packet check structurally cannot see. **Exit 1 = our
parser would desync on a real stream:**
```
python3 tools/pm4_packet_lengths.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/pm4_indirect_walks.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```
Both passing means our *arithmetic* is right. It says nothing about whether the bytes
we walk are the bytes hardware walked — that was finding 39's whole lesson (gotcha 88),
and the live counter for it is `ring: indirect buffers truncated=` in `CZ_RING_TRACE`,
which must be **0**.

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
  Xenia's `dump_shaders` gave us 455 microcode blob files — **335 distinct** shaders,
  A1's 120 being a strict subset of A2's — which is XenosRecomp's input, so the
  renderer was unblocked anyway.

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

Next, in order:

1. **A1 is exhausted as an oracle.** Its position 93 is NOT the next piece of work —
   `KeQueryBasePriorityThread` has been implemented since phase 1, and reaching it
   means reproducing an audio-subsystem FAILURE that hardware had once, late, on a
   path we do not drive (finding 49, gotcha 107). The gate now needs a capture that
   goes further than A1: gameplay (A2), which is a different comparison to build.
2. **Prove the still-unexercised imports** (gotcha 67 — implemented is a prediction,
   not a result). Four of finding 34's eight have now RUN — `XamTaskSchedule`,
   `XamGetOverlappedResult`, `XMsgInProcessCall`, `XMsgCompleteIORequest`, all on the
   save-data path. Still unrun: the rest of finding 34, both of finding 36's teardown
   paths (`XAudioUnregisterRenderDriverClient`, `XMAReleaseContext` — the boot never
   shuts audio down), and the save layer's own `XamContentCreateEx`/`XamContentClose`,
   which need gameplay to reach a save point.
3. **Phase 5 — finish the renderer.** It exists and draws (below); what it does not yet
   do is produce a correct title screen. `docs/phase5-notes.md` §7 is the enumerated
   gap, with a measurement for each part of it. Gate on **per-era aggregates, never
   frame index**: two hardware runs agree frame-exactly only 80.0% of the time (gotcha
   38), so the plan's "per-pixel diff at the same frame" is the one line of it to
   correct.
4. Audio output and XMA decoding (phase 6). The kick bitmap at `0x7FEA1A80` currently
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
