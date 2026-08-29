#!/usr/bin/env bash
# Regenerate the vendored CI patches from the two local recompiler checkouts.
#
# WHY THESE EXIST. CI (release-plan E.1) clones the UPSTREAM hedge-dev repos,
# because they are public and this repo's checkouts are not — but both local trees
# carry commits upstream does not have (XenonRecomp 13: the devkit AES key, the LZX
# loader fixes, the barrier lowering; XenosRecomp 23: alpha-to-mask, user clip
# planes, the vfetch mask skip — docs/xenonrecomp-upstream-bugs.md is the ledger),
# and the runtime COMPILES against the patched interfaces (shader_recompiler.h,
# xex.h, ppc_context.h all differ). An upstream clone alone does not build this
# runtime, so CI applies these diffs over the pinned merge-base SHAs in
# .github/workflows/build.yml.
#
# RE-RUN THIS AFTER ANY COMMIT TO EITHER CHECKOUT, and commit the result here —
# a stale patch fails CI with a context mismatch, which is loud; a stale PIN
# (the SHA in the workflow) fails the same way. Both name this script.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XENON=${XENON_ROOT:-$HOME/GithubRepo/XenonRecomp}
XENOS=${XENOS_ROOT:-$HOME/GithubRepo/XenosRecomp}

for repo in "$XENON" "$XENOS"; do
    [ -d "$repo/.git" ] || { echo "FAIL: no checkout at $repo" >&2; exit 1; }
done

(cd "$XENON" && git diff origin/main..HEAD -- . ':(exclude)build*' ':(exclude)out*') \
    > "$ROOT/tools/ci/xenonrecomp-local.patch"
(cd "$XENOS" && git diff origin/main..HEAD -- XenosRecomp/ thirdparty/dxc-bin/inc) \
    > "$ROOT/tools/ci/xenosrecomp-local.patch"

echo "bases (pin these in .github/workflows/build.yml):"
echo "  XenonRecomp $(cd "$XENON" && git merge-base origin/main HEAD)"
echo "  XenosRecomp $(cd "$XENOS" && git merge-base origin/main HEAD)"
wc -c "$ROOT"/tools/ci/*.patch
