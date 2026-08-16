#!/usr/bin/env python3
"""Read a part-48 A/B on the PM4 WALK, in the one statistic that is admissible for it:
NANOSECONDS PER PACKET.

WHY NOT `outside` IN MILLISECONDS, which is what every earlier reader in this repo
quotes. Part 47 measured the operator's stream against the headless route's and found
they differ IN KIND, not just in size: 144 ns per packet against 110-113, and 7.8
register dwords per packet against 9.4. Two arms of one A/B are in the same position
with respect to each other -- the title's own AI drives the route, so the arms do not
submit the same command stream, and a run that walks 20% more packets spends 20% longer
walking whatever the change did. A matched DRAW band does not match a PM4 workload
(`docs/perf-plan-part48.md` §2 -- gotcha 321's shape, one subsystem over).

Dividing by the packets actually walked removes exactly that confound, and it is the
form the plan asks every walk item to be quoted in.

WHAT IS AND IS NOT NORMALISED AWAY. ns/packet is normalised by packet COUNT, not by
packet MIX, so it can still be moved by a window that walks proportionally more
register-heavy packets. That is why the opcode census (item 1a) prints alongside: this
reader reports the type-3 share and the register dwords per packet next to every number,
so a difference in ns/packet that is really a difference in what was walked is visible
rather than silent. If those two columns disagree between the arms, the ns/packet
comparison is weakened and the tool says so.

`pm4Ns` -- the numerator -- is the walk MINUS the renderer it drives, computed in
`vk_renderer.cpp` as `walkNs - known`, so a change to the draw path does not appear here
and a change here does not hide in the draw path.

Usage: part48_walk_read.py <dir> [armA armB ...]   # default: base + every other arm tag
"""
import sys, os, re, glob, statistics

# `[vkprof] pm4 43145164 packets (69142/frame, 91 ns each) | 425600193 register dwords
#  (682051/frame, 9.9/packet, 100.0% bulk)`
PM4 = re.compile(
    r'^\[vkprof\] pm4 (\d+) packets \((\d+)/frame, ([\d.]+) ns each\) \| '
    r'(\d+) register dwords \((\d+)/frame, ([\d.]+)/packet, ([\d.]+)% bulk\)')
# `[vkprof] pm4 types: t0(reg-run) 34.5% t1(reg-pair) 0.0% t2(filler) 28.7% t3(command) 36.8%`
TYPES = re.compile(
    r'^\[vkprof\] pm4 types: t0\(reg-run\) ([\d.]+)% t1\(reg-pair\) ([\d.]+)% '
    r't2\(filler\) ([\d.]+)% t3\(command\) ([\d.]+)%')
# The frame line that PRECEDES each pm4 line in a window, for the draw count. Windows are
# banded on it for the same reason the frame-time reader bands: a safehouse window and a
# crowd window pooled together measure the route (gotcha 321).
PROF = re.compile(r'^\[vkprof\] ([\d.]+) fps \(([\d.]+) ms/frame, (\d+) draws/frame\)')

# Only windows in this band are compared. Below it the run is indoors and every arm ties;
# the band is the same one part47_perf_read.py uses so the two readers agree on what a
# comparable window is.
LO, HI = 3000, 10 ** 9


def windows(path):
    """(draws, ns_per_packet, dwords_per_packet, t3_share, filler_share) per profile
    window. The pm4 and types lines are attributed to the most recent frame line, which
    is the window they were differenced over -- the profiler emits them as one block."""
    out, draws = [], None
    pend = None
    with open(path, errors='replace') as f:
        for line in f:
            m = PROF.match(line)
            if m:
                draws = int(m.group(3))
                pend = None
                continue
            m = PM4.match(line)
            if m and draws is not None:
                pend = (draws, float(m.group(3)), float(m.group(6)))
                continue
            m = TYPES.match(line)
            if m and pend:
                out.append(pend + (float(m.group(4)), float(m.group(3))))
                pend = None
    return out


def arm_logs(d, arm):
    """Every log belonging to `arm`. A headless campaign writes `<arm>_1.log` per run; an
    OPERATOR session writes a single `<arm>.log`. Both are real inputs to these readers,
    and a reader that globs only `<arm>_*.log` reports "no windows in band" for an
    operator session -- a zero from a check that could not have matched (gotcha 25)."""
    return sorted(glob.glob(os.path.join(d, f'{arm}_*.log')) +
                  glob.glob(os.path.join(d, f'{arm}.log')))


def med(xs):
    return statistics.median(xs) if xs else float('nan')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    arms = sys.argv[2:]
    if not arms:
        seen = {re.sub(r'(_\d+)?\.log$', '', os.path.basename(p))
                for p in glob.glob(os.path.join(d, '*.log'))}
        arms = ['base'] + sorted(seen - {'base'})

    print(f"PM4 walk, ns per packet -- windows drawing >= {LO} (the outdoor era)")
    print(f"{'arm':<20} {'ns/pkt':>9} {'spread':>14} {'dw/pkt':>8} {'t3%':>6} "
          f"{'filler%':>8} {'wins':>5} {'runs':>5}")
    table = {}
    for arm in arms:
        logs = arm_logs(d, arm)
        rows = [w for p in logs for w in windows(p) if LO <= w[0] < HI]
        if not rows:
            print(f"{arm:<20}   no windows in band ({len(logs)} logs)")
            continue
        ns = [r[1] for r in rows]
        table[arm] = rows
        print(f"{arm:<20} {med(ns):9.1f} {min(ns):6.0f}-{max(ns):<7.0f} "
              f"{med([r[2] for r in rows]):8.1f} {med([r[3] for r in rows]):6.1f} "
              f"{med([r[4] for r in rows]):8.1f} {len(rows):5d} {len(logs):5d}")

    if 'base' in table and len(table) > 1:
        b = med([r[1] for r in table['base']])
        bdw, bt3 = med([r[2] for r in table['base']]), med([r[3] for r in table['base']])
        print()
        for arm, rows in table.items():
            if arm == 'base':
                continue
            a = med([r[1] for r in rows])
            adw, at3 = med([r[2] for r in rows]), med([r[3] for r in rows])
            # The base is the CURRENT default and the arm undoes the change, so a base
            # that is FASTER than its arm is the change working. Said in words rather
            # than as a signed number, because the sign convention here has been got
            # wrong before in this repo and a percentage does not carry it.
            verdict = "the change is a WIN" if a > b else "the change is a LOSS"
            print(f"{arm:<20} {a:.1f} vs base {b:.1f} ns/packet "
                  f"({abs(a - b) / b * 100:.1f}% apart) -- {verdict}")
            # ...and the admissibility check the docstring promises.
            if abs(adw - bdw) / max(bdw, 1e-9) > 0.10 or abs(at3 - bt3) > 3.0:
                print(f"{'':20} !! the arms did not walk the same MIX "
                      f"({adw:.1f} vs {bdw:.1f} dwords/packet, t3 {at3:.1f}% vs "
                      f"{bt3:.1f}%) -- ns/packet is normalised by COUNT, not by mix, so "
                      f"this comparison is weakened")


if __name__ == '__main__':
    main()
