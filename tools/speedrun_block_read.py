#!/usr/bin/env python3
"""Read the running game's speedrun status block — the reference reader, and the gate.

WHY THIS EXISTS
---------------
`runtime/cpu/speedrun_block.cpp` publishes "is the game loading", "is a cutscene
playing" and "which cutscene" at a fixed host address so an external timer does not
have to chase pointers through a binary this project relinks every session. A publisher
nobody has read is a publisher nobody has shown to work: this is the second
implementation, written against the DOCUMENTED offsets rather than against the struct,
so a layout change that breaks a reader breaks this too.

It reads another process's memory with process_vm_readv, which does NOT ptrace-stop the
target — a gdb attach freezes the game for a second and would contaminate the very
loading it is measuring (see CLAUDE.md on recovering shader blobs the same way).

Usage:
    python3 tools/speedrun_block_read.py                 # find cz_runtime, print once
    python3 tools/speedrun_block_read.py --watch         # one line per change
    python3 tools/speedrun_block_read.py --pid 1234 --watch
"""
import argparse
import ctypes
import ctypes.util
import os
import struct
import sys
import time

BLOCK_ADDRESS = 0x0000435A00000000
MAGIC = b"CZSPDRN1"
SIZE = 256

STATE_NAMES = ["None", "Startup", "LegalScreen", "BCGIntro", "FrontEnd", "FEToGame",
               "Loading", "InGame", "InGameTut1", "FEToGameShow", "GameShow"]


class IOVec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]


_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
_libc.process_vm_readv.restype = ctypes.c_ssize_t
_libc.process_vm_readv.argtypes = [ctypes.c_int, ctypes.POINTER(IOVec), ctypes.c_ulong,
                                   ctypes.POINTER(IOVec), ctypes.c_ulong, ctypes.c_ulong]


def read_mem(pid, addr, size):
    buf = (ctypes.c_char * size)()
    local = IOVec(ctypes.cast(buf, ctypes.c_void_p), size)
    remote = IOVec(ctypes.c_void_p(addr), size)
    n = _libc.process_vm_readv(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if n != size:
        return None
    return bytes(buf)


def find_pid():
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/comm") as f:
                if f.read().strip() == "cz_runtime":
                    return int(entry)
        except OSError:
            continue
    return None


def scan_for_block(pid):
    """The fallback the runtime's own log tells you about: if the fixed address was
    taken, the block was relocated and only the magic finds it."""
    try:
        maps = open(f"/proc/{pid}/maps").read().splitlines()
    except OSError:
        return None
    for line in maps:
        rng, perms = line.split()[0], line.split()[1]
        if "r" not in perms or "w" not in perms:
            continue
        lo, hi = (int(x, 16) for x in rng.split("-"))
        if hi - lo > 64 << 20:        # the 4 GB guest map and friends are not it
            continue
        data = read_mem(pid, lo, hi - lo)
        if not data:
            continue
        off = data.find(MAGIC)
        if off >= 0:
            return lo + off
    return None


def decode(raw):
    """Unpack by OFFSET, exactly as an external tool would. Any disagreement with the
    C struct is a contract break and should fail here first."""
    f = {}
    f["magic"] = raw[0:8]
    f["version"], f["size"] = struct.unpack_from("<II", raw, 0x08)
    f["seq"], f["frame"], f["ns"], f["guestBase"] = struct.unpack_from("<QQQQ", raw, 0x10)
    f["state"], f["stateId"] = struct.unpack_from("<II", raw, 0x30)
    f["loading"], f["cutscene"], f["exclusive"], f["ingame"] = struct.unpack_from("<BBBB", raw, 0x38)
    f["loadCount"], f["cutsceneCount"] = struct.unpack_from("<II", raw, 0x3C)
    f["flow"], f["cineMgr"], f["cine"] = struct.unpack_from("<III", raw, 0x48)
    f["stateName"] = raw[0x58:0x68].split(b"\0")[0].decode("ascii", "replace")
    f["cutsceneName"] = raw[0x68:0xA8].split(b"\0")[0].decode("ascii", "replace")
    return f


def read_block(pid, addr):
    """Seqlock retry: the publisher sets seq odd across its write, so an even seq that
    is unchanged either side of the body is a consistent snapshot."""
    for _ in range(64):
        raw = read_mem(pid, addr, SIZE)
        if raw is None:
            return None
        s1 = struct.unpack_from("<Q", raw, 0x10)[0]
        if s1 & 1:
            continue
        raw2 = read_mem(pid, addr, SIZE)
        if raw2 is None:
            return None
        if struct.unpack_from("<Q", raw2, 0x10)[0] == s1:
            return decode(raw)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    ap.add_argument("--watch", action="store_true")
    ap.add_argument("--hz", type=float, default=30.0)
    args = ap.parse_args()

    pid = args.pid or find_pid()
    if not pid:
        sys.exit("no cz_runtime process found (pass --pid)")

    addr = BLOCK_ADDRESS
    probe = read_mem(pid, addr, 8)
    if probe != MAGIC:
        print(f"not at the fixed address (read {probe!r}); scanning...", file=sys.stderr)
        addr = scan_for_block(pid)
        if not addr:
            sys.exit("no block found — was the runtime built with it, and is it past boot?")
        print(f"found at 0x{addr:x}", file=sys.stderr)

    if not args.watch:
        f = read_block(pid, addr)
        if not f:
            sys.exit("could not take a consistent snapshot")
        for k, v in f.items():
            print(f"{k:14} {v}")
        return

    last = None
    while True:
        f = read_block(pid, addr)
        if f is None:
            print("target gone")
            return
        key = (f["state"], f["loading"], f["cutscene"], f["exclusive"], f["cutsceneName"])
        if key != last:
            last = key
            print(f"frame {f['frame']:>7}  state {f['state']:>2} {f['stateName']:<13} "
                  f"loading={f['loading']} cutscene={f['cutscene']}"
                  f"(excl={f['exclusive']})  loads={f['loadCount']} "
                  f"cines={f['cutsceneCount']}  '{f['cutsceneName']}'", flush=True)
        time.sleep(1.0 / args.hz)


if __name__ == "__main__":
    main()
