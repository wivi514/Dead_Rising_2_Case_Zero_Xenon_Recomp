#!/usr/bin/env python3
"""Turn a recorded operator route into a CZ_FAKE_PRESS_SEQ recipe.

WHY THIS EXISTS. `part80-kickoff.md` §1's item 1 — parallel command recording — carries one
blunt measurement rule: **8,000+ draws or not at all**. Below that the autonomous route is
GPU-bound and a CPU saving reads as a dead null, which part 79 spent a six-run campaign
re-learning (`phase5-notes.md` §6dw §3, gotchas 453 and 466). `tools/autoroute.sh` reaches
~6,200 draws. So every CPU item on the board was unmeasurable without the operator sitting
through every arm — three runs a side, an hour of their evening per A/B (gotcha 190 in its
most expensive form).

The operator's answer was to play the route themselves once, with `CZ_INPUT_TRACE=1`
carrying millisecond stamps (part 80 added the clock), and let the recipe be TRANSCRIBED.
Their route holds **9,300-9,700 draws** — the load the item actually has to be priced at.

WHY IT IS NOT A ONE-LINER. A pad trace is not a list of presses:

* **An analog stick at rest is never at rest.** This controller idles at L=(1485,4324),
  R=(2239,-695) and jitters constantly, so `PublishPad` fires on nearly every poll: 1,127
  trace lines for about a dozen actual inputs. Reading intent off raw lines by eye is how a
  transcription acquires a press nobody made.
* **A press is an INTERVAL, not an instant.** The trace records state changes, so a button
  appears on the line where it goes down and disappears on the line where it comes up. The
  recipe needs the duration between them, and for a stick that duration IS the input — a
  150 ms nudge and a four-second walk are the same entry name with different meanings.
* **The gaps matter as much as the presses.** The dead time between two menu presses is
  the game loading, and it is not compressible: a recipe that walks the menu faster than
  the screens appear walks the wrong menu (gotcha 251, and the whole reason WAITJUMP
  exists). So the silences are emitted as `NONE@MS` rather than dropped.

THE THRESHOLD IS THE ONE JUDGEMENT CALL, and it is deliberately the same one the trace
printer makes: **half deflection**. Not "any deflection", which would turn resting drift
into a walk entry, and not something tuned to this pad, which would be a fit to one
controller. Half is also the point past which recording and replay stop being comparable
at all, because the replay side only has full deflection — so it is the honest place to
say "the recipe could reproduce this".

WHAT IT CANNOT SEE, stated rather than glossed: **keyboard input**. F2/F4 and the debug
edges are SDL key events that never reach `PublishPad`, so a route using them shows up in
this transcription as a silence. If the recorded route needed a debug screen, the recipe
needs an `F2` and a `WAITJUMP` inserted by hand — the log's own
`[debug] ... through frontend manager ... at Ns` line is the anchor to place them against.

Usage:
    tools/part80_transcribe_route.py <recorded.log> [--hold-ms N] [--from T]
"""
import sys, re, argparse

LINE = re.compile(
    r"^\[input\] t=(\d+)\.(\d+)s pad (\d+) packet \d+\s+(\S.*?)\s+\| buttons=([0-9A-Fa-f]{4})"
    r" triggers=(\d+)/(\d+) L=\((-?\d+),(-?\d+)\) R=\((-?\d+),(-?\d+)\)")

# The replay side's names, in the order a compound state should be reported. Buttons first,
# then sticks: a recipe entry can only carry ONE of them, so the order is also the priority
# with which a compound press is reduced -- and the reduction is REPORTED, never silent.
BUTTONS = [(0x1000, "A"), (0x2000, "B"), (0x4000, "X"), (0x8000, "Y"),
           (0x0010, "START"), (0x0020, "BACK"),
           (0x0001, "UP"), (0x0002, "DOWN"), (0x0004, "LEFT"), (0x0008, "RIGHT")]

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--half", type=int, default=16383,
                help="unused since the analog path landed; kept so older invocations "
                     "do not fail. See --deadzone.")
ap.add_argument("--deadzone", type=int, default=6000,
                help="axis magnitude above which a stick counts as being USED (not as "
                     "being at full deflection). Must clear this pad's resting jitter, "
                     "which is a few thousand counts.")
ap.add_argument("--quantise", type=int, default=4000,
                help="axis quantisation. Coarser means fewer, more readable entries and a "
                     "less faithful path; 4000 is about 12%% of full deflection, which is "
                     "finer than the steering being reproduced.")
ap.add_argument("--from", dest="start", type=float, default=0.0,
                help="ignore everything before this many seconds")
ap.add_argument("--resample-ms", type=int, default=400,
                help="analog inputs are resampled to this granularity: the MEAN deflection "
                     "over each bucket becomes one entry. Without it a drifting stick "
                     "produces one entry per poll and the recipe is thousands of entries "
                     "long.")
ap.add_argument("--merge-ms", type=int, default=120,
                help="two identical states separated by less than this are one input; "
                     "an analog stick crosses the threshold and falls back repeatedly "
                     "while being held, and each crossing would otherwise be a new entry")
a = ap.parse_args()


def deflected(x, y):
    """Is this stick being used, as opposed to resting?

    The DEADZONE, not the half-deflection threshold, and the two are different jobs. Half
    deflection was the right line for the trace PRINTER, whose question is "is this an
    input the cardinal vocabulary could reproduce". The question here is "is the operator
    touching this stick at all", and the answer has to be yes at 17% deflection -- because
    17% is exactly what their steering was, and treating it as rest is the defect this
    whole analog path exists to fix. The pad in question idles at about (1485, 4324) and
    jitters a few hundred counts, so the deadzone has to clear roughly 5,000 to avoid
    reporting rest as motion; 6,000 is that with margin and is still a third of the
    smallest deflection anyone steers with.
    """
    return abs(x) > a.deadzone or abs(y) > a.deadzone


def quantise(v):
    """Round an axis so that jitter does not produce a new entry every sample."""
    q = a.quantise
    return max(-32768, min(32767, int(round(v / q)) * q))


def state_of(buttons, lx, ly, rx, ry, lt, rt):
    """The set of replay names this pad state corresponds to."""
    out = [n for m, n in BUTTONS if buttons & m]
    # ANALOG, not cardinal. The eight cardinal names can only say "full deflection in one
    # of eight directions", and the operator's own verdict on a cardinal transcription of
    # their route was that it walks into a building: over a 14.5-second walk their Y was
    # pinned at 32767 while X drifted -5,467..+3,993, which is continuous steering that
    # LSUP cannot express. So the axes are emitted as they were, quantised.
    if deflected(lx, ly):
        out.append(f"LS{quantise(lx)}/{quantise(ly)}")
    if deflected(rx, ry):
        out.append(f"RS{quantise(rx)}/{quantise(ry)}")
    if lt > 127:
        out.append("LT")
    if rt > 127:
        out.append("RT")
    return out


raw = []
for line in open(a.log, errors="replace"):
    m = LINE.match(line)
    if not m:
        continue
    ms = int(m.group(1)) * 1000 + int(m.group(2))
    if ms < a.start * 1000:
        continue
    raw.append((ms, int(m.group(5), 16), int(m.group(8)), int(m.group(9)),
                int(m.group(10)), int(m.group(11)), int(m.group(6)), int(m.group(7))))

# RESAMPLE THE ANALOG AXES ONTO A FIXED GRID, and do it before anything else looks at them.
#
# An analog stick emits a new value on nearly every poll -- 1,127 trace lines for about a
# dozen real inputs on the recorded route -- so quantising by VALUE alone still produces an
# entry every time the operator's thumb crosses a quantisation boundary, which during a
# slow sweep is constantly. Bucketing by TIME and taking the mean deflection over each
# bucket is what turns a continuous path into a short list of held positions.
#
# The mean rather than the first or last sample of the bucket: a stick moving steadily
# through a bucket is best represented by where it was on average, and the first/last
# choices bias the whole route consistently in one direction, which over a 14-second walk
# is metres of displacement.
buckets = {}
for ms, b, lx, ly, rx, ry, lt, rt in raw:
    buckets.setdefault(ms // a.resample_ms, []).append((lx, ly, rx, ry))
bucket_mean = {}
for k, v in buckets.items():
    n = len(v)
    bucket_mean[k] = tuple(sum(c[i] for c in v) // n for i in range(4))

samples = []
for ms, b, lx, ly, rx, ry, lt, rt in raw:
    mlx, mly, mrx, mry = bucket_mean[ms // a.resample_ms]
    samples.append((ms, state_of(b, mlx, mly, mrx, mry, lt, rt)))

if not samples:
    sys.exit("** no [input] lines matched -- was CZ_INPUT_TRACE=1 set, and is this the "
             "part-80 timestamped format?")

# Collapse to runs of identical state.
runs = []
for ms, st in samples:
    key = tuple(sorted(st))
    if runs and runs[-1][2] == key:
        runs[-1][1] = ms
    else:
        runs.append([ms, ms, key])

# A STATE PERSISTS UNTIL THE NEXT ONE IS OBSERVED, not until its own last sample -- and
# this has to happen BEFORE the merging below, which is the second ordering bug this tool
# has had (see the collapse/floor note above).
#
# `PublishPad` fires on CHANGE, so a run's last sample is the last time that state was
# REPORTED, not the last time it was true: it held until the next sample contradicted it.
# Two consequences, and they pull in opposite directions, which is why getting the order
# wrong is not merely untidy:
#
#   * A stick held through several quantisation steps produces adjacent runs separated by
#     one poll interval each. Ending each at its own last sample turns every one of those
#     into `NONE@11` -- twenty spurious stick releases during a single continuous walk.
#   * A button RELEASE is often a single sample, so its run has duration ZERO. Merged
#     before its extent is known, it looks like a momentary glitch and gets absorbed into
#     the press before it -- which turned a 145 ms `A` in the menu into a 2,248 ms hold.
#     The first version of this tool did exactly that, and a two-second held `A` in a menu
#     is not a slightly-wrong press, it is a different input.
#
# Resolving extents first makes both cases honest: the release becomes a genuine 2,103 ms
# silence, and the stick's inter-step gaps become zero-length and disappear on their own.
# The last run has no successor and keeps its own extent -- the one place where "how long
# did this really last" is genuinely unknown.
for i in range(len(runs) - 1):
    runs[i][1] = runs[i + 1][0]

# Merge a short gap back into the input either side of it. A stick held near the threshold
# dips under it for a poll or two; without this every dip becomes NONE@8 plus a fresh entry
# and the recipe is hundreds of entries long and reproduces nothing.
merged = []
for r in runs:
    if merged and merged[-1][2] == r[2] and r[0] - merged[-1][1] <= a.merge_ms:
        merged[-1][1] = r[1]
    elif (merged and not r[2] and r[1] - r[0] <= a.merge_ms
          and len(merged) and merged[-1][2]):
        merged[-1][1] = r[1]          # a momentary release inside a held input
    else:
        merged.append(list(r))

# Re-merge across the gaps just absorbed.
final = []
for r in merged:
    if final and final[-1][2] == r[2]:
        final[-1][1] = max(final[-1][1], r[1])
    else:
        final.append(r)


print(f"# transcribed from {a.log}")
print(f"# {len(samples)} pad samples -> {len(final)} distinct inputs "
      f"(half-deflection {a.half}, merge {a.merge_ms} ms)")
print("#")
print("# t (s)   duration   input")
seq = []
prev_end = None
for i, (t0, t1, st) in enumerate(final):
    if prev_end is not None and t0 - prev_end > 0:
        seq.append(("NONE", t0 - prev_end))
        print(f"# {prev_end/1000:8.3f}  {t0-prev_end:6d} ms  (waiting)")
    dur = max(t1 - t0, 1)
    # The LAST sample of a run has no successor, so its true end is unknown; a button that
    # was down at the final sample gets the trace's own resolution rather than a guess.
    # BOTH STICKS SURVIVE, because the replay now understands `LSx/y+RSx/y`. That matters
    # for a PERFORMANCE route specifically: the operator turns the camera while walking,
    # and the camera decides the draw set, which is the quantity being measured. An earlier
    # version emitted the left stick and dropped a live right-stick deflection of -12,000,
    # so the replay took the same path facing the wrong way.
    sticks = [n for n in st if n[:2] in ("LS", "RS")]
    others = [n for n in st if n not in sticks]
    if len(sticks) == 2 and not others:
        name = "+".join(sorted(sticks))          # LS before RS
    elif st:
        name = st[0]
    else:
        name = "NONE"
    dropped = [n for n in st if n not in name.split("+")]
    if dropped:
        # REPORTED, not silently dropped. A button plus a stick is still inexpressible as
        # one entry, and a compound that quietly became one of its halves is a recipe that
        # means something different from what was played.
        print(f"#          ** COMPOUND {'+'.join(st)} -> emitting {name}, DROPPING "
              f"{'+'.join(dropped)}")
    if st:
        print(f"# {t0/1000:8.3f}  {dur:6d} ms  {'+'.join(st)}")
    seq.append((name, dur))
    prev_end = t1

# ADJACENT SILENCES ARE ONE SILENCE, and this has to happen BEFORE the flooring below or
# the flooring cannot find the wait it needs to borrow from: a press followed by NONE@39
# and then NONE@4296 looks, to a rule that only sees its immediate successor, like a press
# with nowhere to borrow. The first version of this tool ordered them the other way and
# reported "everything after it shifts 119 ms LATER" for a press with four seconds of
# silence after it.
def collapse_silences(pairs):
    out = []
    for n, d in pairs:
        if out and n == "NONE" and out[-1][0] == "NONE":
            out[-1][1] += d
        else:
            out.append([n, d])
    assert sum(d for _, d in out) == sum(d for _, d in pairs), "collapsing changed the timing"
    return [(n, d) for n, d in out]

seq = collapse_silences(seq)

# A BUTTON ENTRY SHORTER THAN A POLL INTERVAL IS A PRESS THAT NEVER HAPPENS, and this is
# the failure that would have wasted a whole campaign silently. The replay makes a button
# entry active for the first 150 ms of its window, but only a poll landing INSIDE that
# window delivers it -- and the guest polls input roughly once a frame, every 7-13 ms. The
# raw trace of this route contains an `A` whose recorded state lasted **1 ms**, because the
# operator's press happened to straddle two samples. Emitted literally, that entry has a
# 1 ms delivery window, the poll misses it about nineteen times in twenty, and the recipe
# silently stops one screen short of the crowd -- which then reads as "the route is
# unreliable" rather than as a transcription defect. It is the exact shape of the 150 ms
# DebugJump race part 54 had to fix (gotcha 251).
#
# So every button entry is floored at the replay's own tap length, and the time is BORROWED
# FROM THE FOLLOWING SILENCE rather than added, so the rest of the route keeps its timing
# to the millisecond. Widening a press is safe in a way that shifting a menu is not: the
# title debounces a held button, but it cannot debounce a screen that appears late.
TAP_MS = 150
FLOOR_MS = 160          # the tap, plus one poll interval of margin
floored = []
for n, d in seq:
    floored.append([n, d])
for i, (n, d) in enumerate(floored):
    if n == "NONE" or n.startswith(("LS", "RS")) or d >= FLOOR_MS:
        continue
    need = FLOOR_MS - d
    # Borrow from the next silence; if there is none long enough, extend and say so.
    if i + 1 < len(floored) and floored[i + 1][0] == "NONE" and floored[i + 1][1] > need:
        floored[i + 1][1] -= need
        floored[i][1] = FLOOR_MS
        print(f"#          ** {n} was {d} ms -- floored to {FLOOR_MS} ms, borrowed from "
              f"the following wait (route timing unchanged)")
    else:
        floored[i][1] = FLOOR_MS
        print(f"#          ** {n} was {d} ms -- floored to {FLOOR_MS} ms; NO following "
              f"wait to borrow from, so everything after it shifts {need} ms LATER")
seq = [(n, d) for n, d in floored]
# ADJACENT SILENCES ARE ONE SILENCE. The run-collapsing above emits a NONE for every gap
# it finds, and a stick that dips under the threshold produces several in a row; left as
# they are, the recipe carries a dozen NONE entries that mean nothing individually and are
# impossible to check by reading. Summing them changes no timing at all -- the total is
# identical -- and turns the recipe back into something a human can audit against the
# annotated timeline printed above, which is the only review this transcription gets.
collapsed = collapse_silences(seq)

print("#")
print("# THE ANCHOR PROBLEM, stated because the recipe cannot state it itself. The times")
print("# above are from PROCESS START; the replay's clock starts at the title's FIRST INPUT")
print("# POLL, which is a different and better zero (it is the frontend coming up, not the")
print("# process). They differ by however long this boot took, and boot depth here is a")
print("# distribution, not a constant -- 24 s to 131 s on measured runs (gotcha 75). So the")
print("# leading NONE is the one number in this recipe that is a fit to one afternoon, and")
print("# it is the one to check with the draw gate before trusting anything measured on it.")
print("#")
print("CZ_FAKE_PRESS_SEQ=" + ",".join(f"{n}@{d}" for n, d in collapsed) + ",NONE")
