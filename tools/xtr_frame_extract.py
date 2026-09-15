#!/usr/bin/env python3
"""Pull hardware's FRONT BUFFER out of a Xenia `.xtr` capture, and say what the
display gamma ramp does to it — the post-tone-map oracle for part 119.

WHY THIS EXISTS (2026-09-15, the operator: "interiors are really dark compared to the
Xbox 360"). Every picture comparison this port has ever made was against Xenia's PNG,
which is Xenia's OUTPUT: the front buffer AFTER the display controller's gamma ramp
(`xtr_gamma_ramp.py`; Xenia's `apply_gamma_table` shader, BSD-3, see
docs/lighting-plan-part119.md §2.2). Our renderer presents the front buffer AS
RESOLVED and has never read a DC_LUT register. So "is our picture too dark" has two
separable halves — is our FRONT BUFFER darker than hardware's, and is the missing ramp
the rest — and only the bytes inside the capture can separate them, because the PNG
already has the ramp baked in.

WHERE THE FRONT BUFFER IS IN A CAPTURE. This Xenia build does not record resolve
output as a `MemoryWrite` (every MemoryWrite in the R2/R4 captures is a 4- or 12-byte
fence/writeback). What it DOES record is every texture upload as a `MemoryRead`, and
the title samples the PREVIOUS frame's front buffer early in each frame, so a
3,768,320-byte MemoryRead (1280 x 736 rows x 4, the tiled height rounded to 32) at the
XE_SWAP's front-buffer address is hardware's front buffer ONE FRAME BEFORE the one the
PNG shows. The R2/R4 captures were taken standing still, so for a histogram the two
frames are the same place; for a pixel diff they are not (§6 of the plan says the PNG
is never a pixel oracle anyway).

The bytes are a tiled k_8_8_8_8 surface with the resolve's endian field 0 (the census
prints it: `xtr_resolve_census.py`, the FORMAT PAIR block) — so the dword is stored as
the GPU wrote it, low byte first. The channel order is settled by the picture, not by
a table: `--ppm` writes it and a wrong order is a blue Chuck.

WHAT IT PRINTS
  * the front buffer's per-channel value histogram summary (median / mean luma, the
    deciles) — this is the number our `CZ_VK_SNAP_DUMP` front buffer is compared to;
  * the same after the capture's own 256-entry gamma ramp — what Xenia PRESENTS;
  * if the PNG is next to the .xtr, the PNG's summary, and the residual between "ramp
    applied to the front buffer" and the PNG. A residual near zero is the proof that
    Xenia's output IS table[front buffer] and nothing else, i.e. that reproducing the
    picture is exactly one lookup at present time.

USAGE
    xtr_frame_extract.py <trace.xtr> [--ppm out.ppm] [--ramped-ppm out2.ppm]
                         [--png <xenia.png>] [--front-buffer 0x04BDC000]
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xtr  # noqa: E402
from xtr_gamma_ramp import read_ramp  # noqa: E402
from tex_decode import tiled2d  # noqa: E402

XE_SWAP = 0x64


def find_swap_and_read(data, n, want_base=None):
    """(front_buffer, width, height, raw_bytes) — the XE_SWAP packet's descriptor and
    the largest MemoryRead at that address."""
    swap = None
    reads = []
    for off, cid in xtr.walk(data, n):
        if cid == xtr.CMD_PACKET_START:
            count = struct.unpack_from('<I', data, off + 8)[0]
            if count >= 5:
                hdr = struct.unpack_from('>I', data, off + 12)[0]
                if (hdr >> 30) == 3 and ((hdr >> 8) & 0x7F) == XE_SWAP:
                    tag, fb, w, h = struct.unpack_from('>4I', data, off + 16)
                    if tag == 0x53574150:
                        swap = (fb, w, h)
        elif cid == xtr.CMD_MEMORY_READ:
            _t, base, enc, elen, dlen = struct.unpack_from('<5I', data, off)
            reads.append((base, enc, elen, dlen, off))
    if want_base is None:
        if swap is None:
            raise SystemExit('no XE_SWAP packet in the capture and no --front-buffer given')
        want_base = swap[0]
    key = want_base & 0x1FFFFFFF
    cands = [r for r in reads if (r[0] & 0x1FFFFFFF) == key]
    if not cands:
        raise SystemExit(f'no MemoryRead at {want_base:08X}; the title did not sample the '
                         f'front buffer this frame (it is not a resolve MemoryWrite in '
                         f'this Xenia build)')
    base, enc, elen, dlen, off = max(cands, key=lambda r: r[3])
    payload = data[off + 20:off + 20 + elen]
    if enc == 1:
        import cramjam
        payload = bytes(cramjam.snappy.decompress_raw(payload))
    return swap, want_base, bytes(payload)


def untile_8888(raw, w, h):
    """Tiled 32bpp -> (h, w, 4) uint8, dword low byte first."""
    pitch = (w + 31) & ~31
    xs = np.arange(w)[None, :]
    ys = np.arange(h)[:, None]
    # tiled2d is scalar Python; vectorising it by hand keeps this under a second.
    x, y = np.broadcast_arrays(xs, ys)
    l2b = 2
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (l2b + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << l2b
    off = (macro + ((micro & ~15) << 1) + (micro & 15) +
           ((y & 8) << (3 + l2b)) + ((y & 1) << 4))
    idx = ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> l2b)
    # Spot-check the vectorised form against the scalar reference on a few texels.
    for (px, py) in ((0, 0), (37, 5), (1279, 719), (640, 360)):
        assert idx[py, px] == tiled2d(px, py, pitch, l2b), (px, py)
    dwords = np.frombuffer(raw, dtype='<u4', count=pitch * ((h + 31) & ~31))
    px = dwords[idx]
    out = np.empty((h, w, 4), np.uint8)
    out[..., 0] = px & 0xFF
    out[..., 1] = (px >> 8) & 0xFF
    out[..., 2] = (px >> 16) & 0xFF
    out[..., 3] = (px >> 24) & 0xFF
    return out


def summary(name, rgb):
    r, g, b = (rgb[..., i].astype(np.float64) for i in range(3))
    luma = 0.299 * r + 0.587 * g + 0.114 * b
    q = np.percentile(luma, [10, 25, 50, 75, 90])
    print(f'  {name:<34} luma mean {luma.mean():6.2f}  median {q[2]:6.2f}  '
          f'p10 {q[0]:6.2f} p25 {q[1]:6.2f} p75 {q[3]:6.2f} p90 {q[4]:6.2f}  '
          f'r/g/b mean {r.mean():6.2f}/{g.mean():6.2f}/{b.mean():6.2f}')
    return luma


def write_ppm(path, rgb):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (rgb.shape[1], rgb.shape[0]))
        f.write(np.ascontiguousarray(rgb[..., :3]).tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('xtr')
    ap.add_argument('--front-buffer', type=lambda s: int(s, 16), default=None)
    ap.add_argument('--ppm', help='write the untiled front buffer (no ramp)')
    ap.add_argument('--ramped-ppm', help='write the front buffer through the capture\'s ramp')
    ap.add_argument('--png', help="Xenia's screenshot; default: the .png beside the .xtr")
    ap.add_argument('--order', default='rgba',
                    help='channel order of the dword, low byte first (default rgba)')
    args = ap.parse_args()

    data, hdr = xtr.open_trace(args.xtr)
    n = len(data)
    swap, fb, raw = find_swap_and_read(data, n, args.front_buffer)
    w, h = (swap[1], swap[2]) if swap else (1280, 720)
    print(f'{os.path.basename(args.xtr)}: XE_SWAP front buffer {fb:08X} {w}x{h}; '
          f'MemoryRead of it holds {len(raw):,} bytes '
          f'(tiled {((w + 31) & ~31)} x {((h + 31) & ~31)} x 4 = '
          f'{((w + 31) & ~31) * ((h + 31) & ~31) * 4:,})')
    img = untile_8888(raw, w, h)
    order = [{'r': 0, 'g': 1, 'b': 2, 'a': 3}[c] for c in args.order]
    rgb = img[..., order][..., :3]

    rw, table, _pwl = read_ramp(args.xtr)
    lut = np.array([[(v >> 20) & 0x3FF, (v >> 10) & 0x3FF, v & 0x3FF] for v in table],
                   dtype=np.float64)
    # Xenia's apply_gamma_table: out = table[in] / 1023, written to a 10-bit target and
    # then shown on an 8-bit screen; round to 8 bits the way a screenshot would.
    ramped = np.clip(np.rint(lut[rgb, [0, 1, 2]] * 255.0 / 1023.0), 0, 255).astype(np.uint8)

    print('front buffer (what the resolve wrote; what OUR present shows):')
    l_fb = summary('front buffer', rgb)
    print('after the capture\'s own 256-entry ramp (what Xenia presents):')
    l_rp = summary('front buffer -> ramp', ramped)

    png = args.png
    if png is None:
        cand = os.path.splitext(args.xtr)[0] + '.png'
        png = cand if os.path.exists(cand) else None
    if png:
        from PIL import Image
        pimg = np.asarray(Image.open(png).convert('RGB'))
        if pimg.shape[:2] != (h, w):
            print(f'  png is {pimg.shape[1]}x{pimg.shape[0]}, not {w}x{h}: '
                  f'histograms only')
        print(f"Xenia's screenshot ({os.path.basename(png)}), the NEXT frame:")
        l_png = summary('png', pimg)
        print(f'  residual, ramped front buffer vs png: median luma '
              f'{np.median(l_rp) - np.median(l_png):+.2f}, mean {l_rp.mean() - l_png.mean():+.2f}; '
              f'unramped vs png: median {np.median(l_fb) - np.median(l_png):+.2f}, '
              f'mean {l_fb.mean() - l_png.mean():+.2f}')
        if pimg.shape[:2] == (h, w):
            # Per-pixel: only meaningful where the two frames agree (a still camera).
            d_r = np.abs(ramped.astype(int) - pimg.astype(int))
            d_f = np.abs(rgb.astype(int) - pimg.astype(int))
            print(f'  per-pixel |diff| vs png, ramped: mean {d_r.mean():.2f}, '
                  f'share of pixels within 2 levels {np.mean(d_r.max(axis=2) <= 2) * 100:.1f}%; '
                  f'unramped: mean {d_f.mean():.2f}, within 2 levels '
                  f'{np.mean(d_f.max(axis=2) <= 2) * 100:.1f}%')

    if args.ppm:
        write_ppm(args.ppm, rgb)
        print(f'  wrote {args.ppm}')
    if args.ramped_ppm:
        write_ppm(args.ramped_ppm, ramped)
        print(f'  wrote {args.ramped_ppm}')


if __name__ == '__main__':
    main()
