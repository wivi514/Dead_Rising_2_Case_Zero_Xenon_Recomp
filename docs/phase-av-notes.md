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
