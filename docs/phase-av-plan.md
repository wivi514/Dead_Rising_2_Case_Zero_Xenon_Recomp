# Phase A/V — sound, and cinematics that render

**The plan for one part covering two subsystems, written 2026-08-10 (end of part 27).**
Operator request: make audio work, and stop cinematics being a black screen.

They are one part rather than two because **the assets say they are coupled**. A Case Zero
cinematic script contains a `cCineAudioEvent` and its camera track is named `sync:` — see
§2 — so a cinematic that waits on an audio event we never complete is a live explanation
for the black screen, and it is the cheapest thing to test in either subsystem. Doing
audio first and cinematics second may turn out to be doing one job.

---

## 1. What is ESTABLISHED, with its source

Measured, not assumed. Everything else in this document is labelled as a guess.

**Audio**
* `runtime/kernel/audio.cpp` (662 lines) implements the XAudio render-driver client, the
  XMA context array and its MMIO register file, and it **works**: a headless run reports
  `render driver pump started (5333 us/frame, 256 samples x 6 channels)` and ~27,000
  frames submitted in 150 s. Its own header says what is absent — "There is no audio
  OUTPUT here and no XMA decoding. Submitted frames are counted and dropped." That is an
  honest null sink, not a fake success.
* **The guest hands us real buffers full of ZEROS.** Part 26 rewrote the trace so silence
  and blindness are different numbers: `null=0`, `non-silent=0`, `maxpeak=0.000000` over
  every frame of a run to gameplay, with the scanner self-testing at 0.5000 on a synthetic
  frame. **So the silence is upstream of the mixer, and an output device is NOT the first
  step.** (`docs/open-items.md` 00e.)
* Fable 2 has the pieces: `audio/xma_hw.cpp` (430 lines, the hardware register contract),
  `audio/xma_decoder.cpp` (192, ffmpeg), `audio/audio_out.cpp` (175), and
  `docs/audio-xma.md`, titled *"why nothing the game mixed was audible"*.

**Cinematics — and the framing in the ledger needs correcting**
* `docs/xenia-capture-analysis.md` finding 7 says movies "stream through an in-house
  *Movie Player Object* that loads `.big` cinematic archives", and CLAUDE.md repeats it.
  **The no-Bink half is right and important. The "movie player" half is misleading**, and
  the archives now readable with `tools/big_list.py` say why:
  * `data/cinematics/cinematics.big` is **29 `.txt` files** — cinematic SCRIPTS.
  * `data/anim/cinematic/700_prologue_intro.big` contains **one entry, a nested
    `fullbody.big` of 4.6 MB** — animation data.
* So **a cinematic is an in-engine scripted scene, not a video**: camera animation, actor
  animation, particles, a HUD event and an audio event, played through the ordinary
  renderer. A black screen is therefore a rendering or scene-setup failure, and there is
  no codec to write.

**A cinematic script, in full structure** (`601_survivor_deaths.txt`):

```
cCinematic cinematic
    Duration "12.7"   Exclusive "True"   HideZombies "True"   Skippable "True"
    LoadingScreenName "generic_zombification"   FlushAudio "False"
    cCineAnim cameras         AnimationName "601_survivor_deaths~cameras"
    cCineZombieAnim zombie01
    cCineParticleEvent  x7    (blood_a, blood_drip, zombieFaceDecal, ...)
    cCineHUDEvent hide_hud
    cCineAudioEvent audio_stream    AudioEventName "sync:39791"
```

---

## 2. The one hypothesis that spans both, and it is the first thing to test

**A cinematic may be STALLED waiting on audio.** The audio event is named `sync:39791`,
the script carries a `Duration` and a `FlushAudio` flag, and our audio path completes
nothing. If the cinematic's clock is driven by, or gated on, the audio stream, then a
scene that never advances is exactly a black screen — and it would be fixed by the audio
work rather than by anything in the renderer.

**It is refutable in one run and needs no new code**: a stalled cinematic and a rendering
one look identical on screen and completely different in the counters.

| | stalled | rendering but invisible |
|---|---|---|
| draws per frame during the cinematic | flat, and low | normal or high |
| `cameraFingerprint` in `CZ_VK_FRAME_STATS` | constant | changing |
| the scene ends after `Duration` seconds | no | yes |

Run the prologue with `CZ_VK_FRAME_STATS`, find the black era, and read those three. **Do
this before writing a line of either track.** If it is stalled, the cinematics half is
mostly the audio half and the plan collapses into one job; if it is rendering, the two are
independent and can be worked in parallel.

---

## 3. Audio track

**A0. Fix the pump's timer before anything else can be judged.** Ours is
`sleep_for(5333us)` = ~187.5 Hz nominal, which Fable 2 measured as **~184 frames/s in
practice** against the 187.5 that 48 kHz needs — a ~2% deficit that starves the device
into a stutter easily mistaken for a decode bug. Fable 2's `docs/audio-xma.md` records
this trap verbatim and it applies to us unchanged. Fix it with a deadline-based pump (
accumulate the next wake time, do not sleep a fixed interval), and **prove it with a
counter**: frames submitted per wall second, which must read 187.5 ± 0.2.

**A1. XMA decode.** Lift Fable 2's `xma_hw.cpp` + `xma_decoder.cpp`. Its `xma_hw.cpp` is
the register contract and ours already has the context array and MMIO file, so the seam is
narrow. **The gate is not "it decodes" — it is that the mixer's peak stops being zero.**
The existing `CZ_AUDIO_TRACE` already reports `non-silent` and `maxpeak` and already
self-tests at 0.5000, so the instrument for A1 exists and has been shown able to report a
positive.

**A2. Output, LAST.** `audio_out.cpp`, 175 lines, same frame format we already produce: 6
planes of 256 planar big-endian float32. Doing this before A1 produces silence and invites
"our output path is broken" — a session lost to the wrong subsystem, which is exactly why
00e was written.

**A3. The cinematic audio event.** `AudioEventName = "sync:39791"` is a numeric ID into
some bank, not a filename. Find what resolves it — `tools/gdis.py` on the string's
xrefs — and check whether the resolution succeeds in our runtime even with no decoder. A
lookup that FAILS is a different defect from a lookup that succeeds and plays silence,
and only one of them is fixed by A1.

---

## 4. Cinematics track

**C0. Establish what the black era actually is** — §2. Everything below assumes it turns
out to be rendering rather than stalling.

**C1. Is the scene being drawn at all?** `CZ_CAPTURE_KEY` gives the picture, the per-draw
census and every resolve snapshot of one frame from a single F9 press. Press it during a
cinematic. The census answers "how many draws, with which shaders" and the snapshots
answer "which pass first goes black" — the same first-divergent-operation method that
located the white plateau in part 27.

**C2. Is the CAMERA the problem?** A cinematic drives the camera from an animation track
(`AnimationName = "..."~cameras`). If that track does not load or does not apply, the
camera keeps its gameplay transform or collapses to the origin, and a scene rendered from
inside the ground is black with a full draw list. `CZ_VK_DRAW_PROBE` prints the viewport,
the scissor and the vertex-shader constants, so the view-projection matrix is readable
directly — and `vc(0..3)` on a normal gameplay draw is the known-good comparison.

**C3. Is it the LOADING SCREEN?** Every script names one (`LoadingScreenName =
"generic_zombification"`). A loading screen that starts and never ends is black, holds the
HUD hidden, and looks exactly like a broken cinematic. `CZ_SCREEN_TRACE=1` already names
frontend screens by hash — the instrument exists.

**C4. The animation archives are nested `.big` inside `.big`.** `700_prologue_intro.big`
holds a single 4.6 MB `fullbody.big`. Our VFS serves whole files and the guest parses
archives itself, so this should be transparent — but it is worth one `CZ_FILE_TRACE` check
that the nested read happens at all, because "the animation never loaded" and "the
animation loaded and did not apply" are different bugs with the same picture.

---

## 5. What would make this a wasted part

* **Opening an output device first.** The measurement in 00e exists precisely to stop
  this. Silence in, silence out, and a week spent on SDL.
* **Assuming the cinematic is a video.** There is no codec. If a session starts looking
  for one it will find `.big` archives full of animation and conclude the format is
  undocumented, which is true and irrelevant.
* **Judging audio by ear before A0.** A 2% pump deficit stutters, and a stutter sounds
  like a broken decoder.
* **Reading the black screen instead of the counters.** A stalled scene and an invisible
  one are the same picture. §2 separates them in one run and costs nothing.
* **Not running the null.** Every arm here needs its off-state measured in the same
  block — `CZ_AUDIO_TRACE` on a run with the decoder disabled is the control for A1, and
  a cinematic era in a build without the fix is the control for C1-C3.

## 6. What this part should leave behind

A `maxpeak` that is not zero, with the run that shows it; a cinematic era whose draw count
and camera fingerprint both move; and — whichever way §2 resolves — **finding 7's "movie
player" framing corrected in the ledger**, because the next port of this engine (Case
West, same studio, same cinematic scripts) will read that sentence and go looking for a
decoder that does not exist.
