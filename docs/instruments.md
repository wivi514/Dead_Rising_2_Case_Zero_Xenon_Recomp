# Runtime instruments and measurement arms

**Split out of `CLAUDE.md` on 2026-08-08** (see `docs/gotchas.md` for why). Every
environment variable this runtime reads, what it measures, and — for the ones that are
ARMS rather than instruments — what it is the control for.

Two distinctions that matter more than the list:

* An **instrument** reports; an **ARM** changes behaviour so a claim can be tested
  against a same-binary control (gotcha 86). Arms announce themselves and must never be
  on for a gate run.
* Several entries record a measured number. Those numbers have a shelf life (gotcha 13)
  and several have already been retracted in place — read the wording, not just the
  figure.

Runtime instruments, all off by default and free when off:
```
CZ_MEM_TRACE=1     every virtual-memory call with its arguments AND its answer
CZ_FILE_TRACE=1    every open/read, including the not-founds. NB every operation on a
                   device that is NOT `game:`/`d:` is logged UNCAPPED without it —
                   saves are rare where disc reads are not, and part 17 could not say
                   whether the save had opened a file because the capped printer had
                   fallen silent thousands of opens earlier (gotcha 109)
CZ_FILE_WRITE_SELFTEST=1  the file layer's own create/write/re-open/read/compare round
                   trip, at startup, on its own device in a temporary directory it then
                   deletes. A POSITIVE CONTROL, not a feature: the only thing this title
                   writes is the save, the save needs a save point, and no headless
                   recipe here reaches one — so without it the write path would ship as
                   a prediction rather than a result (gotcha 67). It checks five
                   independent things including the negative (a write through a
                   read-only handle must FAIL), and it has been shown capable of
                   failing: its first run wrote 303,104 bytes and then could not re-open
                   them, which is how the VFS's cached negative lookups were found
                   (finding 52)
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
CZ_RING_TRACE=1    the ring words once a second, incl. the MMIO dword we do NOT use.
                   Carries `ring: waits unmet=N held=N streak=N max=N` — the brake's
                   own health, where `max` is the longest run of CONSECUTIVE ticks
                   spent on ONE wait. That is the number that separates a title pacing
                   itself (1 on the PM4 arm, 2 on the draw arm) from a ring nothing
                   will ever release (5,491, measured with CZ_ISR_SINGLE_CPU=1). A
                   release COUNT cannot do this job: its discriminator is the stall's
                   address, and phase C re-emits its hand-off block at a FIXED scratch
                   address while the PM4 arm's rotate through the ring, so the same
                   behaviour reads 100% healthy on one arm and 4.9% on the other.
                   ALSO carries `ring: chain ...` — the GPU/CPU hand-off counted link
                   by link (arms -> ints -> isr -> kicks -> walks -> ringsub), plus the
                   number of DISTINCT token-buffer pointers the loop has iterated on and
                   the engine's spin counter printed SIGNED. Read it as a chain of
                   ratios: 0.9997 / 1.000 / 0.523 with walks==kicks==drains is the
                   healthy shape, `distinct=2` with `arms` frozen is a replay. It is
                   what retired the "~300x amplification" (gotchas 161-162)
CZ_VBLANK_MS=N     interrupt cadence (default 16); the control for timing symptoms
CZ_PM4_TICK_MS=N   how often the RING is walked, as opposed to how often the guest sees
                   a vblank. **Default 1; `CZ_PM4_TICK_MS=16` is the control arm**, i.e.
                   the pre-part-18 loop in which the two were the same number. They have
                   no reason to be: the vblank cadence is the title's own frame pacing
                   and must stay at 16 ms (parts 5-6), while the command processor is
                   hardware that runs continuously and only looked periodic because it
                   shared this loop. The walk stops at every unsatisfied WAIT_REG_MEM
                   and resumes on the NEXT tick, so at a 16 ms tick each hand-off wait
                   in a frame cost a whole sleep period: 3.00 ticks/frame and 48 ms of
                   pure sleep, against the 2 vblank periods the title is really waiting
                   for. Worth 84.4 -> 69.9 ms (1.21x), with `submit` identical at 24.0 ms
                   on both arms — the entire delta is sleep and none of it is GPU
CZ_VBLANK_TICKCOUNT=1  deliver the vblank every N loop ITERATIONS again instead of on a
                   steady_clock deadline — the pre-part-18 accounting, and a defect that
                   predates the whole of phase C. An iteration is a sleep PLUS a ring
                   walk, and the walk contains the GPU fence wait, so the guest's vblank
                   was pushed out by however long the GPU took: **40.2/s on the old
                   loop, 31.2/s once the ring ticked at 1 ms, 62.2/s on a deadline.**
                   (A faster ring tick making the vblank SLOWER is the tell.) The
                   coupling that makes it worth 2.0x rather than only fidelity: the CP's
                   per-frame WAIT_REG_MEMs are released by the swap-queue walker inside
                   this very ISR, so a late vblank is a late release is a longer frame.
                   NB with the default on, A1's position-71 window permutes on every
                   run; this flag is how a session that needs the strict 84-prefix gets
                   it back
CZ_PM4_NO_CP_INTERRUPT=1   consume the ring but never raise source 1 (the ISR control).
                   NB it cannot be used to test "is the replay the cause": the boot
                   deadlocks at boot.bct (file #5) because the protocol needs the
                   command-processor interrupt from the first frame (measured, part 7)
CZ_PM4_RESYNC=1    scan past a parser stall instead of reporting it (off on purpose)
CZ_PM4_BIN_TRACE=N  the ME predication inputs for the first N draw packets: the draw's
                   own bin MASK, the SELECT of the tile being rendered, their overlap
                   and whether the packet ran. This title splits its scene across two
                   tiles with those two registers and a THIRD of its draw packets are
                   discarded by them, so a tile that renders almost nothing is either a
                   tile the guest had nothing for or a comparison we get wrong — and
                   only the values say which (phase5-notes §6v)
CZ_PM4_BIN_TRACE_ARM=hex / CZ_PM4_BIN_TRACE_ARMMASK=hex  hold the bin trace's budget
                   until the bin SELECT (or MASK) first takes that value. Not a
                   refinement: the first 300,000 packets of our stream and B1's are
                   packet-IDENTICAL, so a trace armed at the start shows perfect
                   agreement and proves nothing. `ARMMASK=8000000F` lands in the mature
                   tiled era, which is where the two diverge
CZ_PM4_BIN_CENSUS=1  the whole-run (bin mask, bin select) -> offered/skipped table,
                   printed by the ring trace. Deliberately the SAME table
                   tools/xtr_bin_predication.py prints for a capture, because our
                   command processor is the suspect in every question about
                   predication and cannot be its own oracle. Hardware discards 0.3% of
                   draw packets; we discard 33%, all of it one mask value at the right
                   tile (phase5-notes §6w)
CZ_BINMASK_PROBE=1 the guest side of that, and all four inputs to the mask in one
                   flag: the bin-mask setter's caller census; the rect-to-bin-mask PATCH
                   pass (sub_8284A7F8) with a histogram of what it wrote, read BACK out
                   of the records rather than recomputed so the probe cannot merely agree
                   with itself; the pass's TWO INPUTS — the tile rects (`tiles=2
                   tile0=0,0..640,720 tile1=640,0..1280,720`) and a census of the
                   per-record screen extents that produced each mask; and the bin SELECT
                   producer (sub_8284A6D0), which is what `[obj+0x164]` actually holds.
                   Every report is on a 15-SECOND CLOCK, not a call count: the previous
                   version printed at call #1 and then every 20,000, so a subsystem that
                   runs a few thousand times a boot emitted exactly one line — reading
                   "ran 1 time" — and part 10 believed it (gotcha 186)
CZ_PM4_NO_SCREEN_EXTENT=1  do NOT answer the GPU's screen-extent query
                   (EVENT_WRITE_EXT event 0x1A) — i.e. the pre-part-11 command
                   processor, in which 818,507 of those packets a boot did nothing and
                   the guest's own bin-mask fix-up pass intersected uninitialised memory
                   against its tile rects. The same-binary control arm for the right
                   tile: with it on, 76% of records come back "touches no tile" and
                   32.7% of draw packets are discarded; off, 100% come back `8000000F`
                   and 0.28% are discarded, against B1's 0.3%. Applies to gpu/pm4.cpp
                   and gpu/d3d_draw.cpp together, so the two arms stay comparable
CZ_PM4_NO_PREDICATION=1  execute predicated packets anyway. An ARM, and a destructive
                   one: running a packet hardware skipped puts one tile's geometry in
                   another tile's pass and corrupts the state stream with it (a boot
                   with it on renders nothing). It exists so that "this pass had 23
                   draws" and "this pass had 900 and 877 were predicated away" stop
                   being the same picture
CZ_PM4_DRAW_TRACE=1  the raw DRAW_INDX body for the first 24 indexed draws — the
                   instrument that settled which dword carries the index buffer's endian
                   swizzle (the TOP two bits of the SIZE dword, not the low two of the
                   ADDRESS, whose bit 1 is a real address bit)
CZ_PM4_INDEX_ADDR_SWIZZLE=1  read the index swizzle off the address dword's low bits
                   again — the pre-part-9 arm, i.e. exploded geometry radiating from the
                   exact screen centre. Applies to gpu/pm4.cpp and gpu/d3d_draw.cpp
                   together, so the two arms stay comparable
CZ_PM4_FENCE_MONOTONIC=1   refuse any GPU store that moves the engine's fence
                   COMPLETION word backwards. An EXPERIMENT arm, never a fix — hardware
                   re-executes stale EVENT_WRITEs too, and a command processor that
                   second-guesses a packet's value is not a faithful one. It engages
                   hard (5,711 refusals in 90 s on the draw arm, counted on the
                   `ring: engine` line rather than by its own capped print) and the boot
                   freezes identically, which is what RETIRED "the regressing fence word
                   is what blocks the wait" (part 7). Kept as the cheap re-ask after any
                   change to segment routing
CZ_PM4_NO_STOP_ON_WAIT=1   do NOT stall the ring at an unsatisfied WAIT_REG_MEM —
                   i.e. the pre-part-6 command processor, which evaluates each wait
                   once and carries on. **The brake is ON by default since phase C
                   part 6**, because that is what hardware does and because 40 runs
                   say so: PM4 control 2,446 frames +-1 over 10 runs with the swap
                   queue's head equal to its tail 10 of 10, against a queue that
                   OVERFLOWS 10 of 10 free-running; phase C draw 3,614-3,670 frames
                   against a BIMODAL 332..3,451,841. Zero crashes and truncated=0 in
                   all 40. This flag is the same-binary control arm for every one of
                   those claims. (CZ_PM4_STOP_ON_WAIT=1 still works, so recipes
                   written before part 6 keep meaning what they said.) Until part 4
                   the brake was gated on `depth == 0` and could not affect a single
                   one of this title's hand-off waits, all of which are inside
                   INDIRECT BUFFERs — so both of its early retirements measured a
                   no-op (gotcha 151)
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
CZ_AUDIO_TRACE=1   XMA context allocation + EVERY driver frame scanned for its peak
                   amplitude, so "the pump runs" and "the game makes sound" stay
                   separable. Rewritten in part 26, because the first version could not
                   tell SILENCE from BLINDNESS and the whole audio item turns on that
                   distinction: it now counts null frames separately from silent ones,
                   reports the first non-silent frame and the running maximum, prints the
                   guest ADDRESS of each buffer, and SELF-TESTS the scanner at pump start
                   on a synthetic frame of big-endian 0.5f — a scan that reads the wrong
                   byte order reports zeros on any input, which is indistinguishable from
                   silence (gotcha 30). ~~Reads `null=0 non-silent=0
                   maxpeak=0.000000` all the way to gameplay: the mixer hands us real
                   buffers full of zeros.~~ **THAT WAS TRUE UNTIL PHASE A/V AND IS NOT
                   NOW**: with the XMA decoder wired it reads `non-silent=15991 of
                   18433, maxpeak=0.108854` on a plain boot to the title screen. The
                   line also carries the PUMP RATE now — 187.4-187.6 callbacks/s
                   against the 187.5 that 256-sample frames at 48 kHz need, which
                   retires the "our pump has Fable 2's 2% deficit" note below
CZ_VK_NO_CUBE_SNAPSHOT=1  see the renderer section — the control arm for the cube map the
                   title renders itself
CZ_AUDIO_FRAME_US=N  the driver frame period (default 5333 = 256 samples @ 48 kHz)
CZ_NO_AUDIO_PUMP=1 register the client but never invoke its callback — the control
                   arm for every claim about driving the audio callback
CZ_NO_XMA_DECODE=1 **the control arm for sound.** Contexts still allocate, the register
                   file is still published, the pump still runs, nothing decodes — i.e.
                   the runtime exactly as it was for the port's first 28 parts. Every
                   claim of the form "you can now hear X" needs this off-state measured
                   on the SAME binary, and it is what established that the prologue
                   cinematic's freeze was audio: 10,527 frames on one camera fingerprint
                   with it set, 159 without
CZ_NO_AUDIO_OUT=1  keep the whole pipeline — pump, decode, mix — and open no device.
                   Separates "the guest produced audio" from "we played it", and it is
                   what a headless gate run wants: a machine with no sound card must not
                   be a different code path from a desktop with one
CZ_CINE_TIME=<file>  **the cinematic's own CLOCK, one line per frame the guest asks
                   for it.** sub_82475718 returns the time the scene is played at, and
                   sub_82478FC8 stores it straight into [cine+0x1698]; that function is a
                   three-way switch on a mode the image ships as 2 = PID(audio position),
                   the PID being sub_824741D8, which the image names itself by plotting
                   `Cine.Audio P-gain / I-gain / D-gain / MV (ms)` in its own tail.
                   Columns: msec mode playing audioPos ret setpoint acc prevErr integ pid.
                   They are chosen to be REFUTABLE: `mode` never reading 2 kills the PID
                   explanation outright, and `setpoint` oscillating moves the defect to
                   the caller. On the prologue it reads mode 2 throughout, a setpoint
                   climbing linearly, `audioPos` FROZEN at 4.906667 s, and `ret` hunting
                   4.91 <-> 5.27 — which is open-items 00j. **Pair it with
                   CZ_VK_FRAME_STATS and never read it alone**: the value only exists when
                   the guest asks for it, so the probe is driven by its own subject
                   (gotcha 269); every line carries a host clock precisely so a gap next
                   to continuing frames is a measurement rather than an absence of data
CZ_CINE_AUDIO_MODE=0|1|2  **the same-binary arm for 00j, and every setting is a path the
                   TITLE implements** — it writes the mode into the config block the guest
                   has just built, so it invents no code path and cannot be stomped.
                   0 = raw scene time, no audio sync · 1 = scene time := the audio stream
                   position · 2 = the shipped PID. Measured on the prologue: mode 2
                   LOOPING at 15 poses and runs/distinct 120, mode 1 **FROZEN** at the
                   stuck position for 338 s, mode 0 no loop and the run reaches gameplay.
                   **Mode 0 is not a fix and must not be read as one** — with no sync the
                   first call site hands over an uninitialised ~138,181 s scene time and
                   the cinematic ends immediately; mode 2's `if (input == 0) return 0` is
                   what normally protects against that
CZ_XMA_DECODE_LOG=1  per-context decode activity every 5 s, plus each context's format
                   when it goes live and a hex dump + independent `Xma_Validate` of its
                   first packet. The counters are `Nf/pkP/starveS/pktC/refusedR/smpS`,
                   and REFUSED is split from short deliberately: libavcodec rejecting a
                   packet and libavcodec returning fewer samples than a decode frame
                   are different defects with the same silence
CZ_FAKE_START_MS=N synthetic press every N ms. A MEASUREMENT ARM, NOT A
                   FEATURE — it manufactures progress, so it announces itself on
                   every press and must NEVER be on for a gate run (gotcha 78).
                   Kept now that real input exists: it is the control for "was it
                   really my press that moved the boot"
CZ_FAKE_PRESS_SEQ=START,A,A  which buttons that arm sends, one per interval, HOLDING
                   the last rather than wrapping. Unset = START every interval, as
                   before. It exists because everything more than one menu level past
                   the title was unreachable headless and therefore unmeasurable
                   (gotcha 190): with START,A,A the boot walks title -> logo -> menu ->
                   loading screen with no operator. Names: A B X Y START BACK UP DOWN
                   LEFT RIGHT NONE. **NONE is the arm's own control** — mask 0, so a
                   sequence can walk to a screen and then go quiet. Without it the
                   title is being poked every 8 s for the whole run and "it froze
                   here" cannot be told from "something keeps pressing at it"
                   (gotcha 214). It takes about TEN A presses to reach the prologue,
                   so `START,A,A,NONE` merely parks on the title screen.
                   **STICK names walk the WORLD, which buttons never could**: LSUP
                   LSDOWN LSLEFT LSRIGHT move Chuck, RSUP RSDOWN RSLEFT RSRIGHT aim the
                   camera. A stick entry HOLDS for its whole interval where a button
                   entry taps for 150 ms — that difference is the point, not a detail,
                   because a 150 ms nudge of the stick moves Chuck a few centimetres and
                   would read exactly like a stick that does not work. Until this
                   existed, gotcha 190 was only half solved: the menus were reachable
                   headless and the WORLD was not, so every gameplay defect on the board
                   (the gas station's whole-frame black, the magenta sky, the NPC part
                   meshes, the sheared pause menu) was an operator report with no
                   reproduction. An unknown name is now REPORTED rather than dropped —
                   a silent drop shifts every later entry one interval early and
                   desynchronises the recipe from the screens it was aimed at
CZ_XMA_PROBE=1     the guest's own audio state, on a 5 s clock: the IsPlaying
                   predicate (sub_82862A90), the per-context "has it run dry" test
                   (sub_8285EFE0, which reads the input-buffer-VALID bits at
                   dword0 bits 20/21), the per-update edge detector (sub_82864808,
                   counted off voice+0x120 either side of the call), and the raw XMA
                   context words with the hardware kick bitmap beside them. The
                   instrument that turned "there is no XMA decoder" from a statement
                   about our silence into one about what the GUEST observes: nothing
                   here ever clears an input-valid bit, so every voice the title has
                   started is still playing for the life of the process.
                   **Phase A/V widened it to ALL SIXTEEN dwords and added a CONTENT
                   SCAN of the declared buffers**, which is what found the defect:
                   `in0=A2538000 (phys 02538000) 64 pkts (131072 bytes): 0 non-zero`
                   next to `NtReadFile(...) -> 131072 into A2538000` is the whole
                   finding on two lines. Pair it with `CZ_NO_XMA_DECODE=1` when you
                   want the reading to be passive — the decoder retires an input
                   buffer within milliseconds of seeing it, which destroys the
                   evidence this scan is asked about. The dword assignment is not
                   taken on faith either: the guest's own arithmetic confirms it,
                   `dw[8] - dw[7] = 6400 = 25 blocks x 256`
CZ_XMA_NULL_DECODER=1  AN ARM, NOT A FEATURE: a decoder that consumes its input and
                   produces nothing, so voices can finish. Announces itself on every
                   run and must never be on for a gate run. It is what REFUTED the
                   prologue's audio hypothesis — with it on, voices demonstrably
                   start and stop (19 start / 18 stop edges) and the prologue is
                   frame-for-frame identical
CZ_XMA_NULL_DECODER_MS_PER_PKT=N  its rate, in ms of audio per 2048-byte packet
                   (default 40, derived from the contexts' own declared 48 kHz and
                   subframe_decode_count=4). 0 retires the whole buffer instantly,
                   which is a DIFFERENT arm: a voice is then dry before anything can
                   poll it, so IsPlaying reads FALSE always — the opposite extreme
                   from the stock runtime rather than the middle (gotcha 213)
CZ_GUEST_LOG=1     the ENGINE'S OWN debug printf. sub_827877C8 is a vsnprintf with
                   **640 distinct callers** handing its result to sub_828223A0, and
                   hooking that one function makes the title narrate itself. It
                   prints nothing today and that is checked, not assumed — the strong
                   PPC_FUNC is in the object file, so it is the CALL SITES that are
                   gated. **PAIR IT WITH `CZ_GUEST_DIAG=1`, which is the switch** —
                   alone it still prints only the ungated errors. `game:\cl.txt`
                   is NOT the switch: sub_82482E50 reads it as a CHANGELIST NUMBER
CZ_GUEST_DIAG=1    **the engine's whole diagnostic layer, switched back on by one
                   byte.** `0x829EC974` is read by 2,013 sites and written by none,
                   every one `lbz / cmplwi 0 / bne skip / bl sub_827877C8`, and the
                   image ships it as **1** — a release KILL SWITCH, not an unset flag,
                   which is gotcha 215 corrected by gotcha 266. This clears it, and
                   sets `0x82AC3EAD` (592 readers, 2 writers) so the assert sites it
                   un-silences PRINT their file and line instead of reaching their
                   `twui` trap. Pumped rather than poked once, so a title that wrote
                   them back would show as a count above 2.
                   **The null and the positive control are the same run twice**: the
                   DebugJump outdoor route with `CZ_GUEST_LOG=1` alone gives 0 `[guest]`
                   lines of 11,168; adding this gives 1,239. What it unlocks that
                   nothing else here can reach: `<> LoadZoneCommonTextureSet : zone=N,
                   filename=COMMON_TEXTURE{,_LOD}.tex` (the per-zone LOD decision),
                   `Largest free in zombie vertex buffer heap is N, delta -N`,
                   `Queue is full in MoveLoadRequest() priority=%d!`, `Out of memory in
                   the load & decomp heap!`, `[LOAD] <X> took N seconds`, and the two
                   `cZone::UpdatePriorities()` asserts.
                   **A DIAGNOSTIC ARM, NEVER A GATE CONFIGURATION** — two thousand
                   formatting sites on the frame path cost real time, so gotcha 7
                   applies and no frame number may be quoted from a run with it set
CZ_PM4_CONST_WATCH=<hex>[-<hex>]  a per-register value HISTOGRAM for one shader
                   constant register or a range of them, on a 15 s clock. Not a
                   sample: the sampling version read the same registers wrong twice in
                   one session (gotcha 211). It answers "which values does the guest
                   write here, how often" — and a count of ZERO over an era is the
                   finding, which no sampling of the value can produce
CZ_PM4_CONST_WATCH_FRAME=N  hold that report until frame N, because the era that
                   matters is never the boot (gotcha 139)
CZ_PM4_CONST_WATCH_ZEROS=1  restrict it to zero writes — what this instrument did
                   when it only had one job ("who zeroes this register mid-frame")
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
                   the renderer looks up. The input to tools/build_shader_spv.sh.
                   **DO NOT ASK WHICH FILES ARE NEW BY MTIME.** It rewrites every blob it
                   sees, so after a long session almost the whole directory looks freshly
                   written — 250 files inside a 15-minute window on the part-29 operator
                   run, of which NONE were new. The two signals that actually
                   discriminate are the `no translated shader` counter (count UNIQUE
                   lines, not total — one missing shader logs once per bind) and a NAME
                   diff of the dump directory against `assets/shader_spv`. A check that
                   cannot tell "new" from "rewritten" is not a clean result (gotcha 25)
CZ_SHADER_SPV=dir  override the shader cache location
CZ_VK_ROBUST=1     enable robustBufferAccess at device creation. KNOW WHAT IT CANNOT
                   TEST (part 33, gotcha 279's shape): every vertex/index stream is
                   sub-allocated from ONE arena VkBuffer and the bind carries no size,
                   so the robust bound is the whole arena — a fetch past its own stream
                   but inside the arena is exactly as undefined-in-effect as before. Its
                   null on the white plateau (890 px vs a 1,092 baseline) says nothing
                   about per-stream overruns; it bounds only reads past the arena itself
CZ_VK_RANGE_CENSUS=1  per draw: walk the index VALUES against every bound stream's
                   declared size (the standing guard bounds indxOffset + indexCount,
                   which is the number of indices, not the vertices they name), and scan
                   each float-format attribute's in-range bytes — FP32 and FP16 — for
                   NaN patterns. Capped [range] lines name the draw; totals need
                   CZ_VK_STATS. A DIAGNOSTIC ARM: it touches every index and vertex of
                   every draw and visibly slows the run (gotcha 7). Part 33: 786,861
                   draws, zero overruns, zero in-range NaNs — which is what moved the
                   white-plateau question from the streams to the FETCH, where the
                   validation layer then named the type mismatch
XE_FLOOR_IS_NAN / XE_NAN_IN_PAINT / XE_NAN_VS_KILL_IN   the part-33 NaN-tracing family,
                   built via CZ_DXC_DEFINES into their own caches and selected with
                   CZ_SHADER_SPV; each has a _FORCE positive control. In order: flag a
                   NaN OPERAND at any differing-operand max (upstream of the laundering
                   that makes isnan(oC0) blind — gotcha 281); paint any pixel whose
                   INTERPOLANTS arrive NaN (the arriving-vs-manufactured split); cull
                   any triangle whose declared float vertex inputs arrive NaN (the
                   data-vs-VS-arithmetic split). docs/xenonrecomp-upstream-bugs.md has
                   the build lines and the part-33 readings for all three
assets/shader_spv_pre45   not an env var but the same arm shape: the pre-part-45 cache
                   (component-blind interpolant liveness — 217 pixel shaders sampling
                   their diffuse at one texel, §6by / gotcha 316), preserved whole and
                   selected with CZ_SHADER_SPV. The same-binary control for every
                   picture claim the part-45 fix makes
CZ_VK_STATS=N      the renderer's named-counter block every N frames. Every path that
                   declines to draw something has a counter, because a renderer that
                   draws 80% of a frame looks exactly like one that draws all of it
CZ_VK_FRAME_DUMP=dir   every 64th presented frame as a PPM — the renderer checked
                   WITHOUT a window, which is what makes the E-screenshot comparison
                   self-servable instead of an operator task
CZ_VK_SNAP_DUMP=dir    EVERY resolve snapshot of one frame. The frame is the last link
                   in the chain, so a wrong frame is consistent with any pass being
                   wrong; this is the only instrument that says which
CZ_VK_SNAP_FRAME=N which frame that is (default 600). It was a hardcoded 600 for as long
                   as the instrument existed, which was fine while every question was
                   about the title screen and useless the moment one was not
CZ_VK_SNAP_ON_BLACK[=pct]  dump the whole resolve chain of the frame the picture DIED
                   on, triggered BY it dying (default floor 0.5% coverage). The
                   view-dependent whole-frame black happens when a human turns a camera,
                   so CZ_VK_SNAP_FRAME — which fires on a frame NUMBER — could never
                   capture it, and every report of that defect has been the black frame
                   ALONE, which is consistent with every pass being wrong and with
                   exactly one being wrong. It fires on a TRANSITION, not a threshold:
                   this runtime presents plenty of legitimately black frames during boot
                   and loading, so a bare threshold dumps a chain nobody wants.
                   CZ_VK_SNAP_ON_BLACK_LIT=pct is the arming bar (default 20) and
                   CZ_VK_SNAP_ON_BLACK_MAX=N caps total episodes (default 4). The cap is
                   not caution: the instrument's own positive control (floor 99, which
                   fires on essentially every frame) dumped **9,833 PPMs** and refilled a
                   tmpfs whose exhaustion kills this machine's shell. Its POSITIVE
                   CONTROL is `CZ_VK_SNAP_ON_BLACK=99 CZ_VK_SNAP_ON_BLACK_LIT=20
                   CZ_VK_SNAP_FRAME=999999` — four lines, 180 files; run it before
                   believing a run that dumped nothing, because the FIRST version of this
                   trigger could not fire at all and neither could its control
CZ_VK_SNAP_ON_DARK=<meanLuma>  the same trigger on the metric the defect actually moves,
                   and it dumps a BRIGHT REFERENCE chain to sit beside the dark one.
                   SNAP_ON_BLACK fires on COVERAGE, and in a gameplay run the only things
                   that trip a 0.5% coverage floor are loading screens, which are
                   legitimately black. Both are kept: the two thresholds answer different
                   questions, and silently changing what an instrument means would
                   invalidate every run taken with it. The PAIR is the point — one dark
                   chain is consistent with "this pass is broken" and with "the scene
                   really is dark here", and only a bright chain from the same location
                   seconds later separates them (gotcha 133). A dark episode OWES a
                   bright reference and the next frame that re-arms pays it.
                   CZ_VK_SNAP_ON_DARK_LIT=N is the arming bar (default 20),
                   CZ_VK_SNAP_ON_DARK_MAX=N the episode cap (default 3), and
                   CZ_VK_SNAP_ON_DARK_AFTER_MS=N ignores everything before N ms of wall
                   clock — not a refinement, because the boot, the title screen and every
                   loading fade would otherwise spend a budget aimed at gameplay before
                   gameplay starts. Wall time rather than a frame index because the
                   recipes that reach gameplay are written in seconds and a frame index
                   for the same moment moves with the frame rate. POSITIVE CONTROL:
                   `CZ_VK_SNAP_ON_DARK=8 CZ_VK_SNAP_ON_DARK_LIT=20 CZ_VK_SNAP_FRAME=999999`
                   on a plain boot fires at the title screen and prints both lines
CZ_VK_NO_SNAPSHOT_VIEWS=1  serve resolve snapshots at the destination surface's PITCH,
                   which is what every phase before this one did. THE CONTROL ARM for
                   §6ao: a sampler normalises over the image it is handed, the fetch
                   declares the surface's REAL width, and where those differ every
                   texture coordinate is scaled by width/pitch. Invisible on the scene
                   chain (1280/640/320/160 are all multiples of 32) and it emptied the
                   tail of the luminance reduction, whose last link — the 2x1 scene
                   average the tone map reads — was identically ZERO in every frame of
                   every era
CZ_VK_ARENA_MB=N   the per-frame bump arena's STARTING size (default 128; it grows now).
                   The arm that identified the whole-frame black: `ArenaAlloc` SKIPS
                   every draw it cannot satisfy, this title's post chain is at the END of
                   the frame, and its biggest frames are the ones a camera angle produces
                   — so an arena that overruns loses exactly the post chain and presents
                   black with a correctly rendered scene sitting in EDRAM behind it.
                   128 MB: 160 black frames of 8,216 and the high water pinned at the
                   cap. 512 MB: zero of either, true peak 161 MB. Every one of the 160 is
                   the frame after an exhaustion — 160 of 160 (§6ap).
                   `arena EXHAUSTED on frame N` names the frame once per frame, uncapped,
                   because exhaustion is a property of ONE frame and a running total
                   cannot be joined to a frame-stats line
CZ_VK_NO_ARENA_GROWTH=1  pin the arena at its starting size — the exact pre-part-19
                   renderer, and the control arm for the growth. Without it the arena
                   doubles at the next frame boundary whenever a frame overran it, which
                   is the only place it is safe (the command buffer has just been reset,
                   so its previous submission has completed, and every consumer of the
                   arena is per-frame)
CZ_VK_TEX_GUARD=1  a CONTENT guard over the texture cache: on a cache hit, hash a bounded
                   sample of the guest bytes the image was built from and compare with the
                   hash taken at upload. A mismatch is the cache serving pixels that are
                   gone. Counted globally and per address (worst 24 printed), because "one
                   atlas is stale every frame" and "a third of the world is wrong" are
                   different defects. **It refuted the hypothesis it was built for** —
                   0.00% on an operator session with wrong textures on screen throughout —
                   and is kept because it is the only thing that can see a stale texture,
                   and because it is two-sided (see the poison below). Needs CZ_VK_STATS=N
                   to print anything at all. NB blind by construction to a fetch pointing
                   at an address that was never that texture's home
CZ_VK_TEX_GUARD_BYTES=N  the bound the TEXTURE content guard folds at, in bytes.
                   Default 16384 — which is what it has silently been since part 38,
                   because it borrowed the STREAM guard's constant; the two were never
                   chosen separately and they are different questions. A stream guard is
                   hunting a small edit inside a batched UI buffer (item 00c). A texture
                   guard is hunting an address the STREAMING SYSTEM RECYCLED — a
                   different texture written over the old one, which the eight spread
                   blocks see at essentially any bound. The `texture guard bytes by
                   SOURCE size` histogram printed with CZ_VK_STATS says what each bound
                   would cost, and the per-address `changed` table (which now carries
                   each texture's source size) says what it would stop being able to see:
                   pick the bound off those two rather than off an argument
CZ_VK_TEX_GUARD_POISON=1  the POSITIVE control: perturbs only the COMPUTED guard, never
                   the stored one, so every hit must mismatch. Measured 14,554,550 of
                   14,554,550 = 100.00%, against 0.00% on an unpoisoned boot. A census that
                   cannot report a positive proves nothing by reporting a negative
                   (gotcha 30), and this project has shipped a comparison that could only
                   ever read 100% (gotcha 234) — so both ends need exercising
CZ_VK_TEX_REVALIDATE=1  **THE DEFAULT since part 38** (the flag remains accepted): re-upload
                   on mismatch into the SAME image and SAME bindless slot, exact and
                   allocation-free because the dimensions are part of the key. The part-35
                   note "repaired all 1,968 stale hits without changing any visible defect"
                   was TRUE OF THAT SESSION's defects (the lightmap-UV blotches, fixed in
                   part 37 — never stale-cache); the part-38 operator evening showed the
                   class this IS the fix for: streaming recycles addresses all session and
                   the once-only cache dressed the tanker in a brick wall ("almost
                   everything up close wears a random texture"). Field-tested a full
                   evening: every prop correct, no reported slowdown. §6bp, gotcha 293.
CZ_VK_NO_TEX_REVALIDATE=1  the same-binary control arm: the pre-part-38 once-only cache,
                   which brings the random-texture class back. **PART 47 RAN THE OWED
                   A/B AND THE GUARD IS ALMOST THE WHOLE TEXTURE PHASE**: on the outdoor
                   route `textures` is 40.8% of the frame with it and **7.8% without**,
                   at MORE draws — i.e. the upper bound on the revalidation item is far
                   above the 8-11 ms `docs/perf-plan-part47.md` §1.1 estimated. Keep
                   using it as the upper bound for any guard-cost question; it is not a
                   shippable configuration, because it is the defect part 38 fixed
CZ_VK_TEX_GUARD_EVERY_FETCH=1  the pre-part-47 CADENCE: revalidate on every fetch rather
                   than once per frame per cache entry. `UploadTexture` runs once per
                   texture fetch per draw, so a texture many draws share was re-hashed
                   once for each of them — which is where 92.9 MB a frame went to catch
                   0.0037% of anything. The counter that proves the policy engaged is the
                   `texture guard cadence:` line, which reports the skipped share and the
                   redundancy factor; under this arm it must read 0. **The falsifiable
                   claim is that `changed` does not fall between the two arms**: a real
                   change is still there at the next frame's first fetch, so a drop would
                   mean the policy is losing detections and the number says how many.
                   What it costs is at most one frame of staleness for a texture the
                   guest rewrites mid-frame
CZ_VK_TEX_REFRESH_ALL=1  re-read EVERY texture on EVERY fetch. Ruinously slow; the cache
                   cannot serve a stale image under it, so it is the picture arm that
                   would have proved the cache guilty had it been guilty
CZ_AUTOCHUCK=<schedule>  hand Chuck to the title's own debug AI with no menu navigation.
                   `EXPLORER`, or a SCHEDULE like `ITEM PICKER@0,ZOMBIE KILLER@180` where
                   @seconds runs from the AI first engaging ON THIS LEVEL (so it means the
                   same thing however long the boot took). Names are case- and
                   space-insensitive; 0..6 and OFF also work; anything else is refused with
                   the list. **Replaces the F4 overlay, which is driven by SDL keyboard
                   events and is therefore unreachable headlessly** — this drives the same
                   guest writes the menu item does.
                   **IT RE-ASSERTS, because the title's AI changes the state underneath
                   you.** Measured: we wrote ITEM PICKER twice and a live read of the
                   running process found MISSION MASTER, stable. That is why every state
                   looked identical from the outside — each one had become MISSION MASTER
                   and was waiting at the ambulance objective. The override count is logged
                   (3 in an 8,657-frame run, so the AI decides once rather than fighting
                   per frame). `CZ_AUTOCHUCK_NO_HOLD=1` is the control arm
CZ_AUTOCHUCK_CLOSE_HASHES=hex,hex   the screens to close automatically while an AutoChuck
                   state is held; empty disables. Defaults to `06903E1A,890DF3E5`, which is
                   THE MAP — measured with CZ_SCREEN_TRACE, because AutoChuck opens it about
                   two minutes into a roam and parks an unattended run on it. The close is a
                   **B** press, not BACK: BACK is what OPENS the map, so pressing it again
                   asks for a screen that is already open. One press per screen rather than
                   per transition (the map fires two hashes at once), and only while
                   AutoChuck holds, so a human who opened the map keeps it
CZ_AUTOCHUCK_CLOSE_DELAY_MS=N   how long to let that screen settle before pressing B
                   (default 1200) and it is the operator's observation that a close on the
                   opening frame is not accepted. NB building this window exposed that the
                   helper it used had SECONDS resolution, so the delay quantised to 0 or
                   1000 ms — a timing window needs a clock finer than the window
CZ_SCREEN_TRACE=1  every frontend screen transition the TITLE makes, by hash, with a
                   timestamp and a FIRST TIME marker. It is how the map was identified:
                   nothing else in this runtime can see the game changing screen on its own,
                   and our own DebugJump request bypasses the hook (it calls the impl
                   directly), so everything this prints is the title acting by itself
CZ_VK_NO_CUBE=1    bind every CUBE fetch the way the renderer did before part 25: publish
                   its descriptor index into the `Texture2D` array, leaving the
                   `TextureCube` array at zero so the shader samples the 1x1 white dummy.
                   **The same-binary control arm for the cube-map fix** (open item 00),
                   and the only way to reproduce four sessions' worth of screenshots in
                   which every reflective surface multiplied its specular by pure white.
                   Counted, so an arm that engaged is distinguishable from one that did
                   not (gotcha 151)
CZ_VK_NO_CUBE_SNAPSHOT=1  decline the cube map the TITLE RENDERS ITSELF to the 1x1 white
                   dummy — i.e. the part-25 renderer, in the part-26 binary. **The
                   same-binary control arm for the cube snapshot path.** By default
                   `06805000` is assembled from the six resolve snapshots at
                   `06805000 + i * 0x4000` into a six-layer cube image in set 2 and
                   refreshed by each face's own resolve; with this on, every one of those
                   fetches reads white again. Both arms are counted, and on a 240 s
                   DebugJump run the path serves **358,767 of 999,508 cube fetches (35.9%)**
CZ_VK_CUBE_FROM_GUEST=1  upload a cube map from guest memory even when its address is a
                   RESOLVE DESTINATION. Exactly one of this title's cube maps is such an
                   address — `06805000`, 64x64, which the title renders itself — so the arm
                   shows that surface as a BLACK reflection (the zeros actually in guest
                   memory) against the assembled snapshot cube the renderer now builds. It
                   predates the snapshot path and is kept because it is the third point of
                   comparison: black (guest memory), white (`CZ_VK_NO_CUBE_SNAPSHOT`), and
                   the rendered map
CZ_VK_PS_CONST_SCALE="14.w=4,18.y=0.5"   multiply chosen PIXEL shader ALU constant
                   components by a factor, applied to the per-draw copy after it is made
                   and before any draw reads it. Added in part 30 as a MAGNIFYING GLASS
                   for the white-surface item, and the reasoning generalises: the tone
                   curve every material shader ends in has a vanishing derivative at
                   `x = colour * pc(14).w = 1`, so a 10% spread in the colour quantises to
                   ONE 8-bit value there and the picture cannot tell a pinned colour from
                   a shaded one (gotcha 273). `14.w=4` moves the same surfaces to a part
                   of the curve where that spread is ~7 levels: a plateau that stays a
                   single spike is pinned, one that spreads was being hidden by the curve.
                   It SCALES rather than sets, because this title's exposure is scene
                   adaptive and reads 0.2 to 1.0 across draws of one run — a fixed value
                   would manufacture the very uniformity the arm tests for. Every clause
                   it parses is echoed at start-up and every clause it cannot parse is
                   named; the draw counter `a PIXEL constant was scaled` is how the arm is
                   shown to have engaged.
                   **PART 31 USED IT AND GOT A REFUTATION, so read what it can say
                   before reaching for it.** `14.w=0.25` engaged on 11,835,619 draws and
                   took the scene buffer from mean luma 35.07 to 18.30, and the white
                   plateau did not move off 180 by a single pixel. A whole-frame input
                   perturbation can say "this input does not reach those pixels"; it
                   cannot say which draw does (`docs/phase5-notes.md` §6be)
CZ_VK_EXPOSURE_TRACE=file  one line per frame: `frame draws expMin expMax`, the spread of
                   the title's own exposure scalar `pc(14).w` over the draws of that
                   frame, recorded BEFORE any arm perturbs it. It exists because
                   `CZ_VK_PSBIND` — the only other way to read that number — dedupes on a
                   key containing the constants and caps at 64 lines, so a scalar that
                   drifts by 1e-4 a frame spends the whole budget in the first few hundred
                   frames and can say nothing about the frame a snapshot was taken on.
                   MIN AND MAX rather than one value: whether one exposure is in force for
                   a whole frame decides whether a whole-frame histogram is invertible at
                   all, and that had been assumed for four parts. It is not (frame 3000 of
                   the outdoor route: 6,116 draws, 0.214622 to 0.214647)
CZ_VK_NO_ADDR_TILE_FOLD=1  the same-binary control arm for part 31's resolve fix. Off, a
                   resolve whose destination ADDRESS is a macro-tile offset into a larger
                   surface becomes its own snapshot; on the default path it folds into
                   that surface at the decoded (x, y). This title packs four shadow
                   cascades into one 4096x1024 atlas that way, and the arm is what makes
                   the fix measurable: `1439B000` reads 53.125% non-zero across all 4,096
                   columns with the fold and 13.281% across columns 0..1023 without, one
                   atlas against four (`docs/phase5-notes.md` §6bc)
CZ_VK_DIM_CENSUS=1  WHERE THE DIMENSION LIVES IN A TEXTURE FETCH CONSTANT, answered by
                   measurement rather than recollection. The shader-declared dimension
                   (from the sidecar) partitions every fetch into classes that must
                   differ in exactly the bits of that field, so this accumulates the AND
                   and the OR of all six dwords per class and prints the bits on which
                   the classes SEPARATE. Over 842,556 2D and 47,574 cube fetches it named
                   two: dword5 bit 10 and dword2 bits 26/28. dword5 bits 9..10 read 1 for
                   every 2D fetch and 3 for every cube one — the TextureDimension
                   encoding — and dword2's top six bits read 5 for every cube and 0 for
                   every 2D, which is the stack depth stored minus one, i.e. six faces.
                   **The second was a prediction stated before the run**, from Xenia's
                   published layout, so the run could refute it. My recollection had put
                   the dimension at bits 7..8 and was wrong; nothing but this would have
                   caught that, because a wrong dimension does not fail, it produces a
                   plausible wrong image. Needs `CZ_VK_STATS=N` to print.
                   **Reusable verbatim for Case West**, and for any field whose value is
                   predicted by an independent oracle
CZ_VK_DIM_DISAGREE=N  WHICH SHADERS DISAGREE WITH THEIR OWN FETCH CONSTANTS ABOUT THE
                   DIMENSION, and about which texture. The cross-check added with
                   `CZ_VK_DIM_CENSUS` says a disagreement HAPPENED and declines that
                   fetch to the 1x1 white dummy; this says who, where and what. Two
                   outputs, and the second is the one to read:
                   * the first N disagreements printed as they happen, each with the six
                     raw dwords of the offending slot AND the whole 32-slot fetch
                     constant file as we hold it at that draw — which is what separates
                     "our register file lost a constant" from "our decode misreads a
                     case", because a lost constant leaves the slot reading as some
                     neighbour's texture and a decode error leaves a slot somewhere that
                     does read cube;
                   * an UNBOUNDED census at `CZ_VK_STATS` time, keyed on (pixel shader,
                     slot, texture address). The first version had only the capped print
                     and every one of its 25 lines was the same shader at the same slot
                     on two frames, which reads as "there is one case" and is equally
                     consistent with the cap having been reached inside one draw batch
                     (gotcha 109).
                   **What it found in part 27**: 9 distinct cases over a 400 s outdoor
                   run, two textures, and slot 4 holding an EXACT DUPLICATE of slot 3
                   where the captures show hardware holding a real 128x128 DXT1 cube map
                   for the same shader pair. Needs `CZ_VK_STATS=N` to print the census.
                   The companion on the capture side is `tools/xtr_cube_agreement.py`:
                   a shader that disagrees here and agrees there is our register file,
                   and one that appears in no capture is a case hardware has never been
                   asked about
CZ_VK_FRAMES_IN_FLIGHT=N  how many frames the CPU may be ahead of the GPU. **Default 2
                   since part 23; `=1` is the pre-part-23 renderer exactly** — submit,
                   block on the fence, read back, present — and is therefore this
                   change's control arm out of the same binary. Max 3.
                   What it duplicates is only what the CPU writes and the GPU reads, or
                   the reverse: the command buffer, the bump arena (cut into N regions of
                   ONE buffer, allocated N times larger so a frame's own capacity is
                   unchanged) and the buffer the presented image is read back into.
                   Everything else is device-only and one queue executes in order.
                   **The counter that says it engaged is `submit [... gpu N]` in the
                   profile** — the fence wait. It is the renderer's "time blocked on the
                   GPU" and the whole prediction is that it collapses while every
                   draw-path column stays put; if it does not move, the frames are not
                   overlapping and nothing else in the profile is worth reading
                   (gotcha 151).
                   Costs ONE FRAME OF LATENCY: the window shows frame N-1 while the CPU
                   records frame N. Each slot therefore carries its own frame number,
                   draw count and fingerprints, captured at submit and read at present,
                   so `CZ_VK_FRAME_STATS` and the PPM dump stay aligned with the pixels
                   they describe.
                   **Three instruments FORCE it back to 1 and say so**, because they read
                   the LIVE resolve chain next to the presented pixels and cannot be
                   frame-aligned under a deferred present: `CZ_VK_SNAP_ON_BLACK`,
                   `CZ_VK_SNAP_ON_DARK`, `CZ_VK_FRAME_STATS_SURFACE` (and
                   `CZ_VK_SNAP_DUMP`, and `CZ_VK_NO_SUBMIT`, which has no fence to wait
                   on). That means `tools/frame_compare.py`'s surface metric always
                   measures the N=1 renderer — which is sound only because the two arms'
                   pictures are the same, and that is checked rather than assumed
CZ_VK_PROFILE=N    the frame's CPU time by phase, every N SECONDS (a clock, not a frame
                   count — a per-N-frames report samples a different amount of wall time
                   in every era and averages the boot's fast frames with gameplay's slow
                   ones, gotcha 186). `draw` wraps ALL of DoDraw so the renderer and
                   everything else separate by subtraction; `submit` is the honest check
                   on the whole table, being the wait for the GPU. **Gameplay, ~1,900
                   draws, 87 ms/frame: draw 8.6% [constants 0.5 streams 2.2 textures 1.0
                   record 5.0 other 0.0] submit 32.6% readback 0.4% outside 58.5%**, at
                   134% process CPU — one saturated thread. So the frame is GPU wait plus
                   guest/PM4 code and the ENTIRE renderer is under a tenth of it; making
                   the renderer instant buys ~1.09x. Read this before planning renderer
                   optimisation — the per-draw constant upload is the obvious suspect
                   (8 KB x 1,900 draws = ~19 MB a frame) and is 0.5%.
                   **Part 18 added the two lines that make `outside` readable**, and it
                   was 92% a `sleep_for`. `submit` is split into `[call gpu]` — the
                   driver translating a 1,900-draw command buffer is **0.1%** of the
                   frame and the fence wait is all of it — and a second `[vkprof] pump`
                   line reports the graphics pump's own ticks/frame, sleep, walk and
                   vblank-ISR shares (gpu/pump_stats.h). The renderer runs on the pump's
                   thread, so everything that thread does between two presents lands in
                   `outside`, including a sleep no cycles profile can see. Gameplay on
                   the current binary: **~34 ms/frame, ~29 fps**, pump 6-9 ticks/frame,
                   sleep ~22%, gpu ~42%, draw ~22%.
                   **PART 20 FIXED THIS INSTRUMENT AND EVERY NUMBER ABOVE IT IS FROM THE
                   BROKEN VERSION.** `ProfScope` accumulated INCLUSIVE time and the
                   scopes nest — `record` opens partway down `DoDraw` and lives to the
                   end of it, so the `UploadStream` calls below it ran inside it and
                   their cost landed in `streams` AND in `record`; `submit` enclosed
                   `submitCall` and `fenceWait` the same way. The print then derived
                   DoDraw's residual by a subtraction that removed `streams` twice, so
                   `record` was overstated by the whole of `streams` and `other` was
                   understated by it. `other 0.0` above is that defect, not a fact.
                   Every scope is now EXCLUSIVE of its children and the whole-draw total
                   is a SUM of the columns rather than a separately measured quantity
                   (gotcha 228). Corrected crowd frame, 6,876 draws, 48.0 ms, GPU at P8:
                   **draw 28.4% [constants 2.7 streams 7.0 textures 6.8 record 6.4 other
                   5.4] submit 37.6% readback 0.7% outside 33.4%**.
                   The pump line gains `[pm4 N]` — the command processor's OWN cost,
                   i.e. `walk` minus the renderer, which `walk` contains because the
                   draws are called from inside it. And a third line counts what the
                   walk was walking: **90,316 packets a frame at 138 ns each, carrying
                   815,020 register-write dwords at 15.3 ns each** — which is the whole
                   of the walk's 12.5 ms and turns `docs/perf-cpu-plan.md` §2 from a
                   suspicion into a target. Counted once per PACKET from its body count,
                   never per `WriteRegister` call, so the census cannot become the thing
                   it measures (gotcha 7)
CZ_VK_PROFILE + tools/gpu_clock_sample.py  **sample the GPU's CLOCK and QUOTE it
                   before believing any `submit` figure — and do NOT pin it.** The
                   P8/210 MHz reading this entry used to carry, and the
                   `sudo nvidia-smi -lgc 2100,2100` it recommended, came from an
                   overnight session with the MONITOR ASLEEP; §6al recorded
                   `display_active: Disabled` beside its own result and guessed as much
                   without being able to test it. Re-measured in part 20 with the
                   display awake, over a full 620 s crowd run: **P5 in 182 of 200
                   samples, clock mean 524 MHz (min 210, max 630), utilisation mean 32%
                   (max 62%), 28.6 W**. `vkcube` — an ordinary presenting Vulkan
                   application — settles in the same place on the same machine (P5,
                   510-600 MHz, 33-39%, 29.5 W), which is the control that was never
                   run. **The governor was never mistreating us.**
                   Read clock and utilisation TOGETHER (gotcha 231). A low clock at LOW
                   utilisation is the governor being right — the GPU is idle 68% of
                   every frame because the renderer submits and then blocks on the
                   fence, so our CPU and our GPU never run at the same time. Pinning to
                   2100 MHz costs 52.8 W against 28.6 to finish work the frame is not
                   waiting on. The fix is to stop being idle, not to raise the clock,
                   and that revives the overnight plan's §2a (overlap the GPU with the
                   CPU) which §6al dismissed on the strength of the artifact
CZ_VK_READBACK_UNCACHED=1  allocate the readback buffer HOST_VISIBLE|HOST_COHERENT only,
                   i.e. the pre-part-17 WRITE-COMBINED buffer. FindMemoryType returns the
                   FIRST type matching its mask, and on a discrete GPU that one is
                   write-combined — right for every other mapped buffer here, which are
                   CPU-write-only, and exactly wrong for the one buffer the CPU READS.
                   With it on, presenting a frame reads 3.7 MB back uncached at ~230 MB/s:
                   readback 15.7% of a 103 ms frame against 0.4% of an 87 ms one, 9.7 fps
                   against 11.5 (operator, windowed: ~10 against ~15)
CZ_VK_FRAME_DUMP_EVERY=N  the frame-dump interval (default 64). The save-slot panel the
                   synthetic-input arm walks THROUGH appears in exactly ONE frame of a
                   180 s boot at 64
CZ_VK_SKIP_TEX / CZ_VK_ONLY_TEX=<hex[,hex]>  render all but, or only, the draws whose
                   first bound texture is at that guest address. The bisection arm one
                   level below CZ_VK_ONLY_VS, because a UI compose is a hundred quads
                   sharing two shaders and the SHADER is not what distinguishes them.
                   This is how a rectangle on screen gets an identity: skip an address
                   and look at what vanished. `CZ_VK_SKIP_TEX=0364B000` deletes the
                   new-game screen's three black panels and reveals three correct
                   thumbnails underneath
CZ_VK_TEX_CENSUS=1 per texture ADDRESS: uploads, how many came out entirely black,
                   fetches served from a resolve snapshot, and fetches that fell back
                   because the snapshot was too old. Off by default because the snapshot
                   column is hit ~500,000 times a run (gotcha 7).
                   **NEEDS `CZ_VK_STATS=N` TOO, AND SAYS NOTHING WITHOUT IT.** The table
                   is printed by `VkRenderer_DumpStats`, which is only called every N
                   frames when CZ_VK_STATS is set — there is no dump at exit, and every
                   headless recipe in this project runs under `timeout`, which SIGTERMs
                   the process. On its own this variable turns the counting ON and prints
                   it NOWHERE, which cost part 22 a 10-minute run that looked like a
                   census with no textures in it (gotcha 236)
CZ_VK_STREAM_CENSUS=1|2  what the per-frame vertex/index stream cache actually DOES:
                   lookups and hit rate, misses, MB copied per frame, MB the hits saved,
                   the split by kind (declared vertex binding / index buffer / shader-side
                   dependent fetch), and — the number that decides the fix — the share of
                   MISSED bytes whose (address, size, endian) key repeats the PREVIOUS
                   frame. Reports through the CZ_VK_PROFILE window and says so if that is
                   off. Level 2 additionally hashes each stream's guest bytes and reports
                   whether the repeated keys' CONTENT was unchanged, which is what a
                   cross-frame cache stands or falls on — it costs about what the copy
                   costs, so level 2 is a diagnostic run and never a frame-time
                   measurement (gotcha 223). This settled perf-cpu-plan §1b, which had
                   two opposite candidate fixes: at ~6,400 draws it reads 94% hit and
                   STILL 74-77 MB copied a frame, 95-97% of which repeats last frame's key
CZ_VK_STREAM_CENSUS_POISON=1  the CONTROL for the line above, and the reason its answer
                   is believable. The content check reads 100.0% and nothing else, which
                   is either a real fact about this title's geometry or a comparison that
                   cannot fail — indistinguishable from the output. This salts the hash
                   with the FRAME NUMBER, so identical bytes must hash differently and the
                   line MUST read 0.0%. Measured on one binary: 75,492 of 75,492 (100.0%)
                   off, 0 of 96,048 (0.0%) on. It also exposed what the rounding hid —
                   164 of 10,154,820 repeated keys really do change content, a recurring
                   set of ~26 — which turns "safe to cache blindly" into "must invalidate".
                   With the store built, poison ALSO drives the GUARD MISSED line to its
                   maximum (240,652 of 240,652), which is that line's own control
                   -- level 2 also prints REWRITTEN IN PLACE: WHICH keys change, cumulative
                   over the run, with address, size, kind and the frame span. That is what
                   chose the invalidation mechanism in part 22: all 30 are exactly 80
                   bytes and all are declared vertex bindings, so a 512-byte exact guard
                   covers the whole observed population and `mprotect` was not needed
                   -- and GUARD MISSED: of the content changes the FULL hash sees, how
                   many the store's bounded-cost guard let through as a hit. A stale
                   vertex buffer handed to a draw. Must read 0; reads 0
CZ_VK_NO_PERSIST_STREAMS=1  the CONTROL ARM for the cross-frame stream store — the same
                   binary with the store off, which is the pre-part-22 renderer. The store
                   is ON by default because it is a 97%-of-first-touch-streams saving;
                   this is how one binary is both arms of its own A/B, the same shape as
                   CZ_VK_NO_ARENA_GROWTH and CZ_VK_NO_SUBMIT
CZ_VK_PERSIST_MB=N  the cross-frame store's STARTING size (default 128; it doubles when a
                   frame overruns it, like the arena, and settles at 256 in a crowd).
                   Two `[vkprof] store` lines report it whenever CZ_VK_PROFILE is on and
                   are NOT behind the census: the share of first-touch streams served
                   across the frame boundary, the MB/frame thereby not copied, fills,
                   **stale** (the guard caught a rewrite and re-copied — ~20 a frame, see
                   gotcha 235), overflow, what the guard itself read, and entries / MB
                   used / flushes. A store that silently served stale data would look like
                   a rendering bug frames later, so its counters are on by default
CZ_VK_TEX_REFRESH=<hex[,hex]>  re-read those textures' pixels on EVERY fetch, into the
                   SAME image and slot (the dimensions are part of the cache key, so
                   updating in place is exact and needs no allocation). The arm for "we
                   cached a texture the guest is still writing" — and on the garbled
                   glyph atlas it engages 2,250 times and changes nothing
CZ_VK_TEX_DUMP=dir + CZ_VK_TEX_DUMP_ADDR=<hex[,hex]>  the UNTILED bytes of a texture as
                   a greyscale PGM for an 8-bit format, and as a raw `.bin` of the block
                   payload for everything else. It separates "our untiling scrambled
                   this" from "the texture is fine and the draw samples it wrong", which
                   are different subsystems — and a human can tell a page of glyphs from
                   a page of noise instantly, which no aggregate over it can.
                   **The .bin half is part 40's and it closed a hole this instrument had
                   from birth**: it was gated on a one-byte unit, i.e. it could dump only
                   8-bit textures, and this title is DXT nearly everywhere — so the one
                   instrument whose job is "did we scramble this texture" was blind to
                   every format that carries the picture. Decode with
                   `tools/tex_decode.py --fmt <n>` (the dump is already untiled and
                   endian-swapped, so NOT --tiled and NOT --swap16). With no
                   CZ_VK_TEX_DUMP_ADDR it dumps every uploaded texture, which is ~900
                   files on the outdoor route and is the cheapest way to eyeball the
                   whole bank at once
CZ_VK_TEX_DUMP=dir + CZ_VK_TEX_DUMP_PS=<ps hash[,hash]>  the raw guest bytes of every
                   texture the draws using that PIXEL SHADER sample, once per address,
                   written TILED exactly as guest memory holds them (so they can be
                   diffed against a capture's own MemoryRead with neither side having
                   decoded first). Decode with `tools/tex_decode.py --tiled --swap16
                   --pitchblk N` — the filename carries the extent, the format, the
                   tiled flag, the pitch and the endian.
                   **THE SHADER IS THE HANDLE THAT SURVIVES A REBOOT AND THE ADDRESS IS
                   NOT.** Part 39 named the foliage material from an operator capture,
                   took its six texture addresses, replayed the route headlessly with
                   CZ_VK_TEX_DUMP_ADDR pointed at them, and got back a picture of BARBED
                   WIRE: a guest address is a fact about one boot's streaming heap, while
                   the shader hash is a hash of the microcode and is the same in every
                   boot. This is the variable to reach for whenever a defect was seen by
                   an operator and has to be reproduced by a headless run
CZ_VK_RESOLVE_TRACE=1  each resolve's destination, SOURCE (colour or DEPTH), extent and
                   clear bits, against the front buffer VdSwap named. The trace that
                   found finding 5 below
CZ_VK_SMALL_EDRAM=1  the pre-part-14 EDRAM stand-in: 1280x720 instead of 1280x1024, and
                   snapshots clamped to it rather than sized to the destination SURFACE.
                   Both numbers used to be the presented frame's, which is right for
                   every pass that happens to be screen-sized and wrong for the one that
                   is not — this title's shadow cascade is declared 4096x1024 and fetched
                   629,023 times a boot, and it was being stored 1280x720 with its bottom
                   304 rows never rendered at all. NB the fix is committed on MECHANISM
                   (a 4096x1024 texture cannot be sampled out of a 1280x720 image), not
                   on a measured picture improvement — shadows still do not appear
CZ_VK_NO_DEPTH_FETCH=1  serve EVERY depth-format fetch the 1x1 white dummy, i.e. nothing
                   is occluded anywhere. An ARM for "is this dark mark a shadow or the
                   surface's own texture", and deliberately NOT called "no shadow": this
                   title has two depth consumers and it hits both, so it also re-blurs
                   the whole frame exactly as the pre-part-14 renderer did (which is a
                   free second confirmation of §6ae). To isolate one consumer, name its
                   address with CZ_VK_SKIP_TEX
CZ_VK_NO_DEPTH_RESOLVE=1  snapshot the COLOUR target even for a resolve whose
                   RB_COPY_CONTROL selects the DEPTH buffer — i.e. the pre-part-14
                   renderer, in which 18.4% of this title's resolves (its three shadow
                   cascades and its scene depth) delivered the wrong picture and the
                   depth-of-field pass computed a circle of confusion out of the scene's
                   own colour. With it on the WHOLE FRAME is uniformly out of focus at
                   every depth, the community-watch sign and `POP 753` are unreadable and
                   the street bunting is gone. NB no aggregate over pixel VALUES can see
                   this (coverage moves 0.01 pp): use tools/frame_sharpness.py, which
                   reads 1.19/1.20 with it on against 7.64/7.67 with it off
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
                   tools/frame_compare.py. The last column is **msec**, wall time since
                   the first measured frame — APPENDED, so every column index the
                   existing tools read is unchanged. It exists because the frame rate of
                   an ERA was not a number this project could state: every frame-rate
                   figure it owned divided a whole run's frames by its wall time, which
                   for a run that boots, walks four menus, loads and only then plays is
                   an average over eras differing by more than the effect anyone wants
                   to measure. "8-12 fps in gameplay" was an operator's stopwatch
CZ_VK_FRAME_STATS_SURFACE=hex  ALSO measure that resolve surface each frame. Not a
                   refinement — the metric does not work without it, because the
                   PRESENTED frame at the title screen is mostly UI and a change
                   touching 476,858 draws moved it 0.1 pp. **The scene colour is
                   0684B000.** It was quoted as 06BE4000 from phase 5 to part 13 and
                   that address is the scene DEPTH — it held colour pixels only because
                   our resolve copied the colour buffer for depth resolves too (part
                   14). Both tiles of each: colour 0684B000/0685F000, depth
                   06BE4000/06BF8000
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
CZ_VK_TEX_CACHE_FIRST=1    consult the fetch-constant texture cache BEFORE the resolve
                   snapshot — the pre-part-9 lookup order, which freezes a surface at
                   whatever guest memory held the first time it was fetched. Reproduces
                   the black scene exactly (2.31% non-black against 99.4%)
CZ_VK_SNAPSHOT_MAX_AGE=N   how many frames old a resolve snapshot may be and still be
                   served (default 0 = NO limit). **1 is the pre-part-15 renderer**, in
                   which a snapshot had to have been taken this frame or last. That is
                   right for the case the mechanism was written for — a post pass reading
                   what an earlier pass in the SAME frame resolved — and silently wrong
                   for a surface the title resolves ONCE and samples forever. The title
                   screen re-renders all three colour-grading LUTs every frame so the
                   window never bound; the prologue's grade is static, so the LUT fetch
                   fell out of the snapshot path into guest memory, which is zero, and
                   §6s already proved a black LUT is a black frame. With the guest's own
                   fade patched out to make the frame visible at all, the prologue reads
                   0.00% non-black at MAX_AGE=1 against 99.99% with no limit
CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1  map a window-coordinate draw (VTE disabled) through
                   the PRESENTED FRAME's 1280x720 rather than the EDRAM's 1280x1024 —
                   the pre-part-15 renderer. The arithmetic is an identity either way,
                   so nothing looks wrong; the CLIP is not, so every window-coordinate
                   draw taller than the screen was cut off at row 719. This title clears
                   its shadow cascade with a (960,0)-(1024,1024) rect, and that strip was
                   losing its bottom 304 rows: cascade non-black 12.82% -> 13.28%, which
                   is 64x304 pixels to the pixel
CZ_VK_RECT_HALF=1  expand a rectangle list to the SAME TRIANGLE TWICE again, i.e. the
                   pre-part-9 half-covered per-pass clear. Half of every depth clear
                   missing means the previous pass's depth rejects the scene behind it
CZ_VK_NO_MSAA_WINDOW_SCALE=1  map window coordinates one-to-one on a 4x MSAA surface —
                   the pre-part-9 behaviour, in which the scene tile's clear covers 320
                   of its 640 columns. Also disables the (never-yet-exercised) tile
                   origin correction
CZ_VK_NO_DEPTH_TEST=1  draw everything regardless of depth. An ARM, never a fix: it is
                   the only cheap way to separate "this geometry was never submitted"
                   from "this geometry was submitted and rejected by depth left over
                   from another pass", which look identical in a snapshot.
                   **USELESS ON A DEPTH-ONLY PASS, AND ITS ANSWER THERE IS THE WRONG ONE.**
                   Vulkan ties depth WRITES to the depth TEST, so with the test disabled
                   the attachment is not written at all and the surface comes out EMPTY —
                   which is the symptom this arm exists to rule out. Use
                   `CZ_VK_DEPTH_ALWAYS` for anything that resolves DEPTH (gotcha 279)
CZ_VK_DEPTH_ALWAYS=1  keep the depth test enabled and force the comparison to ALWAYS.
                   The arm `CZ_VK_NO_DEPTH_TEST` cannot be: it makes the same
                   "submitted or rejected" distinction and KEEPS the writes. Part 32's
                   shadow-cascade result is its worked example — the atlas goes from
                   46.875% zero to 1.86%, which is what proved the cascade's missing half
                   was rejected rather than never drawn
CZ_VK_DEPTH_CLEAR_FAR=1  clear depth to 1.0 whatever RB_DEPTH_CLEAR says. A DIAGNOSTIC
                   ARM (it ignores a register the guest writes), and the positive control
                   that named the shadow cascade's input: the atlas goes 46.875% zero ->
                   0.0113%. This title leaves RB_DEPTH_CLEAR at 00000000 for nearly every
                   pass, and a LESS test against 0 rejects every fragment
CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1  map window Y one-to-one on a 4x MSAA surface — the
                   part-33-and-earlier renderer, and the same-binary control arm for the
                   Y factor that became the DEFAULT in part 34. Xenos 4x is a 2x2 sample
                   grid, so a 4x surface is twice as wide AND twice as tall in samples,
                   and only X had the factor for 25 parts. The shadow cascade's two clear
                   rects — (0,0)-(480,512) on a 520-pitch 4x surface and
                   (960,0)-(1024,1024) — tile its 1024x1024 map EXACTLY under the
                   both-axes rule and cover 53.125% of it under X-only: atlas 46.8750%
                   zero -> 0.0038% (reproduced to four decimals on the part-34 binary).
                   The part-32 arm variable `CZ_VK_MSAA_WINDOW_SCALE_Y` is RETIRED — the
                   behaviour it enabled is the default and the variable is no longer
                   read. The scene-tile reconciliation that unblocked shipping it is
                   phase5-notes §6bf/§6bh: a clear rect is in the CLEAR declaration's
                   pixel space, and doubling Y over-clears past a shorter surface into
                   the shared stand-in exactly as the X factor always has — the same
                   approximation, measured harmless the same way
CZ_VK_SCOPED_CLEAR=1  clear only the region the pass rendered, not the whole EDRAM
                   stand-in — closer to what a Xenos copy block does, which clears the
                   tiles of the CURRENT surface. Off by default because it was measured
                   and it moves nothing (phase5-notes §6bf: 46.8750% zero in both arms,
                   to four decimals), so it is not the shadow defect
CZ_VK_RECT_TRACE=<surfacePitch>  the CORNERS of every distinct rect-list clear on one
                   EDRAM surface, named by its RB_SURFACE_INFO pitch (1040 is this
                   title's shadow cascade, 640 a scene tile). A rect-list draw at the
                   head of a pass IS the guest's clear, and the only way to know what it
                   clears is to read its three corners and the synthesised fourth. NB the
                   arena copy is already LITTLE-endian — swapping again reads every
                   corner as 0.0, which looks exactly like "the title clears nothing"
CZ_VK_PASS_DRAWS=N     how many of a pass's draws the resolve trace lists (default 4),
                   each with the draw's TEXTURE address. Four says what KIND of pass it
                   is; it cannot say what a 115-draw UI compose did, which is where every
                   "why is that rectangle black" question ends. NB this knob was
                   documented here from part 9 and did not exist until part 12 — the
                   count was a hardcoded literal and the variable was read nowhere
CZ_VK_RESOLVE_TRACE_PASSES=N  the resolve trace's budget, in PASSES rather than lines
                   (default 20, about one frame). Counting lines meant the budget bought
                   a different number of passes depending on the resolve order, so the
                   frame's LAST pass fell off the end. NB this is the SECOND knob
                   documented here before it existed (gotcha 193): part 9's note says it
                   put the budget in passes and the code still counted 60 HEADER lines
                   while the two follow-up lines printed uncapped forever — so a trace
                   ran out of headers and then emitted thousands of orphan input lines.
                   Really implemented in part 14; `grep -n CZ_VK_RESOLVE_TRACE_PASSES
                   runtime/` is the check that costs nothing
CZ_VK_DRAW_PROBE_COUNT=N  how many draws the draw probe prints (default 3)
CZ_VK_DRAW_PROBE_VERTS=N  how many VERTICES it prints per attribute (default 4 = one
                   quad = one GLYPH of a text run, which cannot show whether a run's
                   cells advance across the sheet). The probe also decodes every
                   COMPONENT of a float attribute rather than its first dword — printing
                   only `u` of a texture coordinate is what let two draws sampling
                   different atlases agree on every printed value — and carries the bound
                   texture, its dimensions and VGT_INDX_OFFSET/min/max on its header line
CZ_VK_NO_INDX_OFFSET=1  do NOT apply VGT_INDX_OFFSET, i.e. the pre-part-13 renderer, in
                   which every draw reads from vertex 0 of the fetch buffer. A draw
                   packet has no base-vertex field, so this register is the only way a
                   title sub-allocates one dynamic vertex buffer between draws — which
                   is how this title's ENTIRE UI works. With it on, the save-slot panel
                   shows one overlapped garbled text run and nothing else
CZ_DIGEST_PROBE=1  the file-digest check link by link, through the alias seam: the name
                   being verified with its buffer and length, the XEX resource the
                   container asks for, the engine's own string hash RECOMPUTED IN HOST
                   CODE (which makes it an oracle rather than a description — a
                   disagreement would put the defect in the recompiled hash), and the 20
                   bytes SHA1_Final actually wrote. A hook rather than a debugger
                   because gotcha 198: ctx.rN is stale mid-function
CZ_VK_NO_STATE_CACHE=1  re-issue every draw's pipeline, viewport, scissor, blend
                   constants and descriptor sets whether or not they changed — i.e. the
                   pre-part-18 draw path. Vulkan holds all five on the COMMAND BUFFER,
                   so a draw that repeats them is doing nothing, and the five bindless
                   descriptor sets are identical on every draw in the title. Skipped
                   fractions print with CZ_VK_STATS: pipeline 76%, viewport 98%, scissor
                   98%, blend 99.9%, descriptor-sets 99.9% — and 0.0% on every one with
                   this flag on, which is the negative control doing its job. Worth
                   11.4% of `record` (4.34 -> 3.84 ms), which is 1.6 pp of an ordinary
                   frame and ~3% of a crowd frame. It does NOT move fps in ordinary
                   gameplay and cannot: that frame is on the title's two-vblank cap
CZ_VK_NO_TEX_SWIZZLE=1  ignore the fetch constant's component swizzle, i.e. the
                   pre-fix behaviour where a single-channel font atlas samples alpha
                   as a constant 1.0 and all text renders as SOLID BLOCKS
CZ_VK_HALF_PIXEL=1 restore the -0.5 px shift the shaders' g_HalfPixelOffset used to
                   carry — i.e. the pre-part-11 renderer, in which the scene tile's
                   clear covered columns 0..638 and column 639 of the resolved scene
                   surface was BLACK. The frame's blur turns that one column into a
                   ~19 px dark band down the middle of the picture, which no aggregate
                   in this project can see and an operator sees instantly (gotcha 188).
                   The metric that CAN see it is structural: all-black columns in the
                   resolved surface, 1 with this on and 0 without
CZ_VK_NO_FLIP_Y=1  render with a positive-height viewport, i.e. the pre-fix vertically
                   MIRRORED frame. The arm for the flip that made the title screen
                   appear; note no numeric instrument in this project can tell the two
                   arms apart (gotcha 135)
CZ_VK_TEXCOORD_SWAP=1  republish the pre-part-37 16-bit texcoord unswizzle mask — the
                   control arm for the striped-material fix (phase5-notes §6bo). The
                   mask DOUBLE-corrected pairs whose microcode already carries the
                   compensating .yx destination swizzle, which transposed lightmap UVs
                   and painted the item-0s blotches; the default is now mask = 0
                   (hardware semantics). This arm repaints the tanker blotch on demand.
CZ_VK_NO_TEXCOORD_SWAP=1   accepted, now a NO-OP (it names the current default), kept
                   so recorded recipes keep meaning what they meant
CZ_VK_PRIM_RESTART=1   honour 0xFFFF as a strip separator. OFF because the guest
                   declares VGT_MAX_VTX_INDX=65535, i.e. 0xFFFF is a LEGAL index
CZ_VK_RESOLVE_TRACE=N  from frame N: each resolve's destination, extent, copy window,
                   clear bits and the DRAW COUNT of the pass it closes
CZ_VK_VALIDATION=1 the Khronos validation layer. Slow at ~900 draws a frame, and it has
                   twice named an API misuse that was being investigated as a renderer bug.
                   **It also brings VK_EXT_debug_utils in and NAMES our objects** (part 26),
                   so a message reads `VkImage 0x235...[resolve snapshot 14A7A000 96x45
                   slot 32]` rather than a bare handle — which is what turned
                   `vkCmdDraw-None-09600` from an unidentifiable image into a diagnosis in
                   one run (gotcha 255). Both the layer and the extension fall back
                   loudly rather than costing the renderer if absent.
                   **STANDING GATE: one run per session, quote the tally.** As of part 26
                   the outdoor route reports 20 x `VkGraphicsPipelineCreateInfo-Input-08733`
                   and 6 x `VkGraphicsPipelineCreateInfo-topology-08773`, and nothing else
CZ_INPUT_TRACE=1   every pad packet published to the guest, with its button mask.
                   An instrument, not an arm: it fabricates nothing, and it is the
                   witness that a real press reached XamInputGetState. Silent on a
                   keyboard-only run until a key is actually pressed; noisy with a
                   physical stick attached, because XInput's packet number moves on
                   raw jitter too and we do not filter (gotcha 102)
```

## The title's own debug scaffolding (`runtime/cpu/debug_tunables.cpp`)

Not measurement arms — these switch on code the studio shipped and disabled. See
gotcha 239 for how the gate was found and 240 for what flipping it does and does not
prove. Both are free when off (one env-var read on a function that runs once per
process, three hops off the XEX entry point) and neither has any per-frame component.

```
CZ_DEBUG_MENU=1    debug preset: the 26-reader master, debug-jump leaf, and
                   frontend-screen witness. At the title menu F2 requests the shipped
                   DebugJump screen through the frontend's own captured transition
                   manager. F4 opens the host-rendered Case Zero debug menu in the
                   frontend or gameplay. Up/Down select, Enter or Right opens/toggles,
                   and Left returns from a submenu. Categories expose player/weapons,
                   zombies/AI, vehicles, world/rendering, UI/flow and AutoChuck; the
                   engine's readable original nodes remain under their own submenu.
                   Prints each
                   flag's before/after value and its confirmed reader count, so a
                   switch that is connected to nothing is visible as such
CZ_DEBUG_TUNABLES=name[=0|1],...   any of the 21 curated tunables. An unknown name
                   prints the whole list and its per-entry notes rather than failing
                   silently, so `CZ_DEBUG_TUNABLES=?` is the way to see them
CZ_VK_STREAM_GUARD_BYTES=N  the cross-frame store's guard is EXACT up to N bytes and
                   samples 8x64 above it. Default 16384. This is the item 00c fix: at 512
                   a small edit inside a large UI vertex buffer hashed identical and the
                   store served last frame's HUD. Raise it FIRST if anything like that
                   recurs -- no rebuild needed. Every profile window reports how many
                   streams/frame exceeded the bound and were therefore only sampled, which
                   is the residual exposure
CZ_VK_STREAM_GUARD_EXACT=1  hash every byte whatever the size. The diagnostic that
                   identified 00c. Not for normal use: 75x the hashing and +11.9 points of
                   frame time against the 16 KB default's ~a quarter of that
CZ_ZOMBIE_CAPTURE=1 logs up to 512 genuine calls to the retail actor-manager submit
                   routine: caller LR, guest thread, factory, source tag/line, result,
                   and the first 96 descriptor bytes. Use while walking through a
                   populated Case Zero exterior; it is read-only and is the evidence
                   needed to implement a host spawner from the normal population path
                   instead of the incomplete Quickie scaffolding
```

**Addresses**: bound by a dataflow simulation over `sub_824A2470`, NOT by pairing each
name with the nearest store — that pairing is off by one and named every flag after its
neighbour (gotcha 241, which retracts the original table). A `lbz` reader scan cannot
catch this, because every byte in the struct is a tunable something reads.

**What is confirmed**: every flag reads 0 before the hook and 1 after, and the boot is
unaffected (the same-binary control arm — no env var — differs only in rumble calls and
lock counters, no errors either side).

**Confirmed positive**: DebugJump is visible and usable. The operator opened it with
F2 at the title menu; `/tmp/dbgrun5.log` recorded manager `A33F4CC0` and DebugJump hash
`ACC86853`. The separate in-game "Quickie Menu v0.21" renderer remains a confirmed
negative. F4 therefore uses a host renderer over the genuine retained menu state.
The Case Zero categories write the addresses resolved by this executable's own
`sub_824A2470` tunable loader. They deliberately omit Fortune City, TIR, poker/casino,
online, DLC and main-game boss controls.

The original selector type at vtable `820701C4` is also host-rendered now: Left/Right
cycles it and the selected engine name is displayed. Case Zero contains a 64-entry
`NPC To Spawn` selector but no surviving consumer of that selection, so it is not
misrepresented as a working spawner. Likewise the shipped XEX has no retained vehicle
spawn command. The story bike is mission-owned; constructing a partial `cBike` would
not be a safe substitute for the missing debug-build vehicle factory.

Zombie spawning is also deliberately absent. Although the XEX retains the Quickie
labels and apparent request-building path, operator runs `dbgrun20` through `dbgrun26`
showed that neither direct native calls nor injecting the original X-button record
produced an actor. One direct call also crashed through controller-thread TLS. Those
labels are incomplete retail scaffolding, not a feature this overlay claims to restore.

**Scope**: only Case Zero's scenes ship. The image still carries the full Dead Rising 2
scene list because the two games share an engine, but `data/models/environment` holds
only `prologue`, `prologue_menu`, `prologue_menu2`, `prologue_safehouse` and
`safehouse`. A jump anywhere else has no data behind it.

## The pose capture and the live texture filter (part 36)

```
CZ_VK_TEX_FILTER_FILE=<path>   CZ_VK_ONLY_TEX / CZ_VK_SKIP_TEX, RE-READ WHILE RUNNING
```
One line, `only=<hex[,hex...]>` or `skip=<hex[,hex...]>`; empty or missing = no
filtering. Re-read when its mtime changes, once per FRAME (not per draw), and every
reload prints `[vk] tex filter reloaded: only='...' skip='...'` — a filter that silently
failed to parse would look exactly like a texture that is not drawn, which is an arm
failing AS its own symptom (gotcha 279).

**Why the file exists when the env vars already did:** the env forms are latched once
per process, and the striped-material class picks a different streamed quality level on
every boot, so the address worth isolating is only known from a census taken INSIDE the
boot that shows the defect. By then the process has read its environment. The file lets
an operator standing in front of the blotch have textures isolated under them.

Shown capable of failing: `only=DEADBEEF` (matching nothing) takes the title screen from
mean luma **103.4 to 0.0** at the same frame count.

**And the addresses are worth filtering by, because they are STABLE across boots**:
between two operator sessions at the same spot, 703 of 712 shared addresses (98.7%) held
byte-identical content, and only 4 of 628 shared contents lived at a different address.
A census taken in one boot names textures usable in the next.

**READ THAT WITH ITS SCOPE (part 40).** Those two sessions were the same route on the
same day. Carry an address to a DIFFERENT route or a different day and it can name a
different asset entirely: part 39's six foliage-texture addresses, replayed headlessly,
returned a picture of BARBED WIRE. For anything that has to survive the trip from "the
operator saw this" to "reproduce it headlessly", key on the SHADER hash — it is a hash
of the microcode and cannot drift (`CZ_VK_TEX_DUMP_PS`, `CZ_VK_ONLY_VS`). Gotcha 306.

**AND F9 CAN BE PRESSED HEADLESSLY.** `CZ_FAKE_PRESS_SEQ` takes an `F9` entry (host key
9), so `CZ_CAPTURE_KEY` plus `...,F9,NONE,NONE` yields the whole capture — picture,
census, pose and every resolve snapshot — from a run with no window and no operator.
With `CZ_VK_DRAW_ID=1` that is the complete point-at-a-pixel loop, unattended. Put the
`F9` late in the sequence and leave entries after it: the census is armed for the frame
AFTER the press, so a sequence that ends on the press can exit before it is written.

```
CZ_CAPTURE_KEY=<dir>           F9 now also writes capture_<frame>.pose
```
Beside the picture and the census: the 16 float4 vertex ALU constants at the frame's
first draw (the view-projection and world matrices the camera fingerprint hashes) and
the head of the player game object, with the controller address.

Read it with **`tools/pose_read.py`** (one file = camera candidates; two files = also the
object offsets that changed). Everything is written RAW deliberately — deciding which
window is the view matrix, or which offset is a position, is a LAYOUT question, and a
layout decided in the runtime cannot be corrected without a rebuild while a layout
decided in a tool can.

What is established so far:
* `vc12..vc14` is a genuine view matrix on a scene frame — its three rows come out
  **orthonormal to four decimals**, so `eye = -R^T t` is an identity rather than a fit,
  and `vc15` is parameters (it reads `1.571` ≈ π/2), not a fourth row.
* **A frame's FIRST draw may be a SHADOW pass**, whose "camera" is the light frustum —
  one capture solved to a camera 36 units BELOW ground. Treat a pose whose eye is
  implausible as the light's, not as a broken solve, and prefer a scene draw.
* **The object `CZ_AUTOCHUCK` steers is not the transform holder**: 0 of its first 512
  dwords changed across a map-crossing move. It is a controller.
* `tools/live_findpos.py` hunts the position in the live process without ptrace-stopping
  it: `near <x> <y> <z> [radius]` needs no motion (scan for coordinates close to the
  pose's camera), the sampling mode needs the player to WALK. Its first run found
  contiguous arrays of `(x, y, height)` triples with height pinned at **3.373** —
  actors standing on flat ground, clustered around the camera, which independently
  confirms the eye solve lands in real world coordinates.

## Alpha test (part 38)

```
CZ_VK_NO_ALPHA_TEST=1   disable the RB_COLORCONTROL alpha test (the pre-part-38 renderer).
                   The default now drives the shaders' built-in
                   SPEC_CONSTANT_ALPHA_TEST clip from RB_COLORCONTROL bit 3 + funcs
                   GREATER/GEQUAL, with RB_ALPHA_REF as the per-draw threshold
                   (shared constants +272). Any OTHER enabled compare func is counted
                   by name ("alpha test func X UNEMULATED") and left un-emulated.
                   ~~NB: the shard-tree foliage is NOT this — it never fired the RB
                   alpha test. Part 39 read hardware's own RB_COLORCONTROL across
                   all eight R4 traces (40,703 draws) and it enables NEITHER the alpha
                   test NOR ALPHA-TO-MASK anywhere, so the bit-4 suspect §6bp named
                   is refuted and the emulation should not be built. §6bq.~~
                   **RETRACTED — that paragraph is the PART-39 measurement, taken
                   through register 0x2205 (RB_BLENDCONTROL1), and part 40 replaced
                   the index with 0x2202.** Re-run on the right register, hardware
                   enables both: over all 19 round-2/3/4 traces the leaf materials
                   carry `AA000007`, `AA00000C` and `AA00001C` — the last being
                   GREATER + enable + **ALPHA_TO_MASK** — and `RB_ALPHA_REF = 0.0` on
                   every leaf draw. The foliage IS an alpha-test draw, A2M IS set, and
                   part 46 names A2M as the shard mechanism (§6ca). A zero measured on
                   the wrong register is gotcha 3 wearing a different hat.
```

## Alpha-to-mask coverage (part 46)

```
XE_ALPHA_TO_MASK   a SHADER-CACHE arm, not an environment variable — build a second
                   cache with `CZ_DXC_DEFINES="-D XE_ALPHA_TO_MASK=1"` and select it
                   with CZ_SHADER_SPV. XenosRecomp's shader_common.h defines
                   `XeAlphaTestThreshold(pos)`, which the emitter now calls in place of
                   the scalar `g_AlphaThreshold`; with the define it returns
                   `max(threshold, (bayer2x2 + 0.5)/4)` indexed by `uint2(pos) & 1`,
                   i.e. Xenos ALPHA-TO-MASK coverage evaluated per SAMPLE.
                   Without the define it returns `g_AlphaThreshold`, so a cache built
                   from the new emitter is the NULL CONTROL and it is byte-identical:
                   canopy md5 f4a1a593a15b3e27b40d59136aadf622 for the default cache,
                   the null cache and the gated-off arm alike (§6ca addendum).
CZ_VK_A2M_ANY_SURFACE=1   DIAGNOSTIC ONLY. The runtime publishes its A2M flag
                   (shared+284) only for a 4x guest surface, because only there does
                   our window scale make one of OUR pixels one of hardware's SAMPLES.
                   This title's foliage is drawn into a **2x** surface — the counter
                   `draw: ALPHA-TO-MASK with RB_SURFACE_INFO msaa=N — dither declined`
                   reads msaa=1 on 69,390 draws, msaa=0 on 518 and 4x on NONE — so the
                   gate declines everything and the arm would be silently inert. This
                   variable drops the gate, knowingly dithering at PIXEL granularity.
                   It is how §6ca's mechanism was demonstrated (canopy p05/p95
                   0.291 -> 0.324 against hardware's 0.326, hard plates gone), and it
                   is NOT a candidate default: hard-edge share goes 3.13% -> 4.92%
                   against hardware's 0.21%, which is the stipple you get from
                   dithering with no sample grid underneath.
                   Engagement counter: `draw: ALPHA-TO-MASK on a 4x surface —
                   per-sample dither published`.
```

## Mip levels (part 39)

```
CZ_VK_NO_MIPS=1    upload level 0 alone — the pre-part-39 renderer, and the same-binary
                   control arm for the mip chain. The default now reads dword5's
                   separate MIP ADDRESS and uploads levels 1..n from it, so a minified
                   surface selects a filtered level instead of sampling full-resolution
                   texels at whatever rate the rasteriser lands on. Levels below one
                   tile (the PACKED TAIL) are declined and counted, never guessed;
                   cube-map chains likewise. Counters: `mip: chain uploaded`,
                   `mip: PACKED TAIL DECLINED`, `mip: CUBE chain not uploaded`.
                   1,815 textures take a chain on the outdoor DebugJump route. §6bq.
CZ_VK_NO_MIP_TAIL=1  part 41: the tail-only arm. The default now DECODES the packed
                   tail for square DXT levels down to 4x4 texels — offsets derived
                   from 7,466/7,515 hardware votes (tools/packed_mip_derive.py, §6bt):
                   a level of width W blocks sits at block (W,0) in the shared tile.
                   This arm reproduces the part-39/40 walk byte-for-byte (tail read
                   at (0,0), chain ends on the guards). Engagement counter:
                   `mip: packed tail level TAKEN` — 1,877 on the boot route.
                   Non-square/non-DXT/sub-block tails stay declined and counted.
```

## Per-fetch samplers (part 41)

```
CZ_VK_NO_FETCH_SAMPLERS=1  the part-40 renderer, same binary: every fetch reads
                   sampler 0 (one global trilinear REPEAT sampler). The default
                   honours the fetch constant's own mag/min/mip/aniso fields
                   (dword3 bits 19..27, confirmed two-sidedly against 621 R4
                   fetches) with one VkSampler per distinct spec in the set-3
                   heap. The shadow atlas gets the point/point/point/no-aniso
                   sampler hardware asks for; the world's albedo gets 4:1/8:1.
                   Engagement evidence: one `[vk] sampler #N` line per spec.
                   DO NOT re-try global aniso: it speckles the shadow term
                   (§6bt — hardware fetches the shadow atlas with aniso=0/point).
CZ_VK_ANISO=N      cap the per-fetch aniso degree. =0 keeps per-fetch FILTERS while
                   disabling aniso entirely — the arm that separates the two halves
                   of the change. Default cap 16 (the device limit bounds it anyway;
                   this title never asks above 8:1). Address CLAMP modes are NOT yet
                   honoured — a separate experiment (part41-kickoff item 5).
```

## The draw-ID pass (part 39) — which draw painted that pixel?

```
CZ_VK_DRAW_ID=1    with CZ_CAPTURE_KEY and F9: the next recorded frame is rendered with
                   every draw painting its OWN INDEX instead of its colour, and that
                   frame's resolve snapshots are dumped as `drawid_f<frame>_snap_*.ppm`.
                   The scene surface among them IS the map — pick it as the one whose
                   distinct values are all valid draw indices (tools/drawid_read.py
                   reports that). Read it with:
                     tools/drawid_read.py <drawid_snap.ppm> --census capture_fNNNN.census
                                          [--at X,Y | --rect X,Y,W,H] [--top N]
                   The census is written for the SAME frame, so indices line up exactly.
                   The frame's `capture_*.ppm` is NOT a picture on such a run (the post
                   chain is draws too, so it paints indices over everything) and the log
                   says so when it writes it.
                   `--palette out.png` writes the map with one distinct colour per draw.
                   USE IT rather than brightening the raw file: indices are all near-black,
                   so a brightness stretch collides neighbouring ones and invents flat
                   regions — that artifact read as "one draw covers half the screen" where
                   the numbers said 633. The palette view is legible enough to pick a tree
                   canopy out of by eye, which matters because a CZ_VK_DRAW_ID run produces
                   no photograph of the frame.
                   Counter: `draw: painted its INDEX (CZ_VK_DRAW_ID)` — if that reads 0,
                   the pass did not run and anything read off the dumps is a coincidence
                   (gotcha 304).

                   WHY IT EXISTS: every picture defect in this port had been attributed to
                   a draw by inference from shader/texture signatures, and that inference
                   was wrong twice in two sessions — the second time it selected a HAIR
                   material and a whole investigation was built on it (gotchas 291, 302).
                   Blending is forced off; mask, depth and cull are left exactly as the
                   draw had them so the map has the same visibility as the picture (305).
```

## CZ_ZONE_TEX_PROBE — the zone texture-set decision, printed input by input (part 43)

`CZ_ZONE_TEX_PROBE=1` hooks `sub_82270870` ("load zone N's common texture set",
the ONLY site that chooses `COMMON_TEXTURE.tex` vs `COMMON_TEXTURE_LOD.tex`)
and prints, on entry: the camera `[g+0x40]`, the level index and its
threshold-boost table entries, the force byte `0x82A57BD7`, the zone's
LOD-capable flag (`rec+0x90C`), and every volume in the zone's list — sphere,
skip bit, threshold, computed distance, far/near vote — plus the probe's own
prediction of the branch. The prediction is falsifiable against the
`CZ_GUEST_DIAG` narration of the same run (they matched line for line in part
43, which is what makes the model of the branch a measurement rather than a
reading). Diagnostic arm only; costs one predictable branch when off.

Companions, both `process_vm_readv` (no ptrace stop, safe while an operator
plays):
* `tools/zone_lod_live.py <pid> <world> --base <guestbase>` — re-evaluate the
  decision for every zone against the camera position of RIGHT NOW.
* `tools/zone_lod_watch.py <pid> <world> <base> [interval] [count]` — sample
  position + per-zone streaming state (`this+0x841C`) + would-be verdicts on a
  cadence; the control that proves whether a roam ever actually crossed a
  threshold (gotcha 151's counter, built in).

The world pointer is printed by the probe itself (`rec=` of slot 0); the guest
base is the `runtime: guest memory at 0x…` line.

## CZ_SET_APPLY_PROBE — the texture level machine, printed gate by gate (part 44)

`CZ_SET_APPLY_PROBE=1` hooks the whole set-apply pipeline
(`runtime/cpu/guest_probe.cpp`): `sub_82268840` (the promote walk over a
loaded COMMON_TEXTURE container: prints container + entry count, then every
DB lookup inside it — hash, index, +0x44 current level, +0x3C/+0x40 refcounts,
+0x38 asset slot), `sub_82268A10` (the type-8 catch-up walk, with its queue
item's level), `sub_82268238` (the container bind: slot, LEVEL, packed request
word, caller LR — the level argument becomes a new entry's +0x44),
`sub_8222CC80` (registration: hash, idx or −1 for create, level) and
`sub_827D1BC0` (each walk-scheduled payload read op, with the entry's name).
The 35 menu-set name hashes are compiled in and print from ANY caller, so a
run shows when each of those textures first exists in the DB.

Built for part 44's set-apply hunt; kept because it is the only instrument
that can watch the level machine at all, and because its round-2 timeline
LOOKED like a defect (all 18 zone-set lookups missing, entries created at
level 0, no payload ever scheduled through the walk) and was in fact the
machine working as designed — §6bx records that misreading so it is not
re-bought. Free when off; diagnostic-only when on (fprintf per event on the
streaming path).

## CZ_VK_MIP_TINT — which mip level does every pixel sample? (part 44)

`CZ_VK_MIP_TINT=1` replaces every uploaded chain level's blocks with a solid
colour code at upload time — L1 red, L2 green, L3 blue, L4 yellow, L5 magenta,
L6 cyan, deeper white; level 0 stays real — so the rendered picture IS the
per-pixel sampled-level map. DXT1/DXT5 only (others untinted and counted:
`texture: mip levels TINTED`). Built when part 44 needed the fact no census
could give: with mip data verified correct and every LOD bias field zero on
both platforms, WHICH level does a flat wall actually read? It answered twice
in one day: the spawn scene samples L1 at two metres and L2 at ten (the
selection-overshoot signature), and the menu GAS ball is flat AT LEVEL 0
(untinted), which split item 00i into a non-mip content/layer term. Pairs
with `CZ_VK_ANISO=0` / `CZ_VK_NO_FETCH_SAMPLERS=1` to attribute octaves to
sampler terms, and with `CZ_VK_NO_MIPS=1` as the whole-feature arm.
Diagnostic arm only — the picture is deliberately wrong everywhere a chain
exists.
