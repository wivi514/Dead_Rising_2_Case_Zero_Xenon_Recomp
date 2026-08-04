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

## 5. Where the boot currently stops

Reproducible, and localised to named guest addresses.

The main thread reads `game:\layout.bin` (36,176 bytes) and then parks in a poll loop:

```
[stall] KeDelayExecutionThread tid=00000F00 r13=88004D60 lr=82829FCC
        callers: 82776760 8277196C 82776760 828223F4 82776760 8276D5CC
                 82784938 8277EC4C 8277E424 82786260
```

`0x82829FCC` is inside `sub_82829F78`, which is XAPI's `Sleep()` — the loop that
retries `KeDelayExecutionThread` while it returns `STATUS_ALERTED` (257). Ours never
returns 257, so `Sleep` itself is not the loop; the caller chain is, and it sits in
`0x8276xxxx`–`0x8278xxxx`, the same module as the `.big` reader cluster identified by
finding 14 (`0x82764CF8`–`0x82769338`).

Meanwhile the `cAsyncFileSystem` worker is parked on an event nobody signals:

```
[wait] tid=00000F04 r13=88084D60 (entry=82769D58) handle=BBF16000 lr=825DA5F0
       stuck 5s EVENT callers: 82769D58 82787404 82769D58 82769D84 82769D58 82829BEC
```

i.e. **a producer/consumer completion handshake that is not completing.** That is the
next thing to work on, and both ends of it now have addresses.

Also worth chasing, from the A5 strong gate: our first `RtlNtStatusToDosError` arrives
around position 19, earlier than Xenia's. Something is failing and being translated
that does not fail on hardware; whatever it is, it is upstream of the stall.

## 6. Status

**Not complete.** Phase 1's gate is a prefix-match out to the *title screen*; we reach
the first file read.

- `tools/kernel_call_diff.py --xenia A1 --ours <log>`
  → **PREFIX MATCH, 23 of 93**, stopping before `NtClose`.
- `... --xenia A5 --ours <log> --include-high-frequency`
  → 2 real mismatch windows (see §5).
- 97 of 244 imports are real; 147 are generated honest-failure stubs.
- `cz_runtime --smoke` still passes, so the phase 0.2 link gate is intact.

The next milestones, in order: the async-file completion handshake above; then the
`Vd*` block, which is 20 of A1's next 25 first-occurrences and is phase 3/4 work
arriving early because the boot goes through it.
