# The autonomous performance plan — work I can run without the operator

**Written 2026-08-23 on the operator's instruction:** *"Prepare a performance increase plan
that doesn't require me. If it requires a run, just debug jump to case 0-2 and once you are
in game move the camera to right or left for 30 second to try to reproduce stutter."*

That is a standing authorisation to run the game myself **on that route only**, and it
supersedes `operator-runs-all-verification` for the runs described here. It does not
authorise anything else: a claim about their save, their load, or their felt experience
still needs them.

> Live state: `docs/part72-fix-plan.md` (what the operator session established),
> `docs/perf-plan-part72.md` (the item table), `phase5-notes.md` §6df/§6dg/§6dh.

---

## 0. THE ROUTE, AND WHAT IT IS NOT

```
CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000
CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE, <then RSRIGHT/RSLEFT ...>
```

`F2` opens the shipped DebugJump screen, `DOWN` selects **Case 0-2** (which spawns
outdoors), and **`WAITJUMP` is the load-bearing part**: the request is HELD until the
frontend exists and lands whenever it lands — 27 s on one boot, 131 s on another — so the
barrier parks the sequence until the screen is actually up and starts the remaining
intervals from that moment. A fixed-time recipe is a fit to one afternoon (gotchas 75,
251). Then a stick entry HOLDS for its whole interval, so `RSRIGHT`/`RSLEFT` turn the
camera continuously, which is the 30 seconds the operator asked for.

**FOUR THINGS THIS ROUTE IS NOT, and every number below inherits all four.**

1. **IT IS A LIGHTER LOAD THAN THEIRS.** ~7,431 draws against the operator's 9,750. At
   ~2.35 us/draw that is a ~5.5 ms smaller frame before anything else differs. **No result
   from this route is a claim about their frame** — it is a claim about this one, and an
   item that wins here still has to be re-checked at their load before it ships (gotcha
   356: an A/B measures the load it sampled, and six campaigns once landed on 2,500 draws
   while the operator played at 6,700).
2. **HEADLESS HIDES A WHOLE PHASE.** `Host_PresentPixels` returns immediately when there is
   no window, so `readback` has read **0.0% on every headless run in this project's
   history** — while windowed it is 8.1-8.7% at 720p and 16.4-22.6% at 1440p. **So these
   runs go WINDOWED**, at the operator's own `cz_settings.txt` resolution, or they are
   measuring a renderer that does not exist.
3. **IT CANNOT SEE WHAT THE OPERATOR SEES.** Every picture verdict in this part came from
   their eye and none from a counter. A performance change that quietly breaks the picture
   will pass every gate here. So: any item that touches what is DRAWN gets a picture gate
   (`CZ_VK_FRAME_DUMP` + era medians) and, before shipping, their look.
4. **IT IS ONE MACHINE, ONE DRIVER, ONE AFTERNOON.** Gotcha 433 is three sessions old: when
   something warms monotonically, ordering alone can produce three opposite conclusions.

**EVERY CAMPAIGN OPENS WITH A NULL ARM** — the same configuration twice — because a noise
floor measured on the day beats a remembered one (gotchas 50/51/86). Night Run 1 got the
floor to <=0.4% with soak + banded medians; anything smaller than the day's null is not a
result.

---

## 1. ITEM 1 — THE WIDE-CULLING OVER-WIDEN, finally priceable

**Why now:** the census that was to price it refuted itself in the operator session
(it projected object space and read 98.1% of the world off-screen). It is fixed, and
**verified offline against hardware's own draws** — 84.6% on-screen placed against 12.2%
unplaced over 12,560 draws in 20 `.xtr` traces. It has never been run in-engine since.

* Arms: `census` (default) and `census + CZ_NO_GAME_FOV=1`, both on this route.
* **The census now REFUSES to print a headline if its on-screen share is below 50%**, so a
  still-broken placement announces itself instead of handing me a plausible number.
* Read the WINDOWED RATE, not the run mean — the operator session's 62/frame was `C/n` over
  a transient and the steady state was 1.0 (gotcha 428).
* **Pre-registered kill: below 700 recovered draws (~1.75 ms) the item dies** and the
  21:9 flank fix stays exactly as it is.

## 2. ITEM E — geometry in VRAM, re-asked because item C landed

`CZ_VK_VRAM_STREAMS=1` measured **~14% SLOWER** and the reason was structural: we
re-uploaded the constant window every draw, so device-local memory was being written
through on the hot path (gotcha 363). **Item C cut that by 61.9%.** The plan's own text
says re-ask E after C lands; it has landed and the arm already exists.

* Zero new code. Three arms: null, `CZ_VK_VRAM_STREAMS=1`, and the same with
  `CZ_VK_NO_CONST_GATHER=1` — the third separates "VRAM is better now" from "VRAM is
  better *because of C*", which is the question worth answering for Case West.

## 3. ITEM A — build it, and hand the operator a PRICED decision instead of a policy question

Item A is blocked on a thread-budget call that is the operator's: the budget is 3, the
guard pool takes all 3, so a `record` pool is granted **zero**. That is a decision about
their machine — but **"is it worth taking a thread from the guard pool" is unanswerable
until somebody knows what the recorder is worth**, and that part is mine.

So: build it, and measure it with `CZ_WORKERS` raised explicitly for the experiment only.
That converts an abstract policy question into "it is worth N ms; do you want to spend a
guard thread on it?".

**Preconditions, in order, and the first two are cheap:**

1. **The pass-size HISTOGRAM (§4).** 48 scopes a frame averaging 122 draws is ample
   granularity — or it is two passes of 2,800 and 46 of 5, which has almost none. The mean
   cannot tell those apart and this part has been caught by that twice already.
2. **The order gate, which exists and is proven** (all 3,999 adjacent transpositions, and
   0 FAILED over 19.6M live draws). Any recorder runs with `CZ_VK_ORDER_GATE=1` armed from
   its first line.
3. Only then the recorder itself, per pass, with an inline fallback on every miss.

**Pre-registered kill: below 1.5 ms at this route's load, do not ship it** — the risk is
the highest of any item in the plan and a small win does not justify it.

## 4. THE PASS HISTOGRAM — a few lines, no new probe

`R->drawsThisPass` is already computed at every resolve. A per-frame top-N table plus a
bucket histogram, printed on the stats dump, sizes item A and costs nothing when unarmed.
**Do this first** — it is the cheapest thing in this document and it can kill §3 outright.

## 5. ITEM 0w — the menu/load stutter, which is where the stutter actually is

Today's binning of the operator session says heavy gameplay is smooth (`>2x med` **0.0%**
over 51 windows, median-of-worst 32.1 ms against a 26.63 ms median) and **every recurring
hitch is below ~2,000 draws** — menu and load, where the median window still holds a
~290 ms frame against a 5.7 ms median.

**This route passes through exactly that era**, which makes it the one item here the
operator's own instruction targets directly ("try to reproduce stutter"). The next step is
NOT a new probe: it is a per-frame top-N table for ONE more candidate, in the shape the
pipeline census already proved works (part 71 nailed a 3,891 ms frame with it). **Texture
upload bytes first** — it is the largest per-frame variable cost the renderer has and it
peaks exactly when new ground is streamed in.

## 6. ORDER OF WORK

```
1. §4  pass histogram              (offline build + 1 run)   — can kill §3
2. §5  per-frame texture table     (offline build + 1 run)   — the operator's own ask
3. §1  item 1 priced at last       (2 arms + null)
4. §2  item E re-asked             (3 arms, no new code)
5. §3  item A, only if §4 says the distribution supports it
```

§1 and §2 are pure measurement and cannot break anything. §4 and §5 add counters that are
inert when unarmed. §3 is the only item that changes how frames are recorded, and it is
last on purpose.
