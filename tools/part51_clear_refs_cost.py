#!/usr/bin/env python3
"""Part 51 item 1: what does SOFT-DIRTY page tracking cost on a map this size?

WHY THIS EXISTS. `docs/part51-kickoff.md` names one serious candidate for the largest
remaining CPU item (2a, the vertex-stream content guard, which reads 26 MB of guest memory
every frame to answer "did this buffer change?"): stop asking the bytes and ask the kernel.
Linux offers exactly that -- write "4" to /proc/self/clear_refs to arm soft-dirty tracking,
then read /proc/self/pagemap, where bit 55 of each 8-byte entry says whether that page has
been written since the arm. A 128 KB buffer is 32 pages = 256 bytes of pagemap, against a
131,072-byte read for the hash. Three orders of magnitude, and EXACT rather than sampled.

The kickoff also names the cost that can kill it before any code is written, and it is the
arm, not the query: **clear_refs walks the page tables of the WHOLE process**, and it is
not per-VMA -- there is no way to ask for only the guest map. This runtime maps 7.6 GB of
address space with ~1.2 GB resident (measured on a live outdoor run, part 51). If that walk
costs milliseconds, one arm per frame costs more than the 26 MB of hashing it replaces and
the idea is dead for the price of one script.

WHAT IS MEASURED, and why the shape of the map matters more than its size: a page-table
walk costs per PRESENT page, not per reserved byte, so the benchmark reproduces both --
a large sparse reservation plus a controlled resident set that is swept. It also times the
pagemap read itself, because that is the per-buffer cost the fix would pay 400 times a
frame, and a cheap arm with an expensive query is equally dead.

THE THIRD MEASUREMENT IS THE ONE THAT IS EASY TO FORGET. Soft-dirty is armed by clearing,
so every frame pays the walk whether or not anything changed -- but the walk also WRITES
the page tables (it clears the dirty bit and write-protects the pages), so the first write
to each page afterwards takes a minor fault. That fault cost lands on the GUEST's thread,
not ours, which is exactly the thread part 51 item 0 is asking about. It is measured here
as the cost of re-touching the resident set after an arm.

Usage:  part51_clear_refs_cost.py [--resident-mb 1200] [--reps 20]
"""
import argparse
import ctypes
import mmap
import os
import statistics
import sys
import time

PAGE = 4096
libc = ctypes.CDLL("libc.so.6", use_errno=True)


def now():
    return time.perf_counter()


def arm_soft_dirty():
    """One soft-dirty arm. Returns seconds."""
    t0 = now()
    fd = os.open("/proc/self/clear_refs", os.O_WRONLY)
    os.write(fd, b"4")
    os.close(fd)
    return now() - t0


def arm_soft_dirty_prefetched(fd):
    """The same, with the open/close hoisted -- what a real implementation would do."""
    t0 = now()
    os.pwrite(fd, b"4", 0)
    return now() - t0


def read_pagemap(pm_fd, addr, nbytes):
    """Soft-dirty bits for [addr, addr+nbytes). Returns (seconds, dirty_page_count)."""
    first = addr // PAGE
    npages = (addr + nbytes + PAGE - 1) // PAGE - first
    t0 = now()
    raw = os.pread(pm_fd, npages * 8, first * 8)
    dirty = 0
    for i in range(0, len(raw), 8):
        entry = int.from_bytes(raw[i:i + 8], "little")
        if entry & (1 << 55):
            dirty += 1
    return now() - t0, dirty


def touch(buf, nbytes, stride=PAGE):
    """Write one byte per page over the first nbytes. Returns seconds."""
    t0 = now()
    mv = memoryview(buf)
    for off in range(0, nbytes, stride):
        mv[off] = 1
    return now() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reserve-gb", type=float, default=4.0,
                    help="sparse reservation, mimicking the flat guest map")
    ap.add_argument("--resident-mb", type=int, default=1200,
                    help="resident set to sweep up to (the live runtime measures ~1.2 GB)")
    ap.add_argument("--reps", type=int, default=20)
    args = ap.parse_args()

    reserve = int(args.reserve_gb * (1 << 30))
    print(f"reserving {args.reserve_gb} GB anonymous (MAP_NORESERVE), "
          f"page {PAGE}, reps {args.reps}")
    buf = mmap.mmap(-1, reserve, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | 0x4000,
                    prot=mmap.PROT_READ | mmap.PROT_WRITE)
    # The address of the mapping, needed to index pagemap.
    base = ctypes.addressof(ctypes.c_char.from_buffer(buf))
    print(f"base = 0x{base:x}")

    # Transparent huge pages change the answer in both directions (fewer PTEs to walk,
    # but a 2 MB page dirties wholesale), so report what the kernel is set to rather than
    # letting it be an unrecorded variable.
    try:
        with open("/sys/kernel/mm/transparent_hugepage/enabled") as f:
            print(f"THP: {f.read().strip()}")
    except OSError:
        print("THP: unknown")

    pm_fd = os.open("/proc/self/pagemap", os.O_RDONLY)
    cr_fd = os.open("/proc/self/clear_refs", os.O_WRONLY)

    print("\n=== 1. the ARM (clear_refs '4'), by resident page count ===")
    print(f"{'resident':>10} {'pages':>10} {'arm median':>12} {'arm p90':>10} "
          f"{'ns/page':>9}")
    steps = [0, 64, 256, 512, args.resident_mb]
    for mb in steps:
        if mb:
            touch(buf, mb << 20)
        pages = (mb << 20) // PAGE
        # The first arm after a big touch is not representative (it has the most work to
        # do and the page tables are hot); take reps and report the median, plus p90 so a
        # bimodal cost cannot hide behind a good median.
        samples = []
        for _ in range(args.reps):
            samples.append(arm_soft_dirty_prefetched(cr_fd))
            touch(buf, mb << 20) if mb else None
        med = statistics.median(samples)
        p90 = sorted(samples)[int(len(samples) * 0.9)]
        per = (med / pages * 1e9) if pages else float("nan")
        print(f"{mb:>8} MB {pages:>10} {med*1e3:>10.3f} ms {p90*1e3:>8.3f} ms "
              f"{per:>8.1f}")

    print("\n=== 2. the QUERY (pagemap read), by buffer size ===")
    print("what one stream's change-check would cost, against the hash it replaces")
    print(f"{'buffer':>10} {'pages':>7} {'read median':>13} {'vs 4 GB/s hash':>16}")
    for kb in (4, 16, 64, 128, 512, 2048):
        nbytes = kb << 10
        samples = []
        for _ in range(args.reps):
            samples.append(read_pagemap(pm_fd, base, nbytes)[0])
        med = statistics.median(samples)
        # The incumbent: part 47 measured the folded hash at 35.7 GB/s, but that is the
        # fold alone; the guard's own measured throughput over a session is nearer 4 GB/s
        # once misses are included. Quote the optimistic one so the comparison is hostile
        # to the new idea rather than flattering to it.
        hash_ns = nbytes / 35.7e9 * 1e9
        print(f"{kb:>7} KB {nbytes//PAGE:>7} {med*1e6:>11.2f} us "
              f"{hash_ns/1e3:>13.2f} us")

    print("\n=== 3. the AFTERMATH: write-protect faults the arm creates ===")
    print("clear_refs write-protects every page, so the next write to each takes a minor")
    print("fault -- and that lands on whichever thread writes, i.e. the GUEST's")
    mb = args.resident_mb
    touch(buf, mb << 20)
    warm = statistics.median([touch(buf, mb << 20) for _ in range(5)])
    faulted = []
    for _ in range(5):
        arm_soft_dirty_prefetched(cr_fd)
        faulted.append(touch(buf, mb << 20))
    fmed = statistics.median(faulted)
    pages = (mb << 20) // PAGE
    print(f"{mb} MB resident, {pages} pages")
    print(f"  re-touch, no arm : {warm*1e3:8.3f} ms")
    print(f"  re-touch after arm: {fmed*1e3:8.3f} ms   "
          f"(+{(fmed-warm)*1e3:.3f} ms = {(fmed-warm)/pages*1e9:.0f} ns/page)")

    os.close(pm_fd)
    os.close(cr_fd)


if __name__ == "__main__":
    sys.exit(main())
