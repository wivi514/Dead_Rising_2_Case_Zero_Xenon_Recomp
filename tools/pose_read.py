#!/usr/bin/env python3
"""Read a .pose file written by the F9 capture: where the camera was, and which bytes
of the player object move when the player does.

WHY THIS EXISTS. Every picture finding in this port is anchored to "the operator walked
somewhere and pressed F9", which is not an experiment anyone can run twice — no headless
run can return to that spot, and the striped-material class picks a different streamed
quality level on each boot, so a second visit is a different measurement. The pose
capture records the camera and the player beside the picture so a shot can be restored.

The runtime writes RAW values on purpose (16 float4 vertex constants + the head of the
player game object). Nothing about the layout is guessed there; the two jobs below are
where it gets decided, because both can be fixed without rebuilding the game:

  camera   The vertex constants at the frame's first draw hold the view-projection.
           The eye is the world point that the matrix sends to the origin of clip
           space with w = 0, i.e. the solution of VP^T * [C,1] = 0 for the x/y/w rows.
           Which four constants form VP is NOT documented anywhere in this engine, so
           this tries every 4-constant window and reports the ones that yield a
           self-consistent eye (finite, and stable across the file's own duplicate
           matrices). Read the candidates, don't trust one blindly.

  player   `diff` two .pose files taken in DIFFERENT places: offsets whose float
           changed by a plausible world distance are position candidates, offsets that
           changed slightly are rotation/velocity, and offsets that did not change at
           all are not position. This is binding a field by what MOVES WITH the thing
           rather than by what looks plausible at one sample -- the error that cost
           this project a symbol table once already.

Usage:
  pose_read.py <a.pose>                 camera candidates + a summary
  pose_read.py <a.pose> <b.pose>        the above, plus the offsets that changed
"""
import sys
import re


def load(path):
    """Returns (vc, bvc, obj, meta). `vc` is the frame's FIRST draw — usually the shadow
    pass, whose view matrix is the LIGHT's — and `bvc` is the frame's BIGGEST draw, the
    scene camera. Prefer bvc; vc is kept because a pose written before part 36's fix has
    only that, and because the light's frustum is itself worth reading."""
    vc, bvc, obj, meta = {}, {}, {}, {}
    for line in open(path):
        if line.startswith('#'):
            m = re.search(r'cameraFingerprint (\w+)', line)
            if m:
                meta['camfp'] = m.group(1)
            continue
        p = line.split()
        if not p:
            continue
        if p[0].startswith('bvc'):
            bvc[int(p[0][3:])] = [float(x) for x in p[1:5]]
        elif p[0].startswith('vc'):
            vc[int(p[0][2:])] = [float(x) for x in p[1:5]]
        elif p[0].startswith('obj+'):
            obj[int(p[0][4:], 16)] = (int(p[1], 16), float(p[2]))
        elif p[0] == 'player_object':
            meta['obj'] = p[1]
        elif p[0] == 'controller':
            meta['controller'] = p[1]
    return vc, bvc, obj, meta


def solve3(a, b):
    """Tiny 3x3 solve by Cramer's rule; returns None when the system is singular."""
    def det(m):
        return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    d = det(a)
    if abs(d) < 1e-12:
        return None
    out = []
    for c in range(3):
        m = [row[:] for row in a]
        for r in range(3):
            m[r][c] = b[r]
        out.append(det(m) / d)
    return out


def eye_from(rows):
    """The camera centre of a world->clip matrix given as four rows.

    A point C is the eye when VP * [C,1] maps to the degenerate clip point: every row
    dotted with [C,1] is zero. Three rows determine C; the fourth is the consistency
    check the caller can apply.
    """
    a = [[rows[i][j] for j in range(3)] for i in (0, 1, 3)]
    b = [-rows[i][3] for i in (0, 1, 3)]
    return solve3(a, b)


def view_eye(vc, first=12):
    """The eye of a world->view matrix stored as three rows at vc[first..first+2].

    This is the reading that survived measurement: on a scene frame those three rows
    come out orthonormal to four decimals, so `eye = -R^T t` is an identity, not a fit,
    and the norms below are the check that says whether this pose is one of those.
    Returns (eye, row_norms, forward) or None.
    """
    if any((first + i) not in vc for i in range(3)):
        return None
    R = [vc[first + i][:3] for i in range(3)]
    t = [vc[first + i][3] for i in range(3)]
    norms = [round(sum(v * v for v in row) ** 0.5, 4) for row in R]
    eye = [-sum(R[r][c] * t[r] for r in range(3)) for c in range(3)]
    forward = [-R[2][c] for c in range(3)]
    return eye, norms, forward


def report_eye(label, vc):
    r = view_eye(vc)
    if not r:
        print(f'  {label}: no vc12..vc14 in this pose')
        return
    eye, norms, fwd = r
    ok = all(abs(n - 1.0) < 0.01 for n in norms)
    print(f'  {label}: eye=({eye[0]:.2f}, {eye[1]:.2f}, {eye[2]:.2f})  '
          f'fwd=({fwd[0]:.3f}, {fwd[1]:.3f}, {fwd[2]:.3f})  row_norms={norms}'
          + ('' if ok else '   <- NOT orthonormal: this is not a view matrix'))


def camera_candidates(vc):
    out = []
    for start in range(0, 13):
        rows = [vc.get(start + i) for i in range(4)]
        if any(r is None for r in rows):
            continue
        for label, m in (('rows', rows),
                         # The same four constants read as COLUMNS: D3D-era engines
                         # store either, and the transpose is a different matrix.
                         ('cols', [[rows[j][i] for j in range(4)] for i in range(4)])):
            e = eye_from(m)
            if not e:
                continue
            if all(abs(v) < 1e6 for v in e) and any(abs(v) > 1e-3 for v in e):
                out.append((start, label, e))
    return out


def main():
    vc, bvc, obj, meta = load(sys.argv[1])
    print(f'{sys.argv[1]}: player_object={meta.get("obj")} '
          f'controller={meta.get("controller")} camfp={meta.get("camfp")}')
    report_eye('first-draw view (usually the SHADOW pass — the LIGHT)', vc)
    if bvc:
        report_eye('biggest-draw view (the SCENE camera — use this one)', bvc)
    else:
        print('  (no bvc: pose written before the scene-camera fix)')
    cands = camera_candidates(vc)
    print(f'  camera eye candidates ({len(cands)}):')
    for start, how, e in cands:
        print(f'    vc{start}..vc{start+3} as {how}: '
              f'({e[0]:.2f}, {e[1]:.2f}, {e[2]:.2f})')
    if not cands:
        print('    NONE — no 4-constant window solved. Either the VP is not in vc0..15 '
              'on this frame, or the frame had no scene draw (a menu?).')

    if len(sys.argv) < 3:
        return
    vc2, bvc2, obj2, meta2 = load(sys.argv[2])
    print(f'\n{sys.argv[2]}: player_object={meta2.get("obj")}')
    cands2 = camera_candidates(vc2)
    for start, how, e in cands2:
        print(f'    vc{start}..vc{start+3} as {how}: '
              f'({e[0]:.2f}, {e[1]:.2f}, {e[2]:.2f})')
    # Windows that solved in BOTH files are the ones worth believing: a window that
    # only solves once is as likely to be arithmetic luck as a matrix.
    shared = {(s, h) for s, h, _ in cands} & {(s, h) for s, h, _ in cands2}
    print(f'  windows solving in BOTH: {sorted(shared)}')

    print('\n  player-object offsets that CHANGED between the two captures:')
    if not obj or not obj2:
        print('    (one of the captures has no object dump — no level running?)')
        return
    moved = []
    for off, (bits, f) in sorted(obj.items()):
        if off not in obj2:
            continue
        b2, f2 = obj2[off]
        if bits == b2:
            continue
        # A float that is finite and of world-ish magnitude in both samples is a
        # position candidate; everything else is reported but flagged.
        plaus = (abs(f) < 1e6 and abs(f2) < 1e6
                 and (abs(f) > 1e-3 or abs(f2) > 1e-3))
        moved.append((off, bits, f, b2, f2, plaus))
    print(f'    {len(moved)} of {len(obj)} dwords changed')
    runs = []
    for off, bits, f, b2, f2, plaus in moved:
        if plaus:
            runs.append(off)
        print(f'    obj+{off:04X}  {bits:08X} {f:12.3f}  ->  {b2:08X} {f2:12.3f}'
              f'{"   <- float-plausible" if plaus else ""}')
    # Three consecutive plausible floats is what a position looks like in memory.
    trip = [o for o in runs if (o + 4) in runs and (o + 8) in runs]
    if trip:
        print(f'\n  CONSECUTIVE-TRIPLE candidates (a position is 3 floats in a row): '
              + ', '.join(f'obj+{o:04X}' for o in trip))


if __name__ == '__main__':
    main()
