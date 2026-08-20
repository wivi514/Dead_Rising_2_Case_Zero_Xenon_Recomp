#!/usr/bin/env python3
"""Which SPACE does the hardware dot a user clip plane in? Answered offline from the
part-57 captures, without a view matrix.

WHY THIS EXISTS. Part 57 shipped user clip planes (the zombie-slicing mechanism) dotting
the RAW exported clip position, and closed with two residuals — a see-through cut and an
occasionally doubled slab — plus the hypothesis that a systematic SPACE error in our dot
eats the gore plug (part58-kickoff §1). Testing that hypothesis looked blocked on the
scene VIEW matrix, which the ten .pose files were supposed to carry and do not: in all
ten, the frame's biggest draw was a SHADOW-pass draw, so bvc0-3 is the light's ORTHO
matrix (bvc3 = (0,0,0,1); it maps the player inside a unit box — checked, not assumed).

THE TRICK THAT UNBLOCKS IT: the view matrix is not needed. If the register plane P is
meant to be dotted with the clip-space position (clip = Proj * view_pos), then
    dot(P, Proj * v) = dot(Projᵀ * P, v)
i.e. Projᵀ alone maps the captured plane into VIEW space — and view space is a rigid
transform of world space, so lengths there are true meters. A game engine normalizes the
world/view plane it hands to D3D SetClipPlane (or the normal's length is the bone scale,
within a few percent of 1). So the discriminator is:

    |xyz of (Projᵀ P)| ≈ 1 for every captured plane  <=>  P is a clip-space plane
                                                            for exactly this Proj.

The raw register values have |xyz| ≈ 22-41 with c ≈ -d (a near-cancellation), which is
what a unit view plane looks like after (Projᵀ)⁻¹: the z/w components blow up by ~1/zn
and cancel. No alternative space survives the test (run it: view/world-as-given fail by
20-40x, and a plane already in NDC has the same zero set as the clip reading, so it is
not a distinct hypothesis).

The Proj comes from the pose's FIRST-draw constants vc0-3, which in these captures hold
a clean perspective matrix (row3 = (0,0,1,0), i.e. w_clip = z_view): 45° fov, 16:9,
zn=0.1, zf=1000. The tool verifies that shape before using it and falls back to the
last-seen Proj when a pose's first draw was something else.

WHAT ELSE IT REPORTS, because the same transform answers them for free:
  * per-piece pairing (stencil ref B0+i <-> gore ref AC+i, meshes matched by v0),
    and the view-space angle between a piece's BODY plane and its GORE/cap plane;
  * the true meaning of CZ_VK_CLIP_BIAS: `w += eps*|P|` adds eps*|P| to the VIEW
    plane's z-COEFFICIENT (since q_z = c*P22 + d and the bias lands entirely in d),
    i.e. the arm ROTATES the plane, it does not translate it. The tool prints the
    boundary displacement in METERS at the plane's own camera distance, which is what
    part 57's eps=0.01 result actually measured (~1-4 m, the whole zombie — so the
    "body spans < 0.01|P|" margin inference in phase5-notes §6cm is retracted; the
    plug's real clearance was never measured).

Usage:
  clip_plane_space.py <dir>          # a clip_slice capture directory (pose+census pairs)
  clip_plane_space.py <dir> -v       # add the per-draw piece table
"""
import math
import os
import re
import sys
from collections import defaultdict


def parse_pose_proj(path):
    """Return the 4x4 from vc0-3 if it is a perspective projection, else None.
    The shape test is structural: row3 == (0,0,1,0) (w_clip = z_view) and rows 0/1
    diagonal — anything else (the shadow ortho has row3 = (0,0,0,1)) is refused."""
    vc = {}
    for line in open(path):
        m = re.match(r'vc(\d+)\s+(\S+) (\S+) (\S+) (\S+)', line)
        if m and int(m.group(1)) < 4:
            vc[int(m.group(1))] = [float(m.group(i)) for i in range(2, 6)]
    if len(vc) < 4:
        return None
    r0, r1, r2, r3 = vc[0], vc[1], vc[2], vc[3]
    if r3 != [0.0, 0.0, 1.0, 0.0]:
        return None
    if r0[1] or r0[2] or r0[3] or r1[0] or r1[2] or r1[3] or r2[0] or r2[1]:
        return None
    return [r0, r1, r2, r3]


def parse_player(path):
    for line in open(path):
        if line.startswith('player_pos'):
            p = line.split()
            return [float(p[1]), float(p[2]), float(p[3])]
    return None


CENSUS_FIELDS = re.compile(
    r'draw (\d+) verts=(\d+) .*?vs=(\w+) ps=(\w+) mask=(\w+) .*?'
    r'dc=(\w+) sr=(\w+) cl=(\w+) ucp=([-\d.e/+]+)(?: v0=([-\d.e/+]+))?')


def parse_census(path):
    """Yield (draw, verts, vs, ps, mask, dc, sr, plane[4], v0) for UCP-enabled draws."""
    out = []
    for line in open(path):
        if line.startswith('#'):
            continue
        m = CENSUS_FIELDS.match(line)
        if not m:
            continue
        cl = int(m.group(8), 16)
        if not (cl & 0x3F):          # PA_CL_CLIP_CNTL UCP_ENA bits — plane draws only
            continue
        plane = [float(x) for x in m.group(9).split('/')]
        v0 = m.group(10) or ''
        out.append(dict(draw=int(m.group(1)), verts=int(m.group(2)),
                        vs=m.group(3), ps=m.group(4), mask=m.group(5),
                        dc=m.group(6), sr=m.group(7), plane=tuple(plane), v0=v0))
    return out


def view_plane(P, pl):
    """q = Projᵀ · plane: the same plane expressed against view-space positions."""
    q = [sum(pl[i] * P[i][j] for i in range(4)) for j in range(4)]
    return q


def classify(dc, sr):
    """The four-pass technique of §6cm, by RB_DEPTHCONTROL + stencil ref:
    body prepass (mask=0, no stencil), body color (EQUAL, no stencil),
    stencil/depth write (two-sided REPLACE/ZERO, ref 0xB0+), gore paint (ref 0xAC+)."""
    d = int(dc, 16)
    ref = int(sr, 16) & 0xFF
    if not (d & 1):
        return 'body'
    return f'plug ref={ref:02X}'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv
    d = args[0] if args else os.path.expanduser(
        '~/DR2CZ-troubleshooting/part57-operator/clip_slice')

    poses = sorted(f for f in os.listdir(d) if f.endswith('.pose'))
    censi = sorted(f for f in os.listdir(d) if f.endswith('.census'))
    frame_of = lambda f: re.search(r'(\d+)', f).group(1).lstrip('0')
    pose_by_frame = {frame_of(f): f for f in poses}

    P_last = None
    all_norms = []
    all_planes = []
    print(f'{"frame":>7} {"class":<12} {"register plane (a,b,c,d)":<42} '
          f'{"|view n|":>8} {"cam dist":>8} {"bias@0.01 -> boundary move":>26}')
    for cf in censi:
        fr = frame_of(cf)
        pose = pose_by_frame.get(fr)
        P = parse_pose_proj(os.path.join(d, pose)) if pose else None
        if P is None and P_last is None:
            print(f'  {fr}: no perspective Proj in pose and none seen yet — skipped',
                  file=sys.stderr)
            continue
        if P is None:
            P = P_last
        P_last = P

        draws = parse_census(os.path.join(d, cf))
        # distinct planes, tagged with the classes that used them
        planes = defaultdict(set)
        pieces = defaultdict(set)      # v0-mesh -> {(class, plane)} for pairing
        for dr in draws:
            c = classify(dr['dc'], dr['sr'])
            planes[dr['plane']].add(c)
            if dr['v0']:
                pieces[dr['v0']].add((c, dr['plane']))

        for pl, classes in sorted(planes.items(), key=lambda kv: min(kv[1])):
            q = view_plane(P, pl)
            n = math.sqrt(q[0]**2 + q[1]**2 + q[2]**2)
            all_norms.append(n)
            all_planes.append(pl)
            cam = abs(q[3]) / n if n else float('inf')
            # CZ_VK_CLIP_BIAS=eps: w += eps*|P| lands entirely in q_z. The plane's
            # boundary, at its own camera distance, moves by ~ dq_z * cam / |n|.
            mag = math.sqrt(sum(x * x for x in pl))
            dz = 0.01 * mag
            move = dz * cam / n if n else float('inf')
            cls = ','.join(sorted(classes))
            print(f'{fr:>7} {cls:<12} '
                  f'({pl[0]:9.5f},{pl[1]:9.5f},{pl[2]:9.3f},{pl[3]:9.3f}) '
                  f'{n:8.4f} {cam:7.2f}m {move:22.2f} m')

        # per-piece body<->plug plane relation (the wedge the gore pass keeps)
        if verbose:
            for v0, tags in sorted(pieces.items()):
                body = [p for c, p in tags if c == 'body']
                plug = [p for c, p in tags if c.startswith('plug ref=A')]
                if body and plug:
                    qa, qb = view_plane(P, body[0]), view_plane(P, plug[0])
                    na = math.sqrt(sum(x * x for x in qa[:3]))
                    nb = math.sqrt(sum(x * x for x in qb[:3]))
                    cosang = sum(qa[i] * qb[i] for i in range(3)) / (na * nb)
                    print(f'    {fr} piece v0={v0}: body/gore plane angle '
                          f'{math.degrees(math.acos(max(-1, min(1, cosang)))):.1f} deg')

    if all_norms:
        lo, hi = min(all_norms), max(all_norms)
        mean = sum(all_norms) / len(all_norms)
        print(f'\n{len(all_norms)} distinct planes: |view-space normal| '
              f'min {lo:.4f}  mean {mean:.4f}  max {hi:.4f}')
        print('VERDICT: ' + (
            'unit within a few percent -> the register planes are CLIP-SPACE planes '
            'for exactly this projection; the raw-oPos dot is the right space.'
            if 0.85 < lo and hi < 1.15 else
            'NOT unit -> the clip-space reading fails; re-derive the space.'))
        fit_scene_projection(P_last, all_planes)


def fit_scene_projection(P, planes):
    """Stage 2: the residual pattern under the pose's first-draw Proj is itself a
    measurement. Planes dominated by x/y come out |n| ~ 0.951 while mostly-z planes hit
    ~0.998 — a signature of the x/y SCALES being off by one common factor, i.e. the
    plane-building camera's fov differing from the first draw's 45 deg. Fit that factor
    (and the z-row scale) so all planes are unit at once; a sub-percent RMS closes the
    space question beyond argument, and the fitted Proj is the one later arms (the
    view-space plane SHIFT) should assume. On the part-57 clip_slice set: k=1.0520,
    fov 42.98 deg, 16:9 exact, RMS(|n|-1) = 0.0003 over 88 planes."""
    A, B, C = P[0][0], P[1][1], P[2][2]
    best = None
    for i in range(0, 121):
        k = 1 + i * 0.001
        for j in range(-40, 41):
            c2 = C + j * 0.00001
            s = 0.0
            for (a, b, c, dd) in planes:
                n = math.sqrt(k * k * (a * a * A * A + b * b * B * B)
                              + (c * c2 + dd) ** 2)
                s += (n - 1.0) ** 2
            if best is None or s < best[0]:
                best = (s, k, c2)
    s, k, c2 = best
    rms = math.sqrt(s / len(planes))
    ys = abs(B * k)
    print(f'fitted scene projection: xy scale x{k:.4f} '
          f'(fov {2 * math.degrees(math.atan(1 / ys)):.2f} deg, '
          f'aspect {ys / abs(A * k):.5f}), z row x{c2 / C:.6f}, '
          f'RMS(|n|-1) = {rms:.5f} over {len(planes)} planes')
    if rms < 0.005:
        print('  -> every plane is a unit view-space plane under ONE projection: '
              'the game normalizes the plane and hands D3D its clip-space image. '
              'No space error exists for our dot to make.')


if __name__ == '__main__':
    main()
