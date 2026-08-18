#!/usr/bin/env python3
"""Per-THREAD CPU utilisation of a running cz_runtime, sampled from /proc.

WHY THIS EXISTS. Every performance number this project has produced since part 18 is a
WALL-CLOCK frame time or a per-phase share of one, and both are blind to the question a
reader asks first: is this a single-core problem or a parallel one? A 25 ms frame in
which one thread is saturated and fifteen cores idle is a completely different item from
a 25 ms frame in which eight threads are each half busy -- the first is fixed by making
one thread's work smaller or moving some of it off, the second is not fixed that way at
all. `docs/perf-plan-part50.md` prices items in milliseconds off the graphics pump
without ever having established which of those two it is.

The title itself is built to be parallel: A1 shows it naming `JobThread0` .. `JobThread5`,
`cAsyncFileSystem` and `BigFile Decompress Thread`, and our runtime gives each guest
thread a real host thread with no affinity pinning. So the capacity is there on both
sides and the only question is whether the work uses it.

WHAT IS MEASURED. utime+stime per thread out of /proc/PID/task/TID/stat, differenced over
a wall-clock window, expressed as a percentage of ONE core. A thread at 100% is one core
saturated; the process total against `nproc * 100` is how much of the machine is in use.

WHY THE DELTA AND NOT THE TOTAL. A thread's cumulative CPU time includes the boot, the
load and the menus, which is a different workload from the one being asked about. The
window is taken now, so it describes whatever the game is doing now -- point it at a
crowd, not at the title screen.

Threads are unnamed by our runtime (the GUEST names its threads through an exception
channel we log, but that name never reaches the host TID), so they are reported by TID
and ordered by cost. The first-seen order is stable enough to identify the pump: it is
created early and it is normally the busiest.

Usage: part50_thread_cpu.py [seconds]     # default 20
"""
import os, sys, time, glob

HZ = os.sysconf('SC_CLK_TCK')
# LOGICAL THREADS AND PHYSICAL CORES ARE NOT THE SAME NUMBER, and this tool reported the
# first as if it were the second for five parts. `os.cpu_count()` on the operator's machine
# returns 16; it is a Ryzen 7 5700 with **8 physical cores and 2 threads per core**. So
# every "3.75 of 16 cores (23% of the machine)" this project has quoted since part 50 is
# really 3.75 of 8 (**47%**), and the headroom for a worker pool is half what it looked
# like. Two SMT siblings share one core's execution resources: a second thread on a busy
# core buys maybe 20-30% on a mixed workload and nothing at all on one that is already
# saturating the same units.
#
# Both are printed, because both are the right denominator for a different question:
# logical for "how many runnable threads can we have", physical for "how much machine is
# left".
CORES = os.cpu_count()

def _physical_cores():
    """Physical cores from /proc/cpuinfo's (physical id, core id) pairs.

    Counted rather than divided by a threads-per-core constant, because that constant is
    wrong on anything heterogeneous -- an Intel P/E-core part has SMT on some cores and not
    others, and a single division would silently misreport it."""
    try:
        seen, phys, core = set(), None, None
        with open('/proc/cpuinfo') as f:
            for line in f:
                if line.startswith('physical id'):
                    phys = line.split(':')[1].strip()
                elif line.startswith('core id'):
                    core = line.split(':')[1].strip()
                elif not line.strip() and phys is not None and core is not None:
                    seen.add((phys, core)); phys = core = None
        if phys is not None and core is not None:
            seen.add((phys, core))
        return len(seen) or CORES
    except OSError:
        return CORES

PHYS = _physical_cores()


def find_pid():
    for p in glob.glob('/proc/[0-9]*'):
        try:
            if open(f'{p}/comm').read().strip().startswith('cz_runtime'):
                return int(p.rsplit('/', 1)[1])
        except OSError:
            continue
    return None


def sample(pid):
    """{tid: cpu_ticks} for every thread alive right now."""
    out = {}
    for t in glob.glob(f'/proc/{pid}/task/[0-9]*'):
        try:
            # The comm field is parenthesised and may contain spaces, so split on the
            # LAST ')' rather than on whitespace -- the classic /proc/stat parsing trap.
            fields = open(f'{t}/stat').read().rsplit(')', 1)[1].split()
            out[int(t.rsplit('/', 1)[1])] = int(fields[11]) + int(fields[12])  # utime+stime
        except (OSError, IndexError, ValueError):
            continue
    return out


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    pid = find_pid()
    if not pid:
        sys.exit('no cz_runtime process found')
    a, t0 = sample(pid), time.time()
    time.sleep(secs)
    b, t1 = sample(pid), time.time()
    elapsed = t1 - t0

    rows = []
    for tid, end in b.items():
        if tid in a:
            rows.append((100.0 * (end - a[tid]) / HZ / elapsed, tid))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)

    smt = f', {CORES // PHYS} threads/core' if PHYS and CORES > PHYS else ''
    print(f'pid {pid}, {len(rows)} threads, {elapsed:.1f} s window, '
          f'{PHYS} physical cores / {CORES} logical{smt}\n')
    print(f'{"thread":>10} {"% of one core":>14}   {"bar (100% = 1 core)":<40}')
    for pct, tid in rows:
        if pct < 0.5:
            continue
        print(f'{tid:>10} {pct:>13.1f}%   {"#" * min(40, int(pct / 2.5)):<40}')
    quiet = sum(1 for p, _ in rows if p < 0.5)
    if quiet:
        print(f'{"":>10} {"":>14}   ...and {quiet} thread(s) below 0.5%')

    print(f'\nprocess total   {total:8.1f}% of one core '
          f'= {total / 100:.2f} cores of {PHYS} PHYSICAL ({total / PHYS:.1f}% of the '
          f'machine), {total / CORES:.1f}% of {CORES} logical')
    if rows:
        busiest = rows[0][0]
        print(f'busiest thread  {busiest:8.1f}%  -- it is {100 * busiest / total:.0f}% '
              f'of all CPU this process is using')
        # The verdict, stated as a rule rather than left to the eye.
        if busiest > 85 and total < 200:
            print('\nVERDICT: EFFECTIVELY SINGLE-CORE. One thread is saturated and the '
                  'process is using less\n         than two cores, so the frame time is '
                  'that one thread and nothing else.')
        elif total / CORES > 50:
            print('\nVERDICT: genuinely parallel -- over half the machine is in use.')
        else:
            print(f'\nVERDICT: PARTIALLY parallel. {total / 100:.1f} cores busy, but the '
                  f'busiest single thread\n         is {busiest:.0f}%, so the critical '
                  'path is likely still one thread.')


if __name__ == '__main__':
    main()
