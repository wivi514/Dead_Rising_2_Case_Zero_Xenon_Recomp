#!/usr/bin/env python3
"""Read part 77's texture-pool A/B out of the frame traces the runs wrote.

WHY THIS READER EXISTS AND THE PART-75/76 ONES DO NOT FIT. Both of those partition a run
by DRAW COUNT, because both were built for items that change the cost of a draw. The image
memory pool changes the cost of a texture UPLOAD, which happens on a small minority of
frames and is invisible on the rest — so a draw-banded median is a fact about the 97% of
frames the change cannot reach, and it would report a real 100 ms saving as noise.

So: the population is frames with `texUploads > 0`, and the statistics are the ones a HITCH
is felt as (worst frame, p99, and the run's total decode). The upload-free frames are ALSO
reported, and they are the control channel this item genuinely has: the change cannot touch
them, so a difference there is the noise floor and a difference in it as large as the effect
would refute the reading (gotcha 452's other half — a control channel the change cannot
reach is the one worth having).

Usage:  tools/part77_tex_report.py <dir> [stamp]
"""
import sys, os, glob, statistics as st

def read(path):
    rows = []
    with open(path) as f:
        hdr = f.readline().split()
        idx = {n: i for i, n in enumerate(hdr)}
        for line in f:
            p = line.split()
            if len(p) < len(hdr):
                continue
            try:
                rows.append({k: float(p[i]) for k, i in idx.items() if i < len(p)})
            except ValueError:
                continue
    return rows

def pct(v, q):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]

def summarise(rows):
    up = [r for r in rows if r.get('texUploads', 0) > 0]
    no = [r for r in rows if r.get('texUploads', 0) == 0]
    return {
        'frames': len(rows), 'upFrames': len(up),
        'upWallMed': st.median([r['wallUs'] for r in up]) / 1000 if up else 0,
        'upWallP99': pct([r['wallUs'] for r in up], 0.99) / 1000,
        'upWallMax': max([r['wallUs'] for r in up]) / 1000 if up else 0,
        'decTot': sum(r.get('texDecUs', 0) for r in rows) / 1000,
        'decMax': max([r.get('texDecUs', 0) for r in up]) / 1000 if up else 0,
        'uploads': sum(r.get('texUploads', 0) for r in rows),
        'noWallMed': st.median([r['wallUs'] for r in no]) / 1000 if no else 0,
        # THE BURST FRAME: the single frame of the run that uploaded the most textures. On
        # this route that is the DebugJump load, ~780 uploads, and it is the same event in
        # every run — so it is a MATCHED comparison rather than a distribution statistic,
        # and it is what the pre-registered kill ("40 ms off the worst frame") is measured
        # against.
        'burstWall': (max(up, key=lambda r: r['texUploads'])['wallUs'] / 1000) if up else 0,
        'burstTex': max(r['texUploads'] for r in up) if up else 0,
        'burstDec': (max(up, key=lambda r: r['texUploads'])['texDecUs'] / 1000) if up else 0,
        'burstUp': (max(up, key=lambda r: r['texUploads'])['texUpUs'] / 1000) if up else 0,
        # How many frames of the whole run exceeded 150 ms — the population a player would
        # call a hitch. Reported with its cause split, because this route also produces
        # multi-second frames with ZERO uploads and zero pipelines (an OS or compositor
        # stall), and quoting the run's overall maximum would credit this change with
        # removing those. It cannot reach them.
        'slow150': sum(1 for r in rows if r['wallUs'] > 150000),
        'slow150tex': sum(1 for r in rows if r['wallUs'] > 150000
                          and r.get('texUploads', 0) > 0),
        # Per-upload, so a run that got further down the route cannot move it.
        'decPer1k': (sum(r.get('texDecUs', 0) for r in rows)
                     / max(1.0, sum(r.get('texUploads', 0) for r in rows))),
    }

def main():
    d = sys.argv[1]
    stamp = sys.argv[2] if len(sys.argv) > 2 else ''
    arms = {}
    for p in sorted(glob.glob(os.path.join(d, f'{stamp}*.trace'))):
        base = os.path.basename(p)
        arm = 'fix' if '_fix_' in base else ('ctl' if '_ctl_' in base else None)
        if not arm:
            continue
        rc = p.replace('.trace', '.rc')
        # A TRACE WITH NO `.rc` IS A RUN THAT HAS NOT FINISHED. The driver writes the exit
        # code after the process ends, so a missing one means the file is still being
        # appended to — and a partial trace reads as a complete SHORT run, whose run-total
        # columns (uploads, decode total, no-upload median) are then a fact about how far
        # it happened to have got. That is exactly what happened on the first pass of this
        # A/B: a control arm read at 6,178 frames against a finished arm's 16,685 and made
        # the totals look 43% apart.
        if not os.path.exists(rc):
            print(f'  SKIPPED {base}: still running (no .rc yet) — a partial trace is a '
                  f'complete run of a shorter route, not a shorter sample of this one')
            continue
        if open(rc).read().strip() != '0':
            print(f'  SKIPPED {base}: the route gate failed (rc='
                  f'{open(rc).read().strip()}) — an arm that never left the menu is not a '
                  f'measurement')
            continue
        arms.setdefault(arm, []).append((base, summarise(read(p))))

    for arm in ('ctl', 'fix'):
        print(f'\n--- {arm} ---')
        for base, s in arms.get(arm, []):
            print(f'  {base}: {s["frames"]:6d} frames, {s["upFrames"]:5d} with an upload '
                  f'({s["uploads"]:.0f} uploads) | upload-frame wall med {s["upWallMed"]:7.2f} '
                  f'p99 {s["upWallP99"]:8.2f} max {s["upWallMax"]:8.2f} ms | decode total '
                  f'{s["decTot"]:8.1f} ms, worst frame decode {s["decMax"]:7.1f} | '
                  f'no-upload med {s["noWallMed"]:6.2f} | BURST {s["burstTex"]:.0f} tex '
                  f'-> {s["burstWall"]:7.2f} ms (dec {s["burstDec"]:.1f} + up '
                  f'{s["burstUp"]:.1f}) | >150ms: {s["slow150"]} ({s["slow150tex"]} with '
                  f'uploads)')

    def agg(arm, k):
        v = [s[k] for _, s in arms.get(arm, [])]
        return st.median(v) if v else 0.0

    print('\n=== medians across runs ===')
    hdr = ('stat', 'ctl', 'fix', 'delta', '%')
    print('  %-26s %10s %10s %10s %8s' % hdr)
    for k, name in (('burstWall', 'BURST FRAME wall (ms)'),
                    ('burstTex', '  its uploads (must match)'),
                    ('burstDec', '  its decode'),
                    ('burstUp', '  its stage+submit'),
                    ('slow150', 'frames over 150 ms'),
                    ('slow150tex', '  of those, with uploads'),
                    ('decPer1k', 'decode us per upload'),
                    ('decTot', 'decode total (ms/run)'),
                    ('decMax', 'worst frame decode (ms)'),
                    ('upWallMed', 'upload-frame wall med'),
                    ('upWallP99', 'upload-frame wall p99'),
                    ('upWallMax', 'upload-frame wall max'),
                    ('noWallMed', 'NO-upload med (control)'),
                    ('uploads', 'uploads (must match)')):
        c, f = agg('ctl', k), agg('fix', k)
        d_ = f - c
        print('  %-26s %10.2f %10.2f %10.2f %7.1f%%'
              % (name, c, f, d_, 100.0 * d_ / c if c else 0.0))
    print('\n  The NO-upload row is the control channel: this change cannot reach a frame')
    print('  that uploads nothing, so its delta is the floor every other row is read against.')

main()
