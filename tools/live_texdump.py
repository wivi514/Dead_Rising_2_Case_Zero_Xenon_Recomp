#!/usr/bin/env python3
"""Dump every texture the NEWEST F9 census binds, out of the LIVE process.

WHY THIS EXISTS (gotcha 285): an address-based dump taken minutes after the frame
reads recycled streaming-heap memory — plausible garbage that supports any theory.
Part 35's first dumps were late and nearly convicted the wrong subsystem. The
protocol that works: the operator presses F9 (CZ_CAPTURE_KEY) and STANDS STILL,
and this fires within seconds — process_vm_readv, no ptrace stop, the game never
notices. The census supplies address/extent/format, so each dump is self-describing
and decodable offline (see phase5-notes §6bi for the DXT decode + Tiled2DOffset).

The guest base is parsed from the newest .log near the capture dir ("runtime: guest
memory at 0x..."), and fetch addresses are physical: host = base + 0xA0000000 + addr.

Usage: live_texdump.py <capture_dir> <out_subdir>
Finds the newest capture_f*.census, parses every sN= fetch, computes each
texture's byte size from extent+format, reads base+0xA0000000+addr via
process_vm_readv (no ptrace stop), writes <addr>_<WxH>_fmt<N>.bin files.
"""
import sys, os, re, glob, ctypes

BPP = {  # format -> (bits per texel, block-compressed)
    '2': (8, False), '6': (32, False), '18': (4, True), '19': (8, True),
    '20': (8, True), '47': (4, True), '48': (4, True), '49': (8, True),
    '22': (32, False), '10': (16, False), '4': (16, False), '26': (16, False),
}

def main():
    cap_dir, out_sub = sys.argv[1], sys.argv[2]
    # NEWEST BY MODIFICATION TIME, NOT BY FRAME NUMBER. The frame counter restarts at
    # zero every launch, so across two sessions in one capture directory the highest
    # frame number is not the latest press -- part 36 lost an operator's F9 that way,
    # re-dumping the previous session's frame and reporting success for it. An explicit
    # census path as argv[3] overrides the choice entirely.
    if len(sys.argv) > 3:
        census = sys.argv[3]
    else:
        census = max(glob.glob(f'{cap_dir}/capture_f*.census'), key=os.path.getmtime)
    frame = re.search(r'f(\d+)', census).group(1)
    out = f'{cap_dir}/{out_sub}_f{frame}'
    os.makedirs(out, exist_ok=True)

    pid = int(os.popen('pgrep -x cz_runtime').read().split()[0])
    log = open(sorted(glob.glob(f'{cap_dir}/*.log') + glob.glob(f'{cap_dir}/../reload_test.log') + glob.glob(f'{cap_dir}/session.log'), key=os.path.getmtime)[-1]).read(65536)
    base = int(re.search(r'guest memory at (0x[0-9a-f]+)', log).group(1), 16)

    libc = ctypes.CDLL('libc.so.6', use_errno=True)
    class iovec(ctypes.Structure):
        _fields_ = [('iov_base', ctypes.c_void_p), ('iov_len', ctypes.c_size_t)]
    def read(addr, size):
        buf = ctypes.create_string_buffer(size)
        loc, rem = iovec(ctypes.cast(buf, ctypes.c_void_p), size), iovec(ctypes.c_void_p(addr), size)
        n = libc.process_vm_readv(pid, ctypes.byref(loc), 1, ctypes.byref(rem), 1, 0)
        return buf.raw if n == size else None

    seen, ok, fail = set(), 0, 0
    for line in open(census):
        for m in re.finditer(r's\d+=([0-9A-Fa-f]{8}) (\d+)x(\d+)(?:x\d+)? fmt=(\w+)'
                             r'(?:.*?tiled=(\d+) pitchBlk=(\d+))?', line):
            addr, w, h, fmt = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4)
            if addr in seen or fmt not in BPP:
                continue
            seen.add(addr)
            bits, blocked = BPP[fmt]
            # SIZE BY THE TILED FOOTPRINT, not by w*h*bpp. A tiled Xenos surface is
            # stored in 32x32-unit macro tiles, so both its pitch and its row count
            # round up to 32 units — exactly the rule the runtime's own untiler uses.
            # Sizing at w*h short-reads every tiled texture whose height is not a
            # multiple of the tile, and a short dump does not announce itself: it
            # decodes as a texture whose right-hand blocks are missing, which reads as
            # a decode defect. Part 39's 256x64 sign was dumped at 8 KB of 16 KB on
            # BOTH sides of a cross-platform md5 pairing; the pairing held only because
            # both tools truncated identically.
            unit = 4 if blocked else 1
            uw, uh = (w + unit - 1) // unit, (h + unit - 1) // unit
            tiled = m.group(5) != '0' if m.group(5) else True
            pitch = int(m.group(6)) * 32 // unit if m.group(6) and m.group(6) != '0' \
                else ((uw + 31) & ~31)
            if tiled:
                pitch, uh = (pitch + 31) & ~31, (uh + 31) & ~31
            size = pitch * uh * unit * unit * bits // 8
            if size == 0 or size > 8 << 20:
                continue
            data = read(base + 0xA0000000 + addr, size)
            if data is None:
                fail += 1
                continue
            open(f'{out}/{m.group(1)}_{w}x{h}_fmt{fmt}.bin', 'wb').write(data)
            ok += 1
    print(f'frame {frame}: dumped {ok} textures to {out}, {fail} unreadable, '
          f'{len(seen) - ok - fail} skipped')

if __name__ == '__main__':
    main()
