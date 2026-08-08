#!/usr/bin/env python3
"""Sample the GPU's clock, power state and utilisation while something runs.

Why this exists
---------------
This project spent five sessions quoting GPU numbers without asking what clock the GPU
was at, and then over-corrected: it adopted `sudo nvidia-smi -lgc 2100,2100` as the
standing measurement configuration, on the strength of an overnight session that
measured **P8 / 210 MHz while the game rendered**.

That reading was an artifact of the MONITOR BEING ASLEEP. `docs/phase5-notes.md` §6al
recorded `display_active: Disabled` beside its own result and guessed as much, but could
not test it. Re-measured with the display awake, this runtime sits at **P5, a mean of
521 MHz, 32% utilisation, 28.6 W** through gameplay and crowds — which is where `vkcube`,
a normal presenting Vulkan application, settles on the same machine (P5, 510-600 MHz,
33-39%, 29.5 W). The governor was never mistreating us.

So the clock lock was buying comparability at the price of representativeness: it forced
52.8 W to finish, sooner, work the frame was not waiting on, and it made every GPU figure
this port owned describe a machine no player will be running. The honest replacement is
to SAMPLE the clock during the run and quote what it was, which is what this does.

Read the two columns together, because on their own each is misleading:

* a LOW clock at LOW utilisation is the governor being CORRECT — the GPU has little to
  do, and finishing it sooner would change nothing;
* a low clock at HIGH utilisation would be the governor being wrong, and that is the
  only case in which forcing clocks is a legitimate measurement rather than a thumb on
  the scale.

Case Zero is firmly the first: 32% utilisation means the GPU is idle two thirds of every
frame, because the renderer submits and then blocks on the fence, so our CPU and our GPU
never run at the same time. That is the thing to fix, not the clock.

Usage
-----
    tools/gpu_clock_sample.py --csv /tmp/clk.csv -- ./cz_runtime --smoke
    tools/gpu_clock_sample.py --duration 60                    # sample only

Prints one summary line suitable for pasting into a commit message; --csv writes every
sample.
"""

import argparse
import statistics
import subprocess
import sys
import time

FIELDS = "pstate,clocks.gr,utilization.gpu,power.draw,display_active"


def sample():
    """-> (pstate, MHz, util%, watts, display_active), or None if nvidia-smi cannot."""
    try:
        out = subprocess.run(
            ["nvidia-smi", f"--query-gpu={FIELDS}", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if out.returncode != 0:
        return None
    parts = [p.strip() for p in out.stdout.strip().split(",")]
    if len(parts) < 5:
        return None
    try:
        return parts[0], float(parts[1]), float(parts[2]), float(parts[3]), parts[4]
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=float, default=3.0, help="seconds between samples")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="sample for this long when no command is given")
    ap.add_argument("--skip", type=float, default=15.0,
                    help="exclude the first N seconds from the SUMMARY. A process's "
                         "startup burst reaches P0 and is not what the run ran at; the "
                         "samples are still written to --csv so the burst stays visible")
    ap.add_argument("--csv", help="write every sample here")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="-- followed by the command to run while sampling")
    args = ap.parse_args()

    if sample() is None:
        print("nvidia-smi is not available — no GPU clock to report", file=sys.stderr)
        return 2

    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd and args.duration <= 0:
        ap.error("give a command after -- or a --duration")

    child = subprocess.Popen(cmd) if cmd else None
    clocks, utils, watts, states, display = [], [], [], set(), set()
    t0 = time.monotonic()
    csv = open(args.csv, "w") if args.csv else None
    if csv:
        csv.write("elapsed_s,pstate,clock_mhz,util_pct,watts,display_active\n")
    try:
        while True:
            if child is not None and child.poll() is not None:
                break
            if child is None and time.monotonic() - t0 >= args.duration:
                break
            s = sample()
            elapsed = time.monotonic() - t0
            if s:
                if csv:
                    csv.write(f"{elapsed:.1f},{s[0]},{s[1]:.0f},{s[2]:.0f},"
                              f"{s[3]:.1f},{s[4]}\n")
                if elapsed >= args.skip:
                    clocks.append(s[1])
                    utils.append(s[2])
                    watts.append(s[3])
                    states.add(s[0])
                    display.add(s[4])
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        if csv:
            csv.close()
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait(timeout=10)

    if not clocks:
        print("no samples after --skip; lower it or run for longer", file=sys.stderr)
        return 1

    # The display state belongs in the summary line, not in a footnote: a blanked
    # monitor is what produced this project's P8/210 MHz reading, and a number quoted
    # without it cannot be compared with one taken while someone was watching.
    print(f"GPU over {len(clocks)} samples ({args.interval:.0f} s apart, "
          f"first {args.skip:.0f} s excluded): "
          f"clock mean {statistics.mean(clocks):.0f} MHz "
          f"(min {min(clocks):.0f}, max {max(clocks):.0f}), "
          f"utilisation mean {statistics.mean(utils):.0f}% "
          f"(max {max(utils):.0f}%), "
          f"power mean {statistics.mean(watts):.1f} W, "
          f"pstate {'/'.join(sorted(states))}, "
          f"display_active {'/'.join(sorted(display))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
