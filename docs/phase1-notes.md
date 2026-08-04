# Phase 1 — kernel HLE and guest bootstrap

Written 2026-08-04 (session 4), while phase 1 is in progress. Status at the bottom.

The plan is `docs/runtime-plan.md` §"Phase 1"; the hand-off that started this work is
`docs/phase1-kickoff.md`. This file records what the work actually found — the parts
that were not in either document, because they came out of the ground truth or out of
running the thing.

---

## 1. The gate, and the two captures it needs

`tools/kernel_call_diff.py` compares the **first-occurrence order** of kernel calls,
ours against Xenia's. Not the raw stream: Case Zero calls `XamInputGetCapabilities`
5,501 times and `XamUserGetXUID` 4,441 times in the A1 boot, and no runtime
reproduces an emulator's thread interleaving. The order in which subsystems are
*first* touched is reproducible; the interleaving of the polls is not.

Three traps are baked into the tool because each one produces a confident wrong
answer:

**Xenia hides part of the kernel even at `log_level=3`.** Exports tagged
`kHighFrequency` need `log_high_frequency_kernel_calls=true`, which defaults off, and
A1–A4 all record it as false. A naive diff shows our runtime "calling functions Xenia
never called".

Case Zero's hidden set is **35 names**, and it is *measured* rather than copied from
the previous port: it is exactly `first-occurrence(A5) − first-occurrence(A1)`, A5
being A1's drive re-run with the cvar on. Deriving it this way makes it automatically
the intersection with what *this* title imports, and true to the fork the captures
were taken on. It is most of the synchronisation surface —
`RtlEnterCriticalSection`, `KeSetEvent`, `NtWaitForSingleObjectEx`,
`KeDelayExecutionThread`, `NtReadFile`, `VdSwap`.

**A5 is NOT a superset of A1.** It nearly is, which is what makes the assumption
dangerous. Eleven names appear in A1 and not in A5 — `XamShowDeviceSelectorUI`,
`XamContentAggregateCreateEnumerator`, `XamGetOverlappedResult`, `XamTaskSchedule`,
`XMsgInProcessCall` and the rest of the storage-device-selector path, which that
drive did not enter. So **A1 is the authority for the boot sequence, A5 for the
synchronisation surface, and neither alone is the gate.** Run both.

**Xenia's internal helpers wear the same line shape as kernel calls.**
`GetProcAddressByOrdinal`, `ResolvePath`, `SetThreadName`, `SetInterruptCallback`,
`XEnumerateCrossTitle`, `XamGetPrivateEnumStructureFromHandle` and `XGIUserSetContext`
are all logged as `Name(args)` and none is an import of this image. Both sides are
intersected with `ppc/ppc_recomp_shared.h`'s 244 `__imp__` externs.

## 2. The import set: three independent sources agree

Worth recording because it is the cheapest confirmation available that we are
building against the right title, and it cost one grep:

| source | functions | variables |
|---|---|---|
| `ppc/ppc_recomp_shared.h` (`PPC_EXTERN_FUNC(__imp__…)`) | 244 | — |
| A1's Xenia import dump (`F` / `V` rows) | 244 | 13 |
| our loader's descriptor walk at runtime | 244 | 13 |

`kernel/xex_imports.cpp` warns if its own walk disagrees with 244/13, so a future
drift is loud rather than silent.

**The 13 kernel *variable* ordinals are identical to Asura's Wrath's.** Same
ordinals, same names, despite a different studio, engine and SDK year. These are what
the XDK's static libraries pull in, so the set is a property of the SDK rather than of
the title — useful for Case West, and *not* a licence to skip the check.

## 3. The arena map is confirmed by this title, not assumed from the last one

Gotcha 9 says to put the guest arenas where the console puts them. A1 makes that
checkable in two independent ways:

```
NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0) = 40000000
NtAllocateVirtualMemory(base=40000000, size=00010000, 60001000, 4, 0) = 40000000
NtAllocateVirtualMemory(base=40010000, size=00020000, 60001000, 4, 0)
...
NtAllocateVirtualMemory(base=00000000, size=00100000, 60002000, 4, 0)  <- second 1 MB
NtAllocateVirtualMemory(base=40100000, size=00020000, 60001000, 4, 0)
```

`0x60002000` = `MEM_16MB_PAGES | MEM_LARGE_PAGES | MEM_RESERVE`; `0x60001000` is the
same with `MEM_COMMIT`. So the title reserves 1 MB regions and then commits 64 KB
granules up through them. Two things follow and both are checkable rather than
assumed: the large-page arena must begin at exactly `0x40000000`, and the *second*
reservation must come back at `0x40100000` — which it does iff our arena hands out
64 KB-granular blocks in address order. It does.

The first physical allocation is a single **447 MB** `MmAllocatePhysicalMemoryEx`
(`0x1BF16000`), i.e. the title claiming essentially all of the console's 512 MB up
front. Our physical arena is 511.94 MB, so it fits with room to spare.

## 4. Findings

### Finding 15 — a generated stub cannot obey the out-parameter rule, and saying so is the fix

Asura's Wrath's finding 14 sharpened gotcha 5 into: *a stub for a call with an
out-parameter must either fill the out-parameter or not exist*, because "returns an
error" only protects you against a guest that checks the return.

`tools/gen_import_stubs.py` **cannot obey that rule**. It is produced from a list of
names in `ppc/ppc_recomp_shared.h` and has no signature, so it does not know which
register holds a pointer — and guessing by name prefix would be wrong *silently*,
which is worse than being wrong loudly. The resolution is not a cleverer generator; it
is to state the limit in the generator's own docstring, name the two failure modes so
they are recognisable in the field, and keep the escalation path short (give the
import a real `GUEST_FUNCTION_HOOK` signature in `kernel/imports.cpp`, and the
generator drops it on the next run).

The two failure modes, both of which look like something else:

- **A guest that trusts an untouched out-buffer.** Presents as corruption or a hang
  far from the call. The gate diff localises it: look at what our log called last.
- **A guest that uses the RETURN VALUE as a pointer.** Not every import returns
  NTSTATUS; some return handles, counts or pointers. `0xC0000002` is then a wild
  pointer, and the fault lands on a dereference of `0xC0000002` or an address near it.
  **That specific address in a segfault means "an unimplemented import was asked for a
  pointer", every time.**

### Finding 16 — an honest-failure stub steered the guest into its dirty-disc path, exactly as designed

The first run that entered the guest produced this:

```
  22  NtCreateFile               NtCreateFile
  23  NtQueryFullAttributesFile  XamShowDirtyDiscErrorUI
  24  NtClose                    XamLoaderLaunchTitle
```

`NtCreateFile` was still a generated stub, so the title concluded it could not read
the disc and took its dirty-disc path — a *correct* response to an honest failure. The
whole reason phase 1 converts the abort-stubs to honest failures is visible in one
line here: an abort would have stopped at call 1 and told us nothing about ordering,
whereas this diverged at a named call whose repair was obvious.

It also decided the phase boundary. `docs/runtime-plan.md` puts the VFS in phase 2,
but A1's **22nd** distinct kernel call is already an `NtCreateFile`, so phase 1's own
gate cannot be reached without a file layer. `kernel/vfs.cpp` and
`kernel/file_imports.cpp` therefore moved forward into phase 1; what stays in phase 2
is the part that phase is really about — the `.big` archive semantics and the
seek-order oracle.

### Finding 17 — a diagnostic can be disabled by an address-range assumption inherited from another port's memory map

Both template ports scan a stalled guest thread's stack for return addresses and bound
the scan with `addr < 0x80000000`. That is true of a console guest stack and true of
their runtimes'. It is **false of ours**: guest thread blocks are allocated from the
o1heap user arena, which `kernel/heap.cpp` places at `0x88000000`.

So the bound failed on the very first word, the scan broke immediately, and every
stall and wait trace printed

```
[stall] KeDelayExecutionThread tid=00000F00 ... lr=82829FCC callers:
```

with nothing after `callers:`. That reads as *"the instrument ran and found no guest
callers"* — a clean, small, wrong answer — rather than *"the instrument never ran"*.
It is gotcha 25's shape in a new place: before believing an empty result, confirm the
thing was capable of producing a non-empty one. Corrected, the same run prints ten
frames immediately.

The general form, and it is not specific to stack scans: **a ported diagnostic can
carry an assumption about the memory map that was true where it came from.** Every
inherited constant that is a *range* deserves the same question as every inherited
constant that is an *address*.

### Finding 18 — this title resolves all seven xam ordinals; Asura's Wrath's refusal does not transfer

A1's loader seam:

```
XexLoadImage("xam.xex", 9, 0, out) -> 30002000
XexGetProcedureAddress(30002000, 0xAFF)   XamPartyGetUserList
                       ... 0xB00          XamPartySendGameInvites
                       ... 0xB0B          XamPartySetCustomData
                       ... 0xB10          XamPartyGetBandwidth
                       ... 0x305          XamShowPartyUI
                       ... 0x30B          XamShowCommunitySessionsUI
                       ... 0x279
```

Seven resolutions, and Xenia resolves **all seven** — there is no "ordinal not found"
line anywhere in A1. Asura's Wrath's runtime reproduces a refusal (its ordinal 0x48C),
and copying that behaviour here would have invented a failure the console never had.
A title told "no party support" takes a different path from one told yes and then
handed an error at call time.

### Finding 19 — Case Zero leaves no free code-range constant, so the mint region is found rather than chosen

`XexGetProcedureAddress` must return a guest address the dispatch table can index,
i.e. inside `[PPC_CODE_BASE, PPC_CODE_BASE + PPC_CODE_SIZE)` — the table covers the
CODE range, not the image range. Asura's Wrath's finding 54 is what happens otherwise:
it picked a constant "past the last section, still inside the image", which was 4.7 MB
beyond the table, so the write landed 9.4 MB past the table's end and the stub that
was supposed to fail honestly never ran. The caller saw `r3 = 0` — success — from a
call that did nothing.

Case Zero offers no comfortable constant at all. Its last mapped function starts at
`0x829C3554` and the code range ends **16 bytes later** at `0x829C3564`. There is no
tail pad.

So the slot region is **found at first use**: one pass over the dispatch table for the
longest run of consecutive unmapped 4-byte slots, taking a point inside it, logging
what it picked and re-checking each slot at mint time. It costs one scan per process
and it cannot go stale, because there is no constant to go stale. That is the general
lesson — *when the safe value is a property of the generated function list, compute it
from the function list.*

### Finding 20 — the async completion is an APC, and a read that drops it looks like it worked

This is the one that unblocked the boot, and it took the prefix from **23 to 43**.

Our first `NtReadFile` filled the caller's buffer and its `IO_STATUS_BLOCK`,
returned `STATUS_SUCCESS`, and hung the title. The main thread polled a completion
flag forever while the `cAsyncFileSystem` worker sat in a wait, and nothing in our
log said anything was wrong — the read had *worked*.

A5 is the read oracle (finding 2 — `NtReadFile` is `kHighFrequency` and invisible in
A1), and one line settles it:

```
d> F8000018 NtReadFile(F8000020, event=00000000, apc=82831B21, apcCtx=82789670,
                       iosb=E418D208, buffer=FFCA0000, length=00009000, offset=0)
```

`event` is **zero**. This title's async file system delivers completion through the
**APC routine**, and the issuing thread then parks in an alertable wait so the APC
can run — A5 shows the idiom plainly on the same thread:

```
d> F8000018 NtSetEvent(F8000014, 00000000)                       <- its own queue event
d> F8000018 NtWaitForSingleObjectEx(F8000014, 1, alertable=1, NULL)
```

Our implementation signalled the event and ignored `apcRoutine` entirely. Signalling
a handle of 0 is a no-op, so the notification was simply dropped.

The general rule, and it is the out-parameter rule one level up: **the contract of an
async call includes its notification, not just its data.** A stub that gets the data
right and the notification wrong is *harder* to find than one that fails outright,
because every observable it touches looks correct.

Two supporting details that would each have cost time on their own: the APC routine's
low bit is the kernel's own flag rather than part of the address (`0x82831B21` →
`0x82831B20`), and the APC's `IO_STATUS_BLOCK` argument must be passed as a **guest**
address, not the host pointer the marshalling layer handed us.

A1 alone could not have found this. Its `NtReadFile` lines do not exist.

## 5. Where the boot currently stops — the GPU ring buffer

Reproducible, and it is exactly where `docs/runtime-plan.md` said phase 4 would be
needed. Not a bug: a wall.

The main thread runs the whole GPU bring-up in hardware's order — `VdInitializeEngines`,
`VdSetGraphicsInterruptCallback`, `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`,
`VdQueryVideoMode`, `VdRetrainEDRAM` — with **every one of those still a generated
honest-failure stub**, and then stops making kernel calls at all. It is spinning in
guest code, in the D3D driver:

```
sub_82845160  <-  sub_82846210  <-  sub_828519A0  <-  sub_8284CF88  <-  sub_8283CCE8
```

and the loop in `sub_82845160` is unmistakable:

```
lwz  r11, 10896(r31)     ; the read-pointer WRITE-BACK slot's address
lwz  r10, 10908(r31)     ; the write pointer
lwz  r11, 0(r11)         ; *write-back = how far the GPU has consumed
subf r9,  r30, r10
subf r11, r11, r10
cmplw cr6, r9, r11
bge  cr6, <exit>         ; enough ring space? no -> spin
```

That is a **ring-buffer free-space wait**. A1 shows the driver setting it up:

```
VdInitializeRingBuffer(03D72000, 14)            <- 1 MB ring at physical 0x03D72000
VdEnableRingBufferRPtrWriteBack(03D7103C, 8)    <- write-back slot at 0x03D7103C
```

Both of those are honest-failure stubs here, so the slot was never armed and nothing
can ever advance it. The driver is waiting for a GPU that does not exist yet.

This is verbatim the trap the plan flagged for phase 4:

> a command stream carrying the answers to its own waits — the D3D driver's GPU waits
> poll a retired-fence counter and a consumed-to pointer that no runtime could
> honestly invent

so reaching it is the expected end of the kernel-only road. Faking the write-back
pointer would be exactly the "never fake success" violation gotcha 5 forbids: the
driver would march on and submit commands into a ring nobody reads.

**Also still open** (from the A5 strong gate, and cheaper than the GPU): our first
`RtlNtStatusToDosError` arrives at position 19, ahead of hardware's. Something is
failing and being translated that does not fail on the console. It has not blocked
anything, but it is an unexplained difference and those do not stay harmless.

## 6. Status

Phase 1's stated gate is a prefix-match out to the *title screen*. We reach the GPU
ring-buffer wait, which is as far as a kernel-only runtime can go.

- `tools/kernel_call_diff.py --xenia A1 --ours <log>`
  → **PREFIX MATCH, 43 of 93**, stopping before `VdIsHSIOTrainingSucceeded`.
- `... --xenia A5 --ours <log> --include-high-frequency`
  → mismatch windows only in the scheduling-sensitive region; see §5's open item.
- 97 of 244 imports are real; 147 are generated honest-failure stubs — and note that
  the last 16 positions of that 43 were walked entirely on **stubs**, which is the
  clearest evidence available that honest failure beats aborting.
- `cz_runtime --smoke` still passes, so the phase 0.2 link gate is intact.

**The next milestone is `gpu/vd.cpp` and the command processor** — nominally phases 3
and 4, arriving now because the boot goes through them. Two things are already
prepared for it: `kernel/heap.cpp` withholds the GPU register aperture at
`0x7FC80000` from the virtual arena, and phase 0.3 established that the command
processor can be built and gated against **B1 alone** (finding 10d — 21 type-3
opcodes, identical across all three captures).

One piece of debt to clear when that lands: `XGetVideoMode` currently lives in
`kernel/imports.cpp`. It **must** move to `gpu/vd.cpp` beside `VdQueryVideoMode` and
both must call one filler — A1 calls `XGetVideoMode` twice and `VdQueryVideoMode`
three times during the same display bring-up, and the driver's letterbox arithmetic
straddles the two. Two independent copies is how they drift.
