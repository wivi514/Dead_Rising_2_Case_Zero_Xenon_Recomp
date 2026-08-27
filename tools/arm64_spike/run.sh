#!/bin/bash
# THE ARM64 SPIKE — does SIMDe lower the guest's VMX unit to NEON, or to a scalar loop?
#
# WHY IT EXISTS. `docs/release-plan.md` milestone C is the macOS/Apple Silicon port, and it
# was written with a pre-registered kill: *if the vector unit falls back to scalar, macOS is
# a different project*. `runtime/CMakeLists.txt` says the same thing from the other side —
# "-msse4.1 is not optional … without it simde silently falls back to scalar emulation,
# which is correct but very slow". So the question had to be answered before anything was
# promised, and it is answerable without a Mac.
#
# The ten operations are the ones the CMakeLists names plus the ones ppc_context.h's own
# helpers are built from. It compiles FREESTANDING (-nostdlibinc + clang's own headers), so
# it needs no aarch64 sysroot — which is the trick that makes this runnable on the x86_64
# dev box.
#
# RESULT ON 2026-08-27, clang 22.1.8: ten of ten NEON, zero scalar fallbacks,
# min/max_epu32 in two instructions. Re-run it natively on the Mac as item C.0 — this
# proves the LOWERING, not that the full image builds, and those are different claims.
#
#   tools/arm64_spike/run.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
S="${SIMDE:-$HOME/GithubRepo/XenonRecomp/thirdparty/simde}"
OUT="${OUT:-$(mktemp -d)}"
[ -d "$S" ] || { echo "!! no simde at $S (set SIMDE=)"; exit 2; }
CLANGINC="$(clang++ -print-resource-dir)/include"

for T in x86_64 aarch64; do
    EXTRA=""; [ "$T" = "x86_64" ] && EXTRA="-msse4.1 -mavx"
    # --target=<arch>-none-elf so no OS sysroot is consulted at all.
    clang++ --target=$T-none-elf -ffreestanding -nostdlibinc -isystem "$CLANGINC" \
        -O2 -std=c++20 -c -I"$S" $EXTRA \
        "$ROOT/tools/arm64_spike/simde_spike.cpp" -o "$OUT/spike_$T.o" || {
            echo "!! $T failed to compile"; exit 1; }
done

python3 - "$OUT" <<'PY'
import subprocess, re, sys
out = sys.argv[1]
def dis(o):
    t = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", o],
                       capture_output=True, text=True).stdout
    funcs, cur = {}, None
    for l in t.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+?)>:", l)
        if m:
            cur = m.group(1); funcs[cur] = []
        elif cur and ":" in l and l.strip()[:1] in "0123456789abcdef":
            funcs[cur].append(l.split(":", 1)[1].strip())
    return funcs
a = dis(out + "/spike_aarch64.o")
# A NEON instruction names a v/b/h/s/d/q register. A scalar fallback would be a long run of
# w/x general-purpose ops with no vector register in sight.
NEON = re.compile(r"\b(v[0-9]+|[bhsdq][0-9]+)\b")
print(f"\n{'op':16s} {'insns':>6s} {'NEON':>5s}   verdict")
print("-" * 44)
bad = 0
for f in sorted(a):
    ins = a[f]; simd = sum(1 for i in ins if NEON.search(i))
    ok = simd > 0
    if not ok: bad += 1
    print(f"{f:16s} {len(ins):6d} {simd:5d}   {'NEON' if ok else 'SCALAR  <-- KILL'}")
print(f"\n{len(a)-bad} of {len(a)} lowered to NEON")
sys.exit(1 if bad else 0)
PY
