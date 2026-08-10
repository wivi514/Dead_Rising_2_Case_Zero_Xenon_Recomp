# Xenia ground-truth captures — request round 2 (the WORLD, not the boot)

**Written 2026-08-10 (phase C part 26). Round 1's list is `xenia-capture-requests.md` and
is closed.** This one exists because part 26 established two things on the same day:

* The operator's white ground, white props, blown-out glass and wrong reflections are
  **OUR defects** — Xenia renders those surfaces correctly (open item 00f).
* **Seven hypotheses for the white ground have been refuted**, each by a measurement, and
  every one of them was about an INPUT: the textures, the coordinates, the dummy, the
  clear, the surface format, the constants. The inputs are all correct. What is left needs
  a comparison against something that renders it right, and this repository has no such
  thing for the world — round 1's `.xtr` captures are the BOOT (B1) and a bounded Still
  Creek gameplay slice (B2), and neither contains the material in question.

**Why a capture answers all of it at once.** A `.xtr` is not only register writes. The
format carries `MemoryRead`, `MemoryWrite` and `EdramSnapshot` commands, so a trace holds
the actual bytes the GPU sampled. For every draw in it we can recover: the shader pair,
the six fetch-constant dwords of every texture slot, **the texture contents themselves**,
the render state, and the shadow cascade's own render and resolve. That is "which texture
belongs on this object", "what it should look like" and "how the shadow behaves" from one
artifact, for every object on screen — rather than one surface per operator trip.

---

## THE NON-NEGOTIABLES (round 1 lost a capture to the first one)

1. **`license_mask = 1`.** The default boots the TRIAL, whose behaviour differs measurably
   (finding 1). Every file in this round must be the full game.
2. **Render at 1280x720**, the same as our runtime, so a screenshot can be compared with
   ours pixel-for-pixel rather than by impression.
3. **Same method as B1/B2** for the trace itself — you have done this before and your
   notes are better than my guess at the flags. Keep `dump_shaders` on for the whole
   session; it costs nothing and every new area is a shader our cache may not hold.
4. **Keep each trace SHORT — a few seconds, standing still.** B1 was 1.61 GiB for a boot.
   Seven ten-second traces are more useful than one long roam, because each one is
   anchored to a place I can name.

## R1 — THE PAIRED SET. This is the whole request; everything else is optional.

For each location below: **stand still, take a screenshot, and capture a short trace from
the same spot without moving.** The pairing is what makes the comparison admissible — a
picture tells me what it should look like, and the trace tells me why.

| # | location | what is wrong on our side |
|---|---|---|
| 1 | **Case 0-2 spawn, turned around** | the large flat white ground patch — the top item |
| 2 | **gas station forecourt** | the same white ground, a second instance of one material |
| 3 | **pawn shop frontage** | glass showing a ProtoMan cardboard, and it CHANGES between sessions |
| 4 | **Uncle Bill's bathroom window** | blown out to white; poisoning our dummies proved it samples an unfilled texture slot |
| 5 | **the newspaper boxes and the cactus** | fully white props |
| 6 | **a cash register and the white door side** | reported, never captured cleanly |
| 7 | **the slot machine** | its frame was lost to a bug in my own tooling; one press recovers it |

**Name the files by location** (`w1_spawn`, `w2_gasstation`, ...), so a trace and its
screenshot can never be paired wrongly.

**Also take our own screenshot at each of the seven**, from as close to the same viewpoint
as you can manage:
```
cd ~/GithubRepo/Dead_Rising_2_Case_Zero_Xenon_Recomp/runtime/build
CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_VK_DRAW_CENSUS=$HOME/DR2CZ-troubleshooting/r2/ours.txt ./cz_runtime
```
F9 at each spot writes that frame's full draw list (one file per frame, named by frame).
Save the screenshots with **Save As** into `~/DR2CZ-troubleshooting/r2/` — Spectacle keeps
only the most recent one otherwise, and round 1 of this hunt lost seven that way.

## R2 — the shadow question, which needs one specific thing

Our shadow cascade is declared 4096x1024 and is sampled by the ground shader with a 4-tap
percentage-closer filter. The shadows we draw look correct in shape, so the lookup works;
what is unknown is what the cascade CONTAINS on hardware and at what extent it is resolved.
A trace at location 1 or 2 already carries it — **no extra work**, but say in the notes
whether the sun was overhead or low, because the cascade split distances (`pc24` reads
5.0 / 3.0 / 1.5) make that a different picture.

## R3 — optional, only if the above is easy

A single roaming trace through Still Creek. Lower value than the seven anchored ones: a
long trace is hard to index and every conclusion from it still has to name a place. Do this
only if the paired set went quickly.

## What I will do with it, so you can judge whether it is worth your evening

1. Extract, per draw, hardware's shader pair + fetch constants + **the texture bytes**, and
   diff that against our own `CZ_VK_DRAW_CENSUS` of the same location. Bindings, contents
   and state, all three at once.
2. For the white ground specifically: pull the exact albedo texture hardware sampled and
   compare it with what we upload from guest memory. If they differ, the defect is in our
   read; if they match, it is in our shading, and I will have eliminated the last input.
3. Fill the shader cache with anything new — every area nobody has visited is a gap that
   shows up as one silent log line and skipped draws.

**Matching note for my own benefit:** texture ADDRESSES are per-session allocations and do
not carry across runs — part 26 scanned 186,398 draws of B2 for the ground material's
addresses and found none, which is why the paired screenshot matters. Matching is by
signature (extent, format, and the draw's vertex count), and a picture of the same place
is what makes a signature match believable.
