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
    censuses = sorted(glob.glob(f'{cap_dir}/capture_f*.census'),
                      key=lambda p: int(re.search(r'f(\d+)', p).group(1)))
    census = censuses[-1]
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
        for m in re.finditer(r's\d+=([0-9A-Fa-f]{8}) (\d+)x(\d+)(?:x\d+)? fmt=(\w+)', line):
            addr, w, h, fmt = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4)
            if addr in seen or fmt not in BPP:
                continue
            seen.add(addr)
            bits, blocked = BPP[fmt]
            size = max(w, 4) * max(h, 4) * bits // 8 if blocked else w * h * bits // 8
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
