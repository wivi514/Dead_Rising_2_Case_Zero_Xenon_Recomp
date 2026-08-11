# Phase A/V — what the runtime work found

The record of what happened when `docs/phase-av-plan.md` was executed, in the shape the
other phase-notes files use: what the plan got right, what it got wrong, and what neither
the plan nor the kickoff predicted. Run 2026-08-10/11.

**Headline: the game makes sound, and the prologue cinematic's freeze was the missing
sound.** The plan opened by saying the two subsystems might turn out to be one job. They
were.

---

## 1. The plan's own §2 question, answered in the first run

§2 said: before writing a line of either track, run the prologue with
`CZ_VK_FRAME_STATS`, find the black era and read three numbers, because a STALLED
cinematic and a rendering-but-invisible one are the same picture and completely different
counters.

Run it was, and the answer was unambiguous — **stalled**:

| | plan's prediction for "stalled" | measured |
|---|---|---|
| draws per frame | flat, and low | 1,227-1,233, flat |
| `cameraFingerprint` | constant | `00d7a3a4aaed62c6` for **10,527 consecutive frames** |
| ends after `Duration` | no | no, and the run was 400 s |

Presented coverage 0.00%, mean luma 0.000, deepest file `#155 skeleton\childfullbody.big`
— i.e. parked at the start of `701_chuck_arrives_in_town`, exactly where §6ah left it on
the part-16 binary. **This is the same defect, reproduced on the part-28 binary**, which
was worth 7 minutes to establish before working on it (gotcha 13).

## 2. What was ALREADY answered before this part started, and cost a re-read to find

Two of the plan's steps were written against a state of the world that had moved. Both are
recorded here because the plan is a document a future session will read.

* **A0, "fix the pump's timer before anything else can be judged", was already fixed.**
  The plan and `docs/open-items.md` 00e both describe our pump as Fable 2's broken kind —
  `sleep_for(5333us)`, ~184 frames/s against the 187.5 that 48 kHz needs. It has in fact
  always been `sleep_until` on an accumulating deadline that never banks credit. What was
  missing was not the fix but the NUMBER: nobody had ever printed it. Printed now, on the
  `CZ_AUDIO_TRACE` line, it reads **187.4-187.6 callbacks/s**. A rate nobody measured is a
  rate nobody knows, and "we already fixed that" is exactly as perishable as any other
  claim in this repo.

* **§2's hypothesis had been formally REFUTED in part 16**, with an arm, in three
  configurations. See §5 below — the refutation was wrong, and how it was wrong is the
  most transferable thing in this part (gotcha 268).

## 3. The audio defect, and it was one address

`runtime/audio/xma_decoder.cpp` is ffmpeg's `AV_CODEC_ID_XMA2`, lifted from the Fable 2
port. Wiring it in produced *nothing* — and the instrument that had been built to tell
silence from blindness said why in one line:

```
[xma] ctx0 first packet @02538000: 00 00 00 00 00 00 00 00 00 00 00 00
[xma] validate: fed 16 pkts -> 896 float samples, rms=0.0000 peak=0.0000
```

The decoder was fine. Its input was a page of zeros. And with `CZ_NO_XMA_DECODE=1` — so
nothing of ours could be retiring a buffer under the measurement — the passive scan held
for the whole run: `in0=02538000 64 pkts (131072 bytes): 0 non-zero (0.00%), first at -1`.

Adding the destination address to the `NtReadFile` trace closed it immediately:

```
NtReadFile('game:\data\audio\music.big', 131072 bytes @ 16666624) -> 131072 into A2538000
[xma] ctx0 in0=02538000 64 pkts (131072 bytes): 0 non-zero (0.00%)
```

**0xA2538000 and 0x02538000 are the same page, 0xA0000000 apart.** The XMA decoder is a
DMA device, so the title writes PHYSICAL addresses into the hardware context; our address
space is one flat 4 GB map where the physical arena is a window at 0xA0000000
(`kernel/memory.h`), so the two are different offsets and nothing aliases them. The guest
was correct throughout. `MmGetPhysicalAddress_x` had implemented the exact inverse mapping
since phase 1.

Two free corroborations, taken because a single agreeing number is a coincidence:
16,666,624 is exactly `PressStartPrologue.xma`'s offset inside `music.big`
(`tools/big_list.py`), and 131,072 is exactly the 64 packets the context declares. The
title was streaming its title-screen music correctly into the buffer it had told the
hardware about.

Gotcha 267 is the transferable form, and **it is a Case West item on day one**.

### The result, against a measured off-state

| | `CZ_NO_XMA_DECODE=1` | decoder on |
|---|---|---|
| frames non-silent | 0 of 18,433 | **15,991 of 18,433** |
| `maxpeak` | 0.000000 | **0.108854** |
| first non-silent frame | — | 2,442 |
| per-context decode | — | 494 frames / 5 s, `refused0`, `starve1-2` |

An SDL device opens on pipewire at 48 kHz stereo and holds a steady 1,280-frame (~27 ms)
queue — neither draining nor backing up, which is the number the plan's A0 warned would
stutter if the pump were 2% slow.

### Layout, checked rather than recalled

The 64-byte `XMA_CONTEXT_DATA` bitfields were going to be taken from the Fable 2 port's
copy of Xenia's struct, which is a recollection. `CZ_XMA_PROBE` was widened to dump all
sixteen dwords instead, and the guest's own arithmetic settles it: **`dw[8] - dw[7] =
0x019D7B80 - 0x019D6280 = 6400`, and `dw[0]` bits 22..26 read 25, and `25 x 256 = 6400`.**
Two independent fields of the same structure agreeing on the ring's size is a check the
layout could have failed. Two more fields this project had already derived from the
title's own code (`dw[0]` bits 20/21 as the input-valid flags, from sub_8285EFE0) agree
too.

### Three instrument notes worth keeping

* **A vacuous check is worse than none, and the compiler will tell you.** The first
  plausibility test on a buffer pointer was `p != 0 && p < PPC_MEMORY_SIZE`, which for a
  `uint32_t` against a 4 GB map is always true — `-Wtautological-constant-out-of-range-compare`
  said so, and it is gotcha 30 arriving as a build warning.
* **A bound on work per tick can be an EVIDENCE guard, not a performance one.** With no
  packet budget, a decoder returning nothing walked a whole 64-packet input buffer in a
  single 1 ms tick and then retired it — which reads downstream as "the voice finished"
  and destroys the state being measured. The first run of the decode path did exactly
  that and reported only `0f/starve2`.
* **The per-context filter had to test `decodeCalls`**, not just frames and starves, or
  the 5-second line would have hidden precisely the context the instrument exists to
  find: one being asked to decode and refusing (gotcha 264).

## 4. The cinematics defect was the audio defect

Same recipe, **same binary**, one variable — `CZ_NO_XMA_DECODE`:

| prologue, 400 s, `START,A×10,NONE` | decoder OFF (control) | decoder ON |
|---|---|---|
| longest run on one `cameraFingerprint` | **10,513 of 12,427** | **159 of 12,429** |
| distinct camera runs | 1,014 | 7,175 |
| frames with presented coverage > 0 | 1,864 (**15.00%**) | 12,422 (**99.94%**) |
| `maxpeak` | 0.000000 | 0.173476 |
| deepest file | #155 | #155 |

The camera moves, the scene is presented, and the black era is gone. **The prologue
cinematic was waiting on audio**, which is what `docs/phase-av-plan.md` §2 proposed in its
first paragraph and what part 16 believed it had refuted.

> **CORRECTION, from the operator session an hour later: "the scene is presented" is
> right and "the cinematic plays" was TOO STRONG.** It now runs forward ~1 s, backward
> ~1 s, and repeats — see `open-items.md` 00j, which also gives the mechanism (the
> title's own PID controller on audio latency).
>
> **The evidence was in the table above the whole time and I read past it.** 7,175 camera
> runs over only **1,170 distinct** fingerprints is a 6.1x recurrence; a scene that plays
> normally has runs ~= distinct. I checked the statistic that answered the question I was
> asking — is the camera frozen — and not the one sitting in the same column that says
> whether it is *advancing*. "Not frozen" and "playing" are different claims and only one
> of them was measured. The audio finding is unaffected: the freeze really was the
> silence, and `CZ_NO_XMA_DECODE=1` still separates the two arms cleanly.

Three runs, not two, and the third is what makes the pair admissible. `CZ_FAKE_PRESS_SEQ`
is a fixed-interval arm against a boot whose depth in wall time is a distribution
(gotcha 75), so a single control could have differed by drift rather than by the variable.
The control was therefore run twice — once as the pre-commit binary and once as the
current binary with the decoder switched off — and the two agree to within 0.1%: **10,527
vs 10,513 frozen frames, 15.0% vs 15.00% coverage**. The split between control and arm is
two orders of magnitude wider than the run-to-run spread, and both arms reach the same
depth (#155), so they are the same drive with one thing changed.

## 5. Why part 16's refutation was wrong, which is the lesson

Part 16 did not guess. It built `CZ_XMA_NULL_DECODER` — a decoder that consumes input and
produces nothing — precisely so the "cinematics wait on audio" hypothesis could be tested,
and it ran **three** configurations of one binary: voices always playing, voices never
playing, and voices that start and finish (19 start / 18 stop edges). All three froze
frame-for-frame identically, and §6ah recorded it as *"refuted, not merely unconfirmed"*.

That is a better-constructed negative result than most, and it still retired a true
hypothesis. The arm moved **the predicate the title polls** — the input-buffer-valid bits
that sub_8285EFE0 reads. It could not move anything downstream of PCM actually existing.
An arm refutes a hypothesis only over the states it can reach, and a null implementation
reaches the states its author believed were load-bearing.

The tell was in the arm's own header comment at the time: it "fabricates playback progress
the real hardware would only make after actually decoding the audio". That sentence names
the gap. Gotcha 268.

## 6. Corrections this part owes the ledger

* `docs/open-items.md` **00e is CLOSED** — and its step 3 ("output last") was right, and
  its warning against opening a device first was right, and following that order is what
  made the two-line diagnosis possible.
* `docs/open-items.md` **1d / 2, the prologue**, are closed by §4. The list of things
  "retired with arms and not to be re-bought" in 1d begins **"not audio"** — that entry is
  now the one that was wrong, and it is corrected in place there.
* `docs/phase5-notes.md` **§6ah (i)** — corrected in place.
* The plan's **A0** premise is retracted (§2 above).
* `docs/xenia-capture-analysis.md` finding 7's "movie player" framing was already
  corrected at the end of part 27, before this part ran.

## 7. What is still open in this area

* **A3, the cinematic audio event.** `AudioEventName = "sync:39791"` is still unresolved
  as a lookup — nothing here established whether it resolves, only that cinematics now
  advance. Worth one `tools/gdis.py` pass if a specific cinematic misbehaves.
* **Whether every cinematic now completes.** This part measured the PROLOGUE. Item 1's
  retraction already established that Katey Zombrex, the bike-frame delivery and the combo
  weapon completed on the part-19 binary, so the population of failing cinematics may now
  be empty — but that is an inference, and the operator is the instrument for it.
* **Mixing quality.** The downmix is a fixed 5.1 -> stereo matrix and the per-voice
  volumes are the title's own; nothing has compared what we play against hardware. The
  first thing to check by ear is speech intelligibility during a cinematic, because that
  is where a wrong `is_stereo` or `sample_rate` bit would be loudest.
* **`starve1-2` per 5 s per context** is small but not zero. It means the guest had ring
  space and we produced nothing that tick. Attributable rather than mysterious, which is
  why the counter exists.

---

# Part 29 — the cinematic loop, and it is a control loop

Run 2026-08-11. The part-28 hand-off left one instruction for this defect and it was the
right one: *a palindrome means some clock DECREMENTS, so find what writes the cinematic's
time each frame.* This part did exactly that and the answer turned out to be a PID
controller the title ships switched on.

Full item, with the arm table and the corrections: `docs/open-items.md` 00j.

## 1. The lead was invisible to a tool, and the tool said "0"

The first question — who touches the cinematic manager's singleton at `0x82A46294` —
was put to `tools/gdis.py --find-uses` and came back **`0 site(s)`**. For a global that
is read on the frame path.

The scanner reconstructed `lis`+`addi`/`ori` pairs only, i.e. it could see code that
takes an address and not code that *dereferences* one, and dereferencing is what the
compiler emits for a global it is about to load:

    lis  r9,  0x82A4
    lwz  r31, 0x6294(r9)        <- the reference, folded into the memory operand

It is 301 sites. Every D-form load/store is matched now and the mnemonic prints with each
hit, because for a data address the interesting question is almost always *who writes
it*, and two `stw` sites out of 301 is a five-second answer once they are labelled.

This is gotcha 25 in its purest form and it is worth stating as a rule for the next port:
**a scanner's zero is a statement about the scanner until you have shown it can match the
shape you are asking about.** The control cost one command — re-run it on an address
whose `lis`+`addi` site is already on screen.

## 2. What writes the cinematic's time

`sub_82475718`, and `sub_82478FC8` stores its return straight into `[cine+0x1698]`. It
switches on a mode word (global `0x829DC320`, shipped as **2**):

    0  raw scene time · 1  the audio stream position · 2  PID(audio position)

`sub_824741D8` is the PID. It does not have to be *guessed* to be a PID: its own tail
plots four values through the engine's debug-graph API under the strings `Cine.Audio
P-gain / I-gain / D-gain / MV (ms)`, which is the module naming its own control law. It
accumulates `P*err + I*integral + D*(err - prevErr)` and returns **`setpoint minus that
accumulator`** — a value with no monotonicity anywhere in it.

That last line is the whole defect class. A scene time that is a setpoint minus a
controller's output runs backwards whenever the controller overshoots.

## 3. The measurement, and what it found was not what was expected

`CZ_CINE_TIME=<file>` logs the clock at its source. The expectation going in was
"controller oscillates". The file says something better:

| | |
|---|---|
| `mode` | 2 on all 2,212 lines; the PID ran on 2,208 |
| `setpoint` | climbs linearly, forever — 0.06 -> 122.7 s |
| `audioPos` | **freezes at 4.906667 s after 4 s and never moves again** |
| `ret` | hunts 4.91 <-> 5.27 s, period ~11 s |

The controller is not misbehaving. It is tracking an input that has stopped, integrating
against an error it cannot close, and dragging the scene back and forth across it. **The
PID is the mechanism of the symptom and not the defect**, and the distinction is the
difference between tuning a gain and fixing an audio pipeline.

The columns were chosen so this could have come out the other way, which is the only
reason the reading is worth anything: `mode` never reading 2 would have killed the PID
explanation outright, and `setpoint` oscillating would have moved the defect to the
caller. Both were live possibilities when the probe was written.

## 4. The camera palindrome IS this clock — joined, not asserted

Interpolate `ret` onto every frame of the same run and ask how tightly one
`cameraFingerprint` pins it: median within-camera spread **0.0052 s**. The same statistic
at deliberately wrong alignments reads **0.042 to 0.377 s**, 8x to 72x worse. The null is
built from the same data, so no second run and no assumption about the offset is doing
any work.

## 5. The arm, and why all three settings are the title's own

`CZ_CINE_AUDIO_MODE=0|1|2` writes the mode into the config block the guest just built.
Every setting is a code path the title implements — the arm invents nothing, which is
what makes a negative result from it mean something.

| arm | cinematic era | what it establishes |
|---|---|---|
| 2 — shipped | **LOOPING**, 15 poses, runs/distinct **120** | the defect |
| 1 — scene time := audio position | **FROZEN** at 4.906667 for 338 s | the input really is stuck |
| 0 — no audio sync | no loop; the run reaches gameplay | the correction is what loops |

Mode 1 was **predicted in the commit before it was run**: hand the scene the frozen
position directly and it must freeze rather than oscillate. It did, for 338 seconds.

**Mode 0 is confounded and is not a fix.** With no audio sync the first call site hands
over an uninitialised scene time of ~138,181 s, so the cinematic ends immediately; mode
2's `if (input == 0) return 0` guard is what normally protects against that. Recorded
because "mode 0 makes it play" is exactly the wrong lesson to take from that row.

## 6. Where the defect actually is now

    sub_82759170   the cinematic asks the audio system — message ReqID 0x106, field "Time"
    sub_827213C8   the audio system looks the voice up in [sys+0xA8] and returns entry+8
    sub_82721530   which it refreshes every tick from sub_8270F768(voice)
    sub_8270F768   voice state 2/3 -> sub_82764C48; otherwise a wall clock
    sub_82764C48   **SamplesPlayed / sampleRate**, from a struct shaped exactly like
                   XAUDIO2_VOICE_STATE (SamplesPlayed at +8)

`4.906667 x 48000 = 235,520` samples exactly `= 1,840 XMA subframes of 128`. The voice
plays that many and stops while still reporting itself playing, so the wall-clock
fallback never takes over either. **That is the part-30 question.**

### RETRACTED: the clip did NOT end — we stream 1.6% of it

**This section originally concluded that the dialogue clip ran to its end and only the
end-of-stream handshake was broken. That was wrong, the operator refuted it in one
sentence ("the clip is around 5 min 10 s"), and the asset then confirmed the refutation
to within six seconds.** The original reasoning and the measurement that misled are kept
below the correction, because the shape of the error is the transferable part.

**What the asset says.** `CZ_FILE_TRACE=1` names the file: the cinematic's audio is
`game:\data\audio\cinematics.big` entry **`39694.xma`, 24,377,344 bytes**, read from
its exact start (offset 3,307,520). Summing the `frame_count` field of all 11,903 XMA2
packet headers gives 89,007 frames of 512 samples:

    1 stream  (2 ch)  ->  949.4 s = 15.82 min
    2 streams (4 ch)  ->  474.7 s =  7.91 min
    3 streams (6 ch)  ->  316.5 s =  5.27 min   <- 5 min 16 s

The operator's stopwatch says ~5 min 10 s, so **the clip is 5:16 and it is 5.1 audio
carried as three interleaved 2-channel XMA2 streams**. That independently explains the
loose end this part had flagged as "worth a look, not worth a conclusion": contexts 5, 6
and 7 all report `in0=02584000`, the *same* input buffer, because three stereo XMA
contexts is how the 360 decodes six channels from one packet stream. Two unrelated routes
— the shared buffer and the packet arithmetic matching a stopwatch — agree.

**So the real defect is upstream of the handshake: the guest stops STREAMING.** The file
trace is unambiguous about the asymmetry:

    music.big        47 reads, 128 KB each, alternating A2538000 / A255E000, forever
    cinematics.big    ONE 128 KB read into A2584000, one into A25AA000, then nothing

262,144 bytes of 24,377,344 — **1.1% of the stream ever reaches memory**, and we decode
the first buffer's 4.916 s of a 316 s clip. Music double-buffers correctly for the whole
run on the same machinery, which is what makes this a specific defect and not "streaming
is broken".

**What was actually measured, and why it was over-read.** The dialogue contexts decode
5.03/5.04/4.92 s and stop; `SamplesPlayed` pins at 4.906667 s, 448 sample-frames behind
what we decoded; our decoder stays healthy (`refused0`, ctx0 still ~508k samples per 5 s
window) and the guest's mixer plateaus at driver frame ~12,288. Every one of those
numbers is still true. The error was in the step from "the voice played exactly what we
decoded" to "so the clip ended": **agreement between our own output and the guest's
position says only that the two sides agree with each other, and neither of them is the
asset.** The asset was one `CZ_FILE_TRACE=1` away and had not been asked.

That is the general form, and it is worth a gotcha: **when two components you built agree,
you have measured your own consistency, not the ground truth.** The discriminator was
correctly identified and written down as the next step — the mistake was recording the
likelier branch as the finding rather than leaving it open until the asset answered.

---

**The original section, kept for the record:**

### And a diagnostic run narrows it further: the clip ENDED

`CZ_AUDIO_TRACE=1 CZ_XMA_DECODE_LOG=1` alongside the clock probe, on the prologue:

* the three dialogue contexts (5, 6, 7 — `stereo=1`, 48 kHz) decode across exactly two
  5-second windows and then **never appear in the active list again**. Totals, as
  stereo-interleaved seconds: **5.03, 5.04, 4.92**. The frozen `Time` is **4.906667**;
  ctx7 agrees to 0.014 s.
* our decoder is healthy throughout and stays so: `refused0`, `starve1-2`, ctx0 (the
  music) still producing ~508,000 samples per 5 s window for the rest of the run.
* the guest's mixer output goes to `peak=0.0000` and `non-silent` plateaus permanently
  at driver frame ~12,288 (65.5 s), i.e. the same era.

~~So this is **not** a voice starved part-way through its stream. The clip ran to its end,
our side delivered all of it, and then nothing told the title the stream was over~~
**RETRACTED — see above. It IS starved part-way: 4.916 s of a 316 s clip.** What follows
still describes the state the voice is left in — the
voice stays in the state-2/3 "playing" branch of `sub_8270F768` forever with
`SamplesPlayed` pinned at the clip's last sample, so the wall-clock fallback that would
have let the cinematic carry on never fires.

**It could only appear now.** Before phase A/V nothing decoded, so no voice ever reached
the end of a stream and this handshake was never exercised.

**The one thing NOT settled, and the discriminator for part 30.** Our decoder's output
length agreeing with the frozen position is consistent with two different stories: the
clip really is ~4.91 s and only the end-of-stream handshake is broken, or our decode
stops early at ~4.91 s and the clip is longer. They are told apart by the ASSET, not by
us — put `CZ_FILE_TRACE=1` on this run to name the file the dialogue is read from, then
`tools/big_list.py` for the entry's true length. Do that before touching
`kernel/audio.cpp`, because the two stories have opposite fixes.

**One observation worth a look and not worth a conclusion:** contexts 2, 5, 6 and 7 all
report `in0=02584000` — the same input buffer address. That may be the title reusing one
streaming buffer for sequential lines, or it may be four voices genuinely pointed at one
buffer. `CZ_XMA_PROBE`'s per-context dump over time answers it and nothing here does.

## 7. Two corrections this part owes

* **The recorded `runs/distinct = 6.13` for this defect is diluted 6x**, and every
  previous reading of it has the dilution. A prologue run spends ~1,870 frames in menus
  first, contributing 1,010 of the 1,170 distinct poses and almost none of the runs.
  Whole run 6.14 · menus 1.01 · **cinematic era 38.27** · steady state 15 poses at 120.
  `tools/frame_loopiness.py` prints quarters unconditionally now. It caught the author of
  this part reading a mid-run file and calling mode 0 healthy, which is the positive
  control for the change.
* **"Audio" was one word covering two facts with opposite orderings.** The position the
  guest *reports* stops first and is upstream of the stall; audible output stops ~5.5 s
  later and is downstream, as the already-queued dialogue plays out. Part 28's "do not
  chase the silence — it is downstream" is right about the second and wrong if applied to
  the first, which is where the defect lives.


---

## Part 29, second half — FIXED, and the fingerprint had been on screen twice

The operator refuted "the clip ended" in one sentence, and reading the asset that
sentence forced open answered everything at once.

**`39694.xma`, the prologue's audio.** 24,377,344 bytes; its 11,903 packet headers sum to
89,007 frames of 512 samples = **316.5 s as three interleaved streams**, against the
operator's ~5 min 10 s. Three streams because the 360 decodes 5.1 as several 2-channel
streams sharing one packet stream, one XMA context per pair — **which is why contexts 5, 6
and 7 all pointed at input buffer `02584000`.** This project noticed that twice and wrote
it down twice as "worth a look, not worth a conclusion". It was the bug's fingerprint.

**The defect.** The packet header's `packet_skip` says how far to step to reach the next
packet of the same stream. Our walk advanced by one. Correct for mono and stereo — music,
SFX and one-shot voice lines are all single-stream, `skip` is 0 throughout, and the code
path is byte-identical before and after. Wrong for 5.1, where each context then decodes
every other stream's packets as its own. One 128 KB buffer is 64 packets carrying 519
frames = **1.845 s of programme**; the skip chain from packet 0 reaches **20 of those 64**;
we were producing **4.916 s** per context. 2.66x too much audio, so each ring filled about
three times faster than the mixer drained it, the whole voice group wedged after one
buffer, `SamplesPlayed` stopped, and the PID clock tracked a frozen input.

**The result.** The gate for 00j, on the prologue:

| | cinematic era | audio clock |
|---|---|---|
| before | runs/distinct **120**, 15 poses, LOOPING | frozen at 4.906667 s |
| after | runs/distinct **1.00** every quarter, 9,688 poses, advancing | **310.7 s** of 316.5 |

`audio/cinematics.big` read **201** times against 2; the run leaves the prologue into
gameplay; ten contexts decode concurrently with `refused=0`. **Operator on the live build:
the first cinematic played to completion with sound, and so did the second.**

**A second, separate defect found on the way and fixed on its own evidence.** The walk
retired a spent input buffer by unconditionally switching to the other one. This title
never uses buffer 1 — 136 context dumps over a whole run, `in1Ptr` 0 in every one — so the
switch parked the context on a buffer that does not exist, unrecoverably, because the walk
reads `currentBuffer` to decide where to look. `ctx7` was caught in it (`valid=00 cur=1`,
28 consecutive dumps). Necessary, correct, and it moved the gate by nothing on its own —
recorded that way rather than folded into the win.

**Two lessons, both now in the ledger.** Gotcha 270: two components you built agreeing is
a consistency check, never an oracle. Gotcha 271: a format field that is zero in every
asset you have played is untested, not absent — and an unexplained structural oddity in
your subject is the bug, waiting.
