#!/usr/bin/env python3
"""Direct-call (`bl`) graph queries over the loaded XEX image.

WHY THIS EXISTS
---------------
Phase A of the D3D pivot is one question asked a hundred times: "who calls this
function?" — the API surface to hook is exactly the set of callers one level above
the ring-emit primitives, and CreateDevice is some number of caller-hops above
VdInitializeRingBuffer's wrapper. Answering it by grepping `ppc/` finds the TU but
not the enclosing function (a TU holds hundreds), and answering it by disassembling
candidates one at a time is how the recon session spent an afternoon on seven
functions. This scans the image's own `bl` instructions once (~2.2 M of them) and
attributes every call site to its enclosing function via the recompiler's function
map — the same map the runtime dispatches through, so "enclosing function" here
means what the image means, not a guess.

Limits, stated so nobody reads silence as a fact (gotcha 25): this sees DIRECT
calls only. A `bctrl` through a vtable is invisible, and a function whose only
callers are indirect prints an empty list that means "no *direct* callers", never
"dead code". Tail calls (`b` without link) are included when --tail is given, off
by default because unconditional `b` is also how plain branches spell themselves
and only cross-function `b` targets are meaningful.

Examples:
    # who calls the swap internal?
    python3 tools/guest_callers.py --callers 82841F00

    # what does the draw path call?  (one level down)
    python3 tools/guest_callers.py --callees 82845F68

    # callers-of-callers, deduped, for walking upward toward an API entry
    python3 tools/guest_callers.py --callers 8283CCE8 --up 2
"""
import argparse
import bisect
import re
import struct
import sys

DEFAULT_IMAGE = "assets/game/default_image.bin"
DEFAULT_MAPPING = "ppc/ppc_func_mapping.cpp"
DEFAULT_BASE = 0x82000000
TEXT_START = 0x82150000
TEXT_SIZE = 0x873564


def load_funcs(path):
    addrs = []
    rx = re.compile(r"\{\s*(0x[0-9A-Fa-f]+),")
    with open(path) as f:
        for line in f:
            m = rx.search(line)
            if m:
                addrs.append(int(m.group(1), 16))
    addrs.sort()
    return addrs


def enclosing(addrs, pc):
    i = bisect.bisect_right(addrs, pc) - 1
    return addrs[i] if i >= 0 else None


def scan(data, base, include_tail=False):
    """Return [(site, target, is_tail)] for every bl (and optionally b) in .text."""
    calls = []           # (site_addr, target_addr, is_tail)
    start = TEXT_START - base
    end = min(start + TEXT_SIZE, len(data) - 3)
    for off in range(start, end, 4):
        w = struct.unpack_from(">I", data, off)[0]
        if (w >> 26) != 18 or (w & 2):
            continue
        lk = w & 1
        if not lk and not include_tail:
            continue
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        site = base + off
        calls.append((site, (site + li) & 0xFFFFFFFF, not lk))
    return calls


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--callers", help="list call sites targeting this function (hex)")
    ap.add_argument("--callees", help="list direct calls made from inside this function (hex)")
    ap.add_argument("--up", type=int, default=1,
                    help="with --callers: walk N caller levels, deduped (default 1)")
    ap.add_argument("--tail", action="store_true",
                    help="include unconditional `b` whose target is another function's start")
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ap.add_argument("--mapping", default=DEFAULT_MAPPING)
    ap.add_argument("--base", default=hex(DEFAULT_BASE))
    args = ap.parse_args()

    base = int(args.base, 16)
    with open(args.image, "rb") as f:
        data = f.read()
    funcs = load_funcs(args.mapping)
    fset = set(funcs)
    calls = scan(data, base, include_tail=args.tail)

    if args.tail:
        # keep only cross-function b: target is a known start and differs from the
        # branch's own enclosing function
        calls = [(s, t, tl) for (s, t, tl) in calls
                 if not tl or (t in fset and enclosing(funcs, s) != t)]

    if args.callees:
        target_fn = int(args.callees, 16)
        i = funcs.index(target_fn)
        fn_end = funcs[i + 1] if i + 1 < len(funcs) else TEXT_START + TEXT_SIZE
        for site, tgt, tl in calls:
            if target_fn <= site < fn_end:
                kind = "b " if tl else "bl"
                print(f"{site:08X}  {kind} -> sub_{tgt:08X}")
        return

    if args.callers:
        wanted = {int(args.callers, 16)}
        seen_lvls = []
        for lvl in range(args.up):
            found = {}
            for site, tgt, tl in calls:
                if tgt in wanted:
                    fn = enclosing(funcs, site)
                    found.setdefault(fn, []).append((site, tgt, tl))
            seen_lvls.append(found)
            print(f"--- level {lvl + 1}: {len(found)} distinct caller(s) of "
                  f"{{{', '.join(f'sub_{w:08X}' for w in sorted(wanted))}}} ---")
            for fn in sorted(found):
                sites = found[fn]
                det = ", ".join(f"{s:08X}{'(b)' if tl else ''}" for s, t, tl in sites[:6])
                more = f" +{len(sites)-6} more" if len(sites) > 6 else ""
                print(f"  sub_{fn:08X}  [{det}{more}]")
            wanted = set(found)
            if not wanted:
                break
        return

    ap.error("one of --callers / --callees required")


if __name__ == "__main__":
    main()
