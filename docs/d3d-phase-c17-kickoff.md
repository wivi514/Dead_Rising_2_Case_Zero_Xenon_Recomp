# D3D phase C, part 17 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §6ah** first — it is the
record of the session this hands off from, and it is mostly negative results, which is
the point. `docs/d3d-phase-c16-kickoff.md` is the previous hand-off: **its item 1 is
narrowed, not closed**; items 2–10 are unchanged and repeated below.

## What part 16 changed, in one paragraph

Part 16 took part 15's item 1 — "the prologue is stuck, the leading hypothesis is
audio" — and spent the session removing wrong answers. **Audio is refuted** across all
three configurations of a new arm. **Nothing is deadlocked**: a full `gdb` thread
survey found exactly one thread in guest code, doing its ordinary per-frame GPU sync,
and the main guest thread in a wait that is being signalled. **Our synthetic input is
not holding it**: with input stopped for the last 170 s the state is identical. And
**part 15's own conclusion is confirmed** by an unbiased histogram — the guest really
does write the tone map's full-black fade, 5,662 times, one distinct value per
register. What is left is a game whose main loop is turning, whose renderer is
faithful, whose kernel surface is complete for this era, and whose world simulation
does not advance. It is sitting at the start of its first cinematic.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, no input, empty `CZ_SAVE_DIR`:

* `--smoke` OK. A1: **exact 84-prefix**. A5: **exit 0, 2 windows, 0 real**.
* `truncated=0`, 0 parser stalls, `ring: waits ... max=2`, chain
  `arms=3557 ints=3554 isr=3554`, `kicks == walks == 1964`, `distinct=373`.
* Both capture oracles clean (`pm4_packet_lengths.py`, `pm4_indirect_walks.py`).
* `grep -c "no translated shader"` = 0. Deepest file on a no-input boot **#83**.

## The prologue, as precisely as it is currently known

One 300 s `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A` run, by era:

| frames | draws | presented | camera | what it is |
|---|---|---|---|---|
| 1..591 | 2,514 | ~96% | new every frame | title screen |
| 596..962 | ~150 | ~36% | 4–5 cycling | loading screen |
| 974 | 948 | 0.00% | — | the world's first frame |
| 1002..end | 1,225–1,247 | **0.00%** | **constant** | frozen |

* The world IS being submitted (≈849,000 vertices a frame) and IS static: the scene
  surface's mean luminance is pinned at **104.484 to three decimals**.
* The last files opened are the first cinematic: `#146 cinematics\cinematics.big`,
  `#147 anim\cinematic\701_chuck_arrives_in_town.big`, `#148 skeleton\cineplayer.big`,
  `#149 skeleton\cinechild.big`.
* **The operator reports** that after a new game the real game plays **two cinematics
  with loadings between them, and then PAUSES to show a tutorial.** So there is a known
  good sequence to compare against, and "the world is frozen" is a state the real game
  legitimately enters later — do not treat every frozen world as the bug.

## Where part 17 starts, in order

1. **THE PROLOGUE, with the search space cut down.** Everything below is retired with
   an arm, so do not re-buy it: it is **not audio** (`CZ_XMA_NULL_DECODER`, three
   configurations), **not a deadlock** (one thread in guest code, main loop turning),
   **not our synthetic input** (`CZ_FAKE_PRESS_SEQ=...,NONE`), **not a missing kernel
   import** (`[kcall]`'s first-occurrence list ends at `XeCryptShaFinal`; the era
   reaches nothing new), and **not the renderer** (the fade constants are the guest's
   own, confirmed by histogram). Three lines worth taking, cheapest first:

   * **Raise the engine's debug-log gates.** `CZ_GUEST_LOG=1` already hooks the sink
     (§6ah(v)); the 640 call sites are gated on debug bytes a shipped build leaves at
     zero. Find how those bytes are clustered — the tutorial gate is the global at
     `0x829EC974`, the cinematic ones are object-relative (`lbz r11,0x7cd9(r31)`) —
     and raise them from the runtime at startup. If it works the title tells you what
     state it is in, in its own words, and Case West inherits it. This is the highest
     leverage per hour on the board.
   * **Instrument the cinematic system directly.** `cCinematic`, `cCinematicsItem`,
     `cCineMovieEvent`, `cCineBackendMovieEvent`, `cMissionCinematic` are all named in
     the image, as are the source paths
     (`...\assets\assetfactoryobjects\cinematic\*.cpp`). Find the cinematic's per-frame
     update and print its state/time; a cinematic whose clock does not advance and one
     that was never started look identical from outside.
   * **Compare against the era that WORKS.** The loading screen animates and the title
     screen animates, so the guest's clock and update path are fine in both. Whatever
     changes at frame ~974 is the whole question, and a diff of what the guest calls
     per frame either side of it is a real instrument nobody has built.

2. **XAM ordinal `0x271` is resolved on the save-LOAD path and we answer NOT_FOUND**
   (`docs/phase3-notes.md` finding 51 — unchanged). With A3's real save installed our
   content layer enumerates it correctly and the title reaches the save-slot panel,
   then labels SLOT 1 `Damaged Content` and puts up `Load failed!`, having never opened
   the file. Do NOT mint a stub blind (gotchas 59/201) — name it from the guest's call
   site first; `tools/gdis.py --find-uses 0x271` finds nothing, so it is not built by a
   plain `li`. The profile-signature question is separate.

3. **THE SHADOW CASCADE, half closed** (unchanged from part 16's hand-off). Ours is
   fixed (the window-coordinate clip); the title's own clear rects are
   `(0,0)-(480,512)` and `(960,0)-(1024,1024)` for a 1024x1024 map, at z=1.0 with
   compare func ALWAYS. Three readings, **test the third first because it is free**:
   (a) 480x512 is a pixel extent wanting a x2 (the pass reports `msaa=0`); (b) the
   "cascade" is several maps packed into one 4096-wide surface; (c) the uncleared
   region is never sampled and the shadows fail at the CONSUMER — probe the fetch
   coordinates of the pass that samples `1439B000(depth)`, 629,023 fetches a boot.

4. **No mipmaps have ever been uploaded** — `ci.mipLevels = 1` in `CreateImage`, every
   texture, every phase. The operator's "all textures seem weird grainy". Real work:
   the Xenos mip chain has its own address layout.

5. **The Still Creek sign's dark smear, and the GAS roundel.** `CZ_VK_SKIP_TEX` to give
   each an address, then `CZ_VK_TEX_DUMP`. Not the untiler, not a shadow.

6. **Colour is flat and green-shifted** (§6ad item 2). Re-ask it: it was last judged on
   a binary where the LUT snapshot could go stale (gotcha 172).

7. **The conservative screen extent is still a placeholder** (part 11). Do not do this
   speculatively — the cost has still not been shown to matter.

8. **The depth-resolve cost, if it ever matters.** ~6% of the frame rate, no measured
   problem behind it.

9. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
   next piece of work (finding 49, gotcha 107). Going further needs a gameplay
   comparison built from A2 — and a run that reaches the prologue is the first this
   port has had that could exercise one.

10. **Audio output and XMA decoding (phase 6).** DEMOTED by part 16: it is no longer a
    candidate for the prologue blocker, so it is back to being "the game is silent".
    The kick bitmap at `0x7FEA1A80` lands in ordinary flat memory and is inert; a real
    decoder needs that aperture trapped as MMIO. `CZ_XMA_NULL_DECODER` is the
    half-implementation to build on — it already models input consumption at a rate.

11. **A VFS gap, recorded rather than fixed** (§6ah(vi)). `VfsTranslate` returns empty
    for any path with no `:`, so a guest path with no device prefix can never resolve.
    A boot makes 29 such opens (`data\anim\weapon\<Weapon>.big`), all of which would
    have failed anyway because the package ships only `allweapons.big`. On console a
    relative path resolves against the title's own directory. It costs nothing today
    and CLAUDE.md already warns that at least one path here is built at runtime.

## Traps this session paid for — do not re-buy them

* **A capped print is not a count, and a THINNED print is not a distribution.** Gotcha
  109 has been in this file for six sessions and it still cost two false readings in
  one afternoon, off an instrument whose own comment quoted it. Watching a RANGE, the
  print budget's head is consumed by whichever register the guest writes first, so the
  others read as "never written". Watching four registers with a 1-in-4096 tail samples
  one lane and invites "every write is <that value>". When the question is "which
  values, how often", the instrument has to be a histogram.
* **Attribute a count to the BRANCH, not to the callee.** The first XMA probe hooked
  `sub_828638D0` on the strength of one call site and called it "the finished handler".
  It has two call sites in that one function and the counter read 284,354 where the
  truth was 0.
* **An arm's polarity is a design decision, not a detail.** The instant-consumption
  null decoder makes voices *never* play, which is the opposite end of the axis from
  the stock runtime rather than the middle. Refuting a hypothesis needs the middle
  configuration too.
* **An arm that manufactures progress needs a way to stop.** `CZ_FAKE_PRESS_SEQ` held
  its last button forever, so every prologue observation this port has ever made was
  taken while the title was being poked every 8 s.
* **Negative results are the deliverable when the hypothesis was inherited.** Four of
  this session's five results are "it is not that". Each cost a build and a run and
  each has an arm behind it, which is what stops the next session paying again.
* **Run timed arms serially** (gotcha 183) — unchanged.

## New instruments and arms

```
CZ_XMA_PROBE=1             the guest's own IsPlaying predicate (sub_82862A90), the
                           per-context dry test (sub_8285EFE0) and the per-update edge
                           detector (sub_82864808), with the raw XMA context words and
                           the hardware kick bitmap beside them, on a 5 s clock
CZ_XMA_NULL_DECODER=1      AN ARM: a decoder that consumes its input and produces
                           nothing, so voices can finish. Announces itself; never on
                           for a gate run
CZ_XMA_NULL_DECODER_MS_PER_PKT=N   its rate, ms of audio per 2048-byte packet
                           (default 40; 0 = retire the whole buffer instantly, which
                           tests "nothing ever plays" and NOT "everything finishes")
CZ_GUEST_LOG=1             the ENGINE'S OWN debug printf, via its formatted-string
                           sink sub_828223A0 (640 callers upstream). Silent today
                           because the call sites are gated on debug bytes — that is
                           a fact about the FLAGS, not about the categories
CZ_PM4_CONST_WATCH=<hex>[-<hex>]   now a per-register value HISTOGRAM on a 15 s clock
CZ_PM4_CONST_WATCH_FRAME=N         hold it until the era that matters
CZ_PM4_CONST_WATCH_ZEROS=1         the old zeros-only behaviour
CZ_FAKE_PRESS_SEQ=...,NONE         a real button with mask 0, so the arm can go quiet
```
