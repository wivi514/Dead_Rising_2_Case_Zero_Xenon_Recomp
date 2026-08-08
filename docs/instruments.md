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
CZ_AUDIO_TRACE=1   XMA context allocation + every 512th driver frame WITH its peak
                   amplitude, so "the pump runs" and "the game makes sound" stay
                   separable
CZ_AUDIO_FRAME_US=N  the driver frame period (default 5333 = 256 samples @ 48 kHz)
CZ_NO_AUDIO_PUMP=1 register the client but never invoke its callback — the control
                   arm for every claim about driving the audio callback
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
                   started is still playing for the life of the process
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
                   gated, each on a debug byte a shipped build leaves at zero.
                   Raising those flags is the open work (gotcha 215). `game:\cl.txt`
                   is NOT the switch: sub_82482E50 reads it as a CHANGELIST NUMBER
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
                   sleep ~22%, gpu ~42%, draw ~22%
CZ_VK_PROFILE + nvidia-smi  **read the GPU's CLOCK before believing any `submit`
                   figure.** Every GPU number this port has ever recorded was taken with
                   the card at **P8, 210 MHz of a 2100 MHz maximum, 15.7 W of a 240 W
                   limit**, the driver's own clocks-event reason being "Idle: Active"
                   while the game renders on it. Under part 18's heavier load it lifts
                   to P5/465-480 MHz — still 23%. `nvidia-settings GPUPowerMizerMode=1`
                   and running with a real SDL window both changed nothing; it wants
                   `sudo nvidia-smi -pm 1` / `-lgc`, or a re-measure with the display
                   awake. Until that is answered, `submit` is not evidence about the
                   renderer's workload and the overnight plan's §2a (pipeline the
                   submit) and §2c (EDRAM size / MSAA fill) are both aimed at a number
                   that may be 8x smaller than it reads (gotcha 219)
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
                   column is hit ~500,000 times a run (gotcha 7)
CZ_VK_TEX_REFRESH=<hex[,hex]>  re-read those textures' pixels on EVERY fetch, into the
                   SAME image and slot (the dimensions are part of the cache key, so
                   updating in place is exact and needs no allocation). The arm for "we
                   cached a texture the guest is still writing" — and on the garbled
                   glyph atlas it engages 2,250 times and changes nothing
CZ_VK_TEX_DUMP=dir + CZ_VK_TEX_DUMP_ADDR=<hex[,hex]>  the UNTILED bytes of a texture as
                   a greyscale PGM. It separates "our untiling scrambled this" from "the
                   texture is fine and the draw samples it wrong", which are different
                   subsystems — and a human can tell a page of glyphs from a page of
                   noise instantly, which no aggregate over it can
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
                   from another pass", which look identical in a snapshot
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
