#!/usr/bin/env bash
# Wrap the staged Linux bundle as an AppImage (docs/release-plan.md §2.1 / E.2, part 104).
#
# WHAT AN APPIMAGE IS, so the decisions below read as decisions: a small static ELF (the
# "runtime", from AppImage/type2-runtime) with a squashfs appended. Executed, the runtime
# mounts its own tail with FUSE under /tmp/.mount_XXXXXX, exports APPDIR (the mount) and
# APPIMAGE (the file the player launched) to the environment, and runs AppRun inside the
# mount. The mount is READ-ONLY and moves every launch — which is why the runtime resolves
# its data root from APPIMAGE (host/host_paths.cpp, resolution step 1b) and why this AppDir
# ships NO assets/: an assets/ inside the mount would be found by the executable walk and
# then refuse every write.
#
# WHAT IT DOES NOT DO: lower the glibc floor. The binary inside is whatever was staged;
# the floor is set by tools/release_build_oldbase.sh, and this script prints the staged
# binary's floor so an AppImage of a dev-box build says so out loud rather than shipping.
#
# Layout inside:
#   AppRun                    sh: seeds assets/package/ BESIDE the AppImage on the first
#                             launch (so the first-run refusal names a directory that
#                             exists), then execs usr/cz_runtime with the player's args
#   cz_runtime.desktop, cz_runtime.png, .DirIcon
#                             what desktop integration tools read; the icon is OUR art
#                             (tools/release/make_icon.py) — no Capcom byte ships
#   usr/                      the staged bundle minus assets/: cz_runtime, lib/ (SDL2,
#                             ffmpeg, libstdc++, libdxcompiler.so), prewarm.keys,
#                             vs_recipes.bin, kbm_chips/, cz_defaults.env, README,
#                             THIRD_PARTY.md, LICENSE, tools/extract_stfs.py
#
# The runtime binary is fetched once from the type2-runtime project's releases and cached
# under thirdparty/appimage/ with its SHA-256 printed, because the artifact's first 900 KB
# are somebody else's code and the record should say which bytes.
#
# Usage:  tools/release_package_appimage.sh [stageDir] [outDir]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=${1:-$ROOT/dist/CaseZeroRecomp}
OUT=${2:-$ROOT/dist}
NAME=CaseZeroRecomp
APPDIR=$OUT/$NAME.AppDir
IMG=$OUT/$NAME-linux-x86_64.AppImage
RUNTIME=${CZ_APPIMAGE_RUNTIME:-$ROOT/thirdparty/appimage/runtime-x86_64}
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64

fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$STAGE/cz_runtime" ] || fail "no staged bundle at $STAGE — run tools/release_package_linux.sh first"
command -v mksquashfs >/dev/null || fail "mksquashfs not installed (squashfs-tools)"

# THE FLOOR OF WHAT IS BEING WRAPPED, stated before wrapping it. Same computation as the
# packaging script's; an AppImage does not change it and a reader of this output must not
# be left thinking it did.
floor=""
for f in "$STAGE/cz_runtime" "$STAGE"/lib/*.so*; do
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    v=$(objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -t. -k1,1n -k2,2n -u | tail -1)
    [ -n "$v" ] && floor=$(printf '%s\n%s\n' "$floor" "$v" | grep . | sort -t_ -k2,2V | tail -1)
done
# sed, not head: `head -1` closes the pipe after one line, ldd takes SIGPIPE, and under
# `set -o pipefail` that made this assignment exit the script SILENTLY, right here.
here=$(ldd --version 2>/dev/null | sed -n '1{s/.* //;p}')
echo "==> wrapping $STAGE (glibc floor ${floor#GLIBC_}; this machine $here)"
[ "${floor#GLIBC_}" != "$here" ] || echo "    !! the staged binary was built on THIS machine, not the old base — see tools/release_build_oldbase.sh"

if [ ! -f "$RUNTIME" ]; then
    echo "==> fetching the AppImage runtime"
    mkdir -p "$(dirname "$RUNTIME")"
    curl -fL --retry 3 -o "$RUNTIME.part" "$RUNTIME_URL"
    mv "$RUNTIME.part" "$RUNTIME"
fi
chmod +x "$RUNTIME"
# The runtime's own magic: bytes 8..10 of the ELF are "AI\x02" for a type-2 AppImage.
# A wrong download (an HTML error page, say) fails here and not at a player's double-click.
[ "$(dd if="$RUNTIME" bs=1 skip=8 count=3 2>/dev/null)" = "$(printf 'AI\002')" ] \
    || fail "$RUNTIME is not a type-2 AppImage runtime (no AI\\x02 magic at offset 8)"
echo "    runtime $(basename "$RUNTIME") $(( $(stat -c%s "$RUNTIME") / 1024 )) KB  sha256 $(sha256sum "$RUNTIME" | cut -c1-16)…"

echo "==> AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr"
for entry in "$STAGE"/* "$STAGE"/.[!.]*; do
    [ -e "$entry" ] || continue
    case "$(basename "$entry")" in
        assets) continue ;;   # NEVER inside the image — see the header
    esac
    cp -a "$entry" "$APPDIR/usr/"
done
[ ! -e "$APPDIR/usr/assets" ] || fail "assets/ leaked into the AppDir"
cp "$STAGE/assets/package/PUT_YOUR_GAME_HERE.txt" "$APPDIR/usr/PUT_YOUR_GAME_HERE.txt"

cat > "$APPDIR/AppRun" <<'SH'
#!/bin/sh
# AppRun for CaseZeroRecomp. The runtime resolves its data root from $APPIMAGE (the
# directory beside this file) on its own; this script's one job is to seed that
# directory's assets/package/ with the marker file on the first launch, so that the
# first-run refusal points at a directory that exists. CZ_ROOT, if set, wins and nothing
# is seeded.
HERE=$(dirname "$(readlink -f "$0")")
if [ -n "${APPIMAGE:-}" ] && [ -z "${CZ_ROOT:-}" ]; then
    d=$(dirname "$APPIMAGE")/assets/package
    if [ ! -d "$d" ]; then
        mkdir -p "$d" 2>/dev/null && cp "$HERE/usr/PUT_YOUR_GAME_HERE.txt" "$d/" 2>/dev/null
    fi
fi
exec "$HERE/usr/cz_runtime" "$@"
SH
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/cz_runtime.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Dead Rising 2: Case Zero Recomp
Comment=Static recompilation of the Xbox 360 XBLA title, for PC
Exec=cz_runtime
Icon=cz_runtime
Terminal=false
Categories=Game;
DESK
[ -f "$ROOT/tools/release/icon/cz_runtime.png" ] || fail "no tools/release/icon/cz_runtime.png (python3 tools/release/make_icon.py)"
cp "$ROOT/tools/release/icon/cz_runtime.png" "$APPDIR/cz_runtime.png"
ln -sf cz_runtime.png "$APPDIR/.DirIcon"

echo "==> squashfs + runtime -> $(basename "$IMG")"
SQ=$OUT/$NAME.squashfs
rm -f "$SQ" "$IMG"
# zstd: the type2 runtime's squashfuse reads it, and it is what the .tar.zst uses too.
# -root-owned: a squashfs carrying the build user's uid is a squashfs some other machine
# shows as owned by nobody. -no-xattrs: SELinux labels from this box mean nothing there.
mksquashfs "$APPDIR" "$SQ" -root-owned -noappend -no-xattrs -comp zstd -Xcompression-level 19 -quiet
cat "$RUNTIME" "$SQ" > "$IMG"
rm -f "$SQ"
chmod +x "$IMG"
(cd "$OUT" && sha256sum "$(basename "$IMG")" > "$(basename "$IMG").sha256")
printf '    %s  %s MB\n' "$(basename "$IMG")" "$(( $(stat -c%s "$IMG") / 1024 / 1024 ))"
sed 's/^/    /' "$IMG.sha256"

# SELF-CHECKS, run here because they need no container and a wrapped image that cannot
# start should fail the person packaging it, not the person downloading it.
echo "==> self-check: --appimage-extract-and-run (no FUSE) --smoke"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$IMG" "$T/cz.AppImage"
smoke=$( (cd "$T" && ./cz.AppImage --appimage-extract-and-run --smoke 2>&1) | tail -1)
echo "    $smoke"
echo "$smoke" | grep -q "OK: every generated symbol resolved" || fail "the packaged binary did not pass --smoke through the AppImage"
echo "==> self-check: the data root resolves BESIDE the image, and assets/package/ is seeded"
# CZ_LAUNCHER=0 CZ_NO_WINDOW=1, or the shipped cz_defaults.env opens the launcher window
# on this desktop and the check waits for a player who is not there (it did, part 104).
rootline=$( (cd "$T" && CZ_LAUNCHER=0 CZ_NO_WINDOW=1 CZ_NO_AUDIO_OUT=1 timeout 30 ./cz.AppImage --appimage-extract-and-run 2>&1) | grep -m1 '^\[paths\] root' || true)
echo "    $rootline"
echo "$rootline" | grep -q "root $T (appimage)" || fail "the root did not resolve beside the AppImage (host_paths step 1b)"
[ -f "$T/assets/package/PUT_YOUR_GAME_HERE.txt" ] || fail "AppRun did not seed assets/package/ beside the image"
echo "    $T/assets/package/PUT_YOUR_GAME_HERE.txt seeded"
if [ -e /dev/fuse ]; then
    echo "==> self-check: the FUSE mount path (what a player's double-click does)"
    fsmoke=$( (cd "$T" && ./cz.AppImage --smoke 2>&1) | tail -1)
    echo "    $fsmoke"
    echo "$fsmoke" | grep -q "OK: every generated symbol resolved" || fail "the FUSE-mounted AppImage did not pass --smoke"
else
    echo "==> (no /dev/fuse here: the FUSE mount path is not exercised on this machine)"
fi
rm -rf "$APPDIR"

cat <<MSG

==> NEXT: the gate, in a container OLDER than the build base (Rocky 9 = glibc 2.34)
    tools/release_gate_clean_container.sh $IMG quay.io/rockylinux/rockylinux:9-minimal
MSG
