#!/usr/bin/env python3
"""Sample a RUNNING cz_runtime's XMA context array without stopping it.

WHY THIS EXISTS
---------------
The operator was stuck in a live defect — the prologue cinematic ping-ponging — and the
question was which field of the XMA hardware context oscillates. A `gdb -p` attach
answers that and ptrace-STOPS the game for a second, which during a timing defect
contaminates the very thing being measured, and which is intolerable when a human is
sitting in front of the window. `process_vm_readv` reads another process's memory with
the same permission model and no interruption at all (CLAUDE.md says to prefer it; this
is the tool that does it).

What it found, in one 6-second pass, is recorded in docs/open-items.md 00j: four MONO
dialogue voices appear when speech starts, their `output_buffer_valid` toggles ~94
times in 8 s, the stereo music voice's never toggles once, and every voice's
`input_buffer_read_offset` advances monotonically — so the streams run forward and it
is the SYNC that reverses. None of that is visible from a log.

It reports only fields that CHANGE, and for each it counts steps up against steps down,
because "goes backward" was the property under investigation and a field that only ever
rises cannot be the one driving a reversing timeline.

INPUTS, all printed by the runtime itself at startup:
  pid    `pgrep -x cz_runtime`
  base   "runtime: guest memory at 0x..."
  array  "[audio] XMA context array: ... at BFFEB000"

USAGE
    python3 tools/xma_live_sample.py <pid> <hostBaseHex> <ctxArrayHex> <seconds>
"""
import ctypes, struct, sys, time
libc = ctypes.CDLL("libc.so.6", use_errno=True)
class iovec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]
libc.process_vm_readv.restype = ctypes.c_ssize_t

def read(pid, addr, n):
    buf = (ctypes.c_char * n)()
    l = iovec(ctypes.cast(buf, ctypes.c_void_p), n)
    r = iovec(ctypes.c_void_p(addr), n)
    got = libc.process_vm_readv(pid, ctypes.byref(l), 1, ctypes.byref(r), 1, 0)
    if got != n:
        raise OSError(ctypes.get_errno(), "process_vm_readv")
    return bytes(buf)

pid  = int(sys.argv[1]); base = int(sys.argv[2], 16)
arr  = int(sys.argv[3], 16); secs = float(sys.argv[4])
NCTX, CTXSZ = 8, 64
samples = []
t0 = time.time()
while time.time() - t0 < secs:
    blob = read(pid, base + arr, NCTX * CTXSZ)
    samples.append((time.time() - t0,
                    [struct.unpack_from(">16I", blob, c * CTXSZ) for c in range(NCTX)]))
    time.sleep(0.02)

print(f"{len(samples)} samples over {secs}s\n")
for c in range(NCTX):
    series = [s[1][c] for s in samples]
    if all(v == series[0] for v in series) and series[0][7] == 0:
        continue                      # never used
    print(f"--- ctx{c} ---")
    for w in range(16):
        vals = [s[w] for s in series]
        uniq = sorted(set(vals))
        if len(uniq) == 1:
            continue                  # constant: not the oscillator
        # Does it go DOWN as well as up? That is the signature we are hunting.
        down = sum(1 for a, b in zip(vals, vals[1:]) if b < a)
        up   = sum(1 for a, b in zip(vals, vals[1:]) if b > a)
        print(f"  dw[{w:2d}]  {len(uniq):5d} distinct  up={up:4d} down={down:4d}  "
              f"min={min(uniq):08X} max={max(uniq):08X}")
        if len(uniq) <= 12:
            print(f"           values: {[f'{v:08X}' for v in uniq]}")
