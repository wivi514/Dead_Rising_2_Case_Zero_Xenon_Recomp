# The boot hang on the operator's second PC — investigation record (part 99)

**Status: NOT SOLVED. v1.0.1 is built, gated and staged but NOT PUBLISHED, and
should not be until this is understood** (`docs/release-notes-v1.0.1.md` holds
the finished body and both SHA-256s).

The operator reported "stuck on the Capcom logo" on a second PC, and that the
part-99 skip-intro-logos toggle did not help — it hung on the Blue Castle logo
instead, which is exactly what a shortened timeline does to a hang that is not
about the timeline.

## §0 The machine, and how to reach it

`ssh czamd` → `lisab@192.168.0.60`, Windows 10 (19045), **AMD Radeon RX 6600**,
**Ryzen 5 5500 (6c/12t)**, 16 GB, Crucial MX500 SSD, locale fr-CA. It is the
operator's only AMD-GPU PC and is effectively a dedicated test box.

Getting in took three passes and the record is worth keeping: the built-in
Windows 10 OpenSSH is **7.7p1 (2018)** and was damaged — `C:\ProgramData\ssh`
had no host keys and no `sshd_config`, `sshd -T` failed silently, and the
service died with error 1053 (start timeout). Generating host keys and restoring
the config did NOT fix it. **Microsoft's standalone Win32-OpenSSH release
installed over it and worked immediately** — that is the remedy to reach for
first on Windows 10, not the Windows feature.

Run PowerShell there with `-EncodedCommand` (base64 of UTF-16LE); the three
quoting layers otherwise bite exactly as `windows-build-setup.md` warns. Note
PowerShell prepends a CLIXML block and **the first line of real output gets
concatenated onto it** — strip the XML with a regex, not `grep -v`, or you will
silently lose the first result of every batch (this cost two misread runs).

The batch files written for the operator live in the shared release folder:
`enable-remote-debug.bat`, `fix-ssh.bat`, `fix-ssh-2.bat` (the one that worked),
`capture-hang.bat` (a no-admin five-arm bisection that writes logs beside itself).

## §1 What the hang IS — characterised precisely

* States reach `Startup → LegalScreen → Loading` and **never FrontEnd**. A
  healthy boot gets there in ~8 s (`CZ_STATE_TRACE=1`, new in part 99).
* **It reproduces headlessly**, over SSH, in under 100 s. No window, no logo, no
  operator needed. This is the single most useful fact for the next session.
* It is a **spin, not a blocked wait**: CPU time ≈ wall time (237.9 s of CPU in
  240 s), two threads at 100%. That is why `CZ_WAIT_TRACE` found nothing.
* Frozen hard: file count sits at 63-67 from t=60 s to t=240 s with no movement.
* `CZ_STALL_TRACE=500` names both spinners. The load-side one polls
  `predicate 0x827144C0(obj)` which returns `obj->0x80` under a lock — i.e.
  **`while (status == 1)`, a load-in-progress flag that never clears**. The other
  is an ordinary service loop.
* The engine's own log (`CZ_GUEST_LOG=1 CZ_GUEST_DIAG=1`) stops mid cube-map
  load: `[CUBES] Loading cc_03.bct into slot 0`, then `slot 3`, then nothing.
  A working machine continues `slot 2 → slot 1 → cc_02` and, decisively, prints
  `<> CallbackLoadRequest : zone = 0, asset = 1, info.mStatus = 2`.
  **Hers never prints a single CallbackLoadRequest.**
* File I/O is healthy right up to the stop: every `NtReadFile` returns its full
  byte count, ending with the index reads at the tail of the 73 MB
  `prologue_menu/prologue_z01.big`. Then reads simply stop being issued.

## §2 Refuted — do not re-buy any of these

Each was a real run, most on both machines:

| hypothesis | how it died |
|---|---|
| AMD **GPU** / the renderer | hangs identically with `CZ_VKDRAW=0`; control: renderer-off still reaches FrontEnd on a working box |
| MSAA, deferred clears, async pipelines | subsumed by the above — no Vulkan at all |
| Windows | **czwin** (Windows, Release build) reaches FrontEnd |
| French locale | czwin is fr-FR and works |
| Her game data | extraction is **256 files / 859,007,897 bytes**, byte-identical to the container gate |
| Worker/thread budget | `CZ_WORKERS=1` does not reproduce on a working box; `CZ_WORKERS=6` does not fix hers |
| Core count | local affinity runs at 12, 6 and 4 CPUs all reach FrontEnd |
| A scheduling race | **single-core (`affinity=1`) still hangs** |
| CPU vendor/family | the working dev box is a **Ryzen 7 5700, same Zen 3** |
| Multi-wait APC starvation | `CZ_MULTIWAIT_APC=1` does not fix it |
| Our generated asset overlay | `CZ_NO_PATCHED_ASSETS=1` still hangs |
| Audio / XMA | `CZ_NO_AUDIO_OUT=1 CZ_NO_XMA_DECODE=1` still hangs |
| PM4 tick rate | `CZ_PM4_TICK_MS=1` still hangs |
| A missing/dead guest thread | thread-name census is identical on both machines |
| Wrong binary | her exe matches today's v1.0.1 build by size and timestamp |

## §3 The live hypothesis, and the two arms built for it (UNTESTED)

The completion path is an **APC**: `NtReadFile` completes synchronously and
queues the guest's completion routine with `QueueThreadApc` on the **calling**
thread. `t_apcQueue` is thread-local, so no other thread can ever run it. Every
drain site is gated on `alertable` **and runs only at wait ENTRY**, and
`WaitObject` on an infinite timeout is a true blocking wait. So an APC queued to
a thread that is already parked — or that parks non-alertably — is never
delivered, the zone-load completion never fires, and the loader polls its status
word forever. That matches every observation above, including the absent
`CallbackLoadRequest`.

`imports.cpp`'s own comment predicted this shape years of parts ago ("an IO
completion queued to a thread that then parks in a multi-object wait never
runs… promote it then"), but its arm (`CZ_MULTIWAIT_APC`) only covers multi-waits
at entry, which is why it did not help.

Two arms are now in the tree, **both default OFF and both UNVERIFIED — the
verification run was interrupted before it completed**:

* `CZ_APC_INLINE=1` — run a file-completion APC immediately at the read. Our
  reads are synchronous, so the data is already there. Simplest, most likely to
  work, some re-entrancy risk.
* `CZ_APC_ALWAYS=1` — drain regardless of `alertable`, and poll infinite
  `WaitObject`s in 2 ms slices so an APC queued after a thread parked is still
  picked up.

## §4 Next session, in order

1. **Verify the arms do not regress a working machine** (this is what was
   interrupted): `CZ_APC_INLINE=1` and `CZ_APC_ALWAYS=1` must still reach
   FrontEnd locally, and the A5 kernel-diff gate must be unchanged.
2. **Test both arms on czamd.** The loop is: build on czwin
   (`vc.bat cmake --build …\runtime\build`), copy `cz_runtime.exe` to
   `C:\Users\lisab\Desktop\CaseZeroRecomp\`, run headless with
   `CZ_STATE_TRACE=1`, check for `request FrontEnd`. Whole cycle is minutes.
3. If neither works, the next instrument is a counter for **undelivered APCs**
   (how many sit in `t_apcQueue` at thread exit / after N seconds) — that turns
   the hypothesis into a measurement instead of an inference.
4. Whatever the fix, it is a **correctness fix for everyone**, not an AMD
   workaround — the public "stuck on Capcom logo" report is very likely this
   same bug, and it is NOT part 98's compile stall.
5. Only then publish v1.0.1 (artifacts and hashes are already staged).

---

## §5 Part 100: one real fix, a sharpened diagnosis, and the instruments

**Status still NOT fully solved, but materially advanced. v1.0.1 stays unpublished.**

### §5.1 The fix that shipped — `NtReleaseSemaphore` now honours the maximum

The part-99 APC arms were both refuted by re-running them:
- `CZ_APC_INLINE=1` **regresses a working machine** — it re-enters guest code at the
  read, inside the loader's own lock, and hangs the dev box at file #1.
- `CZ_APC_ALWAYS=1` does not fix czamd.

And `CZ_APC_TRACE=1` (new: per-thread queued-vs-drained APC counts) settled the
APC hypothesis itself: czamd's frozen state is **balanced, 81–88 queued = 81–88
drained on one thread**. APC starvation is NOT the mechanism; every completion APC
ran.

`CZ_KCALL_WHO` (extended to fire on milestones, not just first call) named the hot
loop: `NtReleaseSemaphore` at ~660k/s, backtrace
`82822520 ← 827780FC ← 82778E5C ← 82771E58 ← 827617C8` — the streaming pump's
listener-dispatch (`sub_82771D70`) posting work to a ring
(`sub_82778060`) and releasing its work-semaphore, once per spin.

Our `Semaphore::Release` ignored `Limit` and let the count grow unbounded; the guest
creates every one with `maximum=0x10` and its release wrapper is Win32
`ReleaseSemaphore`, which **checks the status and `SetLastError`s a failure**
(`0x825da110` is `SetLastError`, not an assert). Enforcing the limit
(`STATUS_SEMAPHORE_LIMIT_EXCEEDED` when `count+n > maximum`, count and out-param
untouched) is NT-correct and shipped (commit on master, part 100). Local boot still
reaches FrontEnd; A5 gate exit 0 / 0 real.

**Effect on czamd, measured:** before the fix `UpdateStreaming` (`sub_82760CF0`,
which ticks the stuck streaming manager `A18DE998`) ran **0 times** while the pump
spun; after the fix it runs continuously and `sub_82771D70` returns and re-loops
49k+ times. **The pump is no longer frozen.** This is a correctness fix for everyone
regardless of whether it is the whole story.

### §5.2 The residual hang — precisely located

Even with the pump ticking, czamd still parks in **Loading, never FrontEnd**, frozen
at the **exact same guest point**: cube-map `cc_03.bct` loads slot 0 and slot 3, and
**slots 2 and 1 never load** (`[CUBES] Loading … slot N` stops). `CallbackLoadRequest`
is never printed; the zone-load request object `A18DE998` polls `status==1` forever.

- File I/O is healthy to the stop: the last ops are 4× `streamedassets.big`
  71680 bytes @ 15646720 into two alternating buffers — **the same count the healthy
  box issues** — then reads simply stop being issued. Healthy issues the next slot's
  reads here; czamd issues nothing.
- The completion APC (`82831B20`) runs (APC counts balanced), but its guest routine
  neither issues the next slot's read nor marks the cube done. The step that decides
  "what next" after the 4th read runs on czamd but does not advance.

The game data is byte-identical (256 files / 859,007,897 bytes) and decompression is
deterministic, so the read CONTENT is the same on both machines. That points the
residual at a **machine-dependent state in OUR runtime** on the cube/zone-streaming
path — most likely a host-allocation-layout dependency (cf. gotcha 267: a guest
struct on this path may carry a PHYSICAL address that resolves differently depending
where the host allocator placed it, which differs by machine/ASLR). This is the lead
for the next session.

### §5.3 Instruments added (all default OFF, free when off)

- `CZ_APC_TRACE=1` — per-thread APC queued/drained counts + imbalance ages + a
  balanced summary every 10 s (so silence ≠ zero pending). Refuted APC starvation.
- `CZ_KOBJ_DUMP=N` — every N s, each live semaphore's count/max/waiters and each
  waited-on event's state, with last-waiter/last-signaller tids. The only enumeration
  of the live sync objects.
- `CZ_KCALL_WHO` now dumps the guest backtrace on **milestones** too (every 65536th
  hit), which is what names a frozen hot loop's caller.
- `CZ_ARG_PROBE` streaming-chain probes in `guest_probe.cpp`: `sub_82760CF0`
  (UpdateStreaming delta), `sub_82771D70` (dispatch listener count), `sub_827144C0`
  (the status the loader polls), and the stop-message completion chain
  (`82753228`/`82721248`/`827529C0`/`82714510`).

### §5.4 Next session, in order

1. Chase the residual as a physical-address / allocation-layout dependency on the
   cube-slot streaming path: probe the addresses the slot-2/1 request carries on both
   machines and compare (gotcha 267 is the template).
2. The reads stop after the 4th `streamedassets` read — probe the guest routine the
   4th read's completion APC runs (`82831B20`'s target) and see why it does not queue
   the next slot on czamd.
3. Only then publish v1.0.1 (artifacts + hashes staged; the semaphore fix should be
   rebuilt into them once the hang is fully closed).
